package service

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

// 保持与已部署版本的持久错误文案一致，用于识别旧库中误设 terminal 的记录。
const incompleteCompletionMessage = "完成事件缺少一致的成功结果，不能确认任务成功"

// TaskDelivery 在落盘前准备已校验的帧；Send 只能在确认事务成功后调用一次。
type TaskDelivery struct {
	MessageID  string
	Sequence   int64
	Generation uint64
	Send       func() error
}

// TaskDispatcher 不自动重试任务；连接变化、发送失败均不能证明未执行。
type TaskDispatcher interface {
	PrepareDispatch(string, contracts.TaskDispatchPayload) (TaskDelivery, error)
}

func (s *RequestService) ownedRequest(ctx context.Context, tx *sql.Tx, device domain.DeviceID, id string) (*storage.RequestRow, error) {
	r, err := s.requests.Get(ctx, tx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, domain.ErrNotFound("请求不存在")
	}
	if err != nil {
		return nil, err
	}
	binding, err := s.repos.Bindings.GetByDevice(ctx, tx, string(device))
	if errors.Is(err, sql.ErrNoRows) || (err == nil && (r.DeviceID != string(device) || r.DesktopID != binding.DesktopClientID)) {
		return nil, domain.ErrPermissionDenied("无权访问此请求")
	}
	if err != nil {
		return nil, err
	}
	if err := validateBindingOwner(ctx, tx, s.cfg, binding); err != nil {
		return nil, err
	}
	return r, nil
}

// Confirm 固定唯一 task/投递身份；重复确认（含重启后）仅返回已有快照。
// 未落盘绝不发送；落盘后即使发送失败也不回到待确认或自动重投。
func (s *RequestService) Confirm(ctx context.Context, device domain.DeviceID, id string) (contracts.Request, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.expireLocked(ctx); err != nil {
		return contracts.Request{}, err
	}
	var request *storage.RequestRow
	var delivery TaskDelivery
	err := s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		var err error
		request, err = s.ownedRequest(ctx, tx, device, id)
		if err != nil {
			return err
		}
		if _, err := s.executions.ByRequest(ctx, tx, id); err == nil {
			return nil
		} else if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		if request.Status != "awaiting_confirmation" || request.Text == nil {
			return domain.ErrConflict("请求不在待确认状态")
		}
		target, err := s.repos.Sessions.Get(ctx, tx, request.DesktopID, request.AgentSessionID)
		if errors.Is(err, sql.ErrNoRows) {
			return domain.ErrPermissionDenied("原目标已不在授权目录")
		}
		if err != nil {
			return err
		}
		if target.ThreadID != request.ThreadID || target.ProjectID != request.ProjectID {
			return domain.ErrConflict("原目标路由已变化")
		}
		if target.Status == "running" || target.Status == "unknown" {
			return domain.ErrConflict("目标会话当前不可接收追问")
		}
		if s.dispatcher == nil {
			return domain.ErrDependencyUnavailable("任务投递服务未配置")
		}
		payload := contracts.TaskDispatchPayload{
			TaskID: contracts.NewMessageID("task_"), ProjectID: request.ProjectID, Agent: target.Agent,
			Capabilities: []string{}, Prompt: *request.Text, AcceptanceCriteria: []string{}, PolicyVersion: "request-confirm/v1",
			Continuation: contracts.Continuation{AgentSessionID: request.AgentSessionID, ThreadID: request.ThreadID},
		}
		delivery, err = s.dispatcher.PrepareDispatch(request.DesktopID, payload)
		if err != nil {
			return err
		}
		if delivery.Send == nil {
			return domain.ErrInternal("任务投递未准备完成")
		}
		if err := s.executions.Insert(ctx, tx, &storage.ExecutionRow{RequestID: id, TaskID: payload.TaskID,
			MessageID: delivery.MessageID, Sequence: delivery.Sequence, Generation: delivery.Generation,
			ConfirmedAt: s.now().UnixMilli(), LastEvent: -1}); err != nil {
			return err
		}
		request.Status, request.Error = "sending", nil
		return s.requests.Save(ctx, tx, request)
	})
	if err != nil {
		return contracts.Request{}, err
	}
	if delivery.Send != nil {
		if err := delivery.Send(); err != nil {
			request.Status, request.Error = "unknown", executionUnknown("任务发送结果不确定，请核对原任务；不会自动重发")
			if err := s.requests.Save(ctx, s.repos.DB(), request); err != nil {
				return contracts.Request{}, err
			}
		}
		s.notifier.NotifyRequest(request.DeviceID, "request.updated", request.Request)
	}
	return request.Request, nil
}

// ApplyTask 接受当前认证 Desktop 的执行事实；不读取设备的“最新请求”。
func (s *RequestService) ApplyTask(ctx context.Context, desktop domain.DesktopClientID, generation uint64, msg contracts.DesktopMessage) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	var request *storage.RequestRow
	changed := false
	err := s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		var key struct {
			TaskID string `json:"task_id"`
		}
		if err := json.Unmarshal(msg.Payload, &key); err != nil {
			return err
		}
		execution, err := s.executions.ByTask(ctx, tx, key.TaskID)
		if errors.Is(err, sql.ErrNoRows) {
			return domain.ErrNotFound("任务不存在")
		}
		if err != nil {
			return err
		}
		request, err = s.requests.Get(ctx, tx, execution.RequestID)
		if err != nil {
			return err
		}
		if request.DesktopID != string(desktop) {
			return domain.ErrPermissionDenied("任务不属于当前 Desktop")
		}
		if _, err := s.ownedRequest(ctx, tx, domain.DeviceID(request.DeviceID), request.RequestID); err != nil {
			return err
		}
		previous, _ := json.Marshal(request.Request)
		if msg.Type == "task.ack" {
			if err := s.applyAck(msg.Payload, request, execution); err != nil {
				return err
			}
		} else {
			applied, err := s.applyEvent(ctx, tx, msg, request, execution)
			if err != nil || !applied {
				return err
			}
		}
		execution.Generation = generation
		if err := s.executions.Save(ctx, tx, execution); err != nil {
			return err
		}
		if err := s.requests.Save(ctx, tx, request); err != nil {
			return err
		}
		current, _ := json.Marshal(request.Request)
		changed = string(previous) != string(current)
		return nil
	})
	if err == nil && changed {
		s.notifier.NotifyRequest(request.DeviceID, "request.updated", request.Request)
	}
	return err
}

func (s *RequestService) applyAck(raw json.RawMessage, request *storage.RequestRow, execution *storage.ExecutionRow) error {
	var ack contracts.TaskAckPayload
	if err := json.Unmarshal(raw, &ack); err != nil {
		return err
	}
	if ack.MessageID != execution.MessageID || ack.Sequence != execution.Sequence {
		return domain.ErrConflict("ACK 与原投递不匹配")
	}
	switch ack.Disposition {
	case "accepted", "duplicate":
		if execution.Ack == "rejected" || execution.Ack == "expired" {
			return domain.ErrConflict("ACK 与已保存的拒绝事实冲突")
		}
	case "rejected", "expired":
		if (execution.Ack == "accepted" || execution.Ack == "duplicate") && !execution.Terminal {
			return domain.ErrConflict("拒绝 ACK 与已接受投递的事实冲突")
		}
		if request.Status == "completed" || request.Status == "running" || execution.TurnID != "" {
			return domain.ErrConflict("拒绝 ACK 不能覆盖已启动任务")
		}
		if !execution.Terminal {
			request.Status, request.Error = "failed", &contracts.Error{Code: "DEPENDENCY_UNAVAILABLE", Message: "Desktop 未接受任务投递"}
			execution.Terminal = true
		}
	default:
		return domain.ErrInvalidArgument("未知 ACK 结果")
	}
	execution.Ack = ack.Disposition
	return nil
}

func (s *RequestService) applyEvent(ctx context.Context, tx *sql.Tx, msg contracts.DesktopMessage, request *storage.RequestRow, execution *storage.ExecutionRow) (bool, error) {
	var event contracts.TaskEventPayload
	if err := json.Unmarshal(msg.Payload, &event); err != nil {
		return false, err
	}
	// 对 JSON 对象排序后取指纹，重复事件不受字段顺序或空白影响。
	var canonical any
	if err := json.Unmarshal(msg.Payload, &canonical); err != nil {
		return false, err
	}
	raw, _ := json.Marshal(canonical)
	digest := sha256.Sum256(append([]byte(msg.Type+":"), raw...))
	hash := hex.EncodeToString(digest[:])
	if previous, err := s.executions.EventHash(ctx, tx, event.EventID); err == nil {
		if previous != hash {
			return false, domain.ErrConflict("event_id 已用于其他执行事实")
		}
		return false, nil
	} else if !errors.Is(err, sql.ErrNoRows) {
		return false, err
	}
	// 旧版本曾将缺少成功结果的 completed 错标为 terminal。只为这个
	// 可辨识的 unknown 接受原任务后续终态事实；真实 interrupted 等终态仍不可覆盖。
	pendingCompletion := request.Status == "unknown" && request.Error != nil &&
		request.Error.Code == "DEPENDENCY_UNAVAILABLE" && request.Error.Message == incompleteCompletionMessage
	if (execution.Terminal && !pendingCompletion) || event.EventSequence <= execution.LastEvent {
		return false, domain.ErrConflict("任务事件已结束或序号过期")
	}
	if pendingCompletion && msg.Type != "task.completed" && msg.Type != "task.failed" && msg.Type != "task.cancelled" && msg.Type != "task.interrupted" {
		return false, domain.ErrConflict("完成结果待核实，只接受原任务后续终态事实")
	}
	if event.ThreadID != nil && *event.ThreadID != request.ThreadID {
		return false, domain.ErrConflict("执行事件不属于固定 thread")
	}
	if execution.TurnID != "" && (event.TurnID == nil || *event.TurnID != execution.TurnID) {
		return false, domain.ErrConflict("执行事件不属于原 turn")
	}
	identity := event.ThreadID != nil && event.TurnID != nil && *event.TurnID != ""
	startupFailure := (msg.Type == "task.failed" || msg.Type == "task.interrupted") && execution.TurnID == "" && event.TurnID == nil
	if (event.ThreadID == nil) != (event.TurnID == nil) && !startupFailure {
		return false, domain.ErrConflict("执行标识不完整")
	}
	var result struct {
		Status string `json:"status"`
	}
	if len(event.Result) > 0 {
		if err := json.Unmarshal(event.Result, &result); err != nil {
			return false, err
		}
	}
	valid := true
	switch msg.Type {
	case "task.started":
		valid = identity && event.State == "running" && event.Error == nil && result.Status == ""
		request.Status, request.Error = "running", nil
	case "task.progress":
		valid = identity && execution.TurnID != "" && event.State == "running" && event.Error == nil && result.Status == ""
	case "task.completed":
		valid = identity && event.State == "succeeded" && event.Error == nil
		execution.Terminal = result.Status == "succeeded"
		if execution.Terminal {
			request.Status, request.Error = "completed", nil
		} else {
			request.Status, request.Error = "unknown", executionUnknown(incompleteCompletionMessage)
		}
	case "task.failed":
		valid = event.State == "failed" && event.Error != nil && (result.Status == "" || result.Status == "failed" || result.Status == "rejected")
		request.Status, request.Error = "failed", &contracts.Error{Code: "DEPENDENCY_UNAVAILABLE", Message: "Desktop 报告任务执行失败，请在电脑端查看详情"}
		execution.Terminal = true
	case "task.interrupted":
		valid = event.State == "interrupted" && (result.Status == "" || result.Status == "interrupted")
		request.Status, request.Error = "unknown", executionUnknown("Desktop 报告任务中断，结果未确认；不会自动重发")
		execution.Terminal = true
	case "task.cancelled":
		valid = identity && event.State == "cancelled" && (result.Status == "" || result.Status == "cancelled")
		request.Status, request.Error = "cancelled", nil
		execution.Terminal = true
	case "task.approval_required":
		valid = identity && event.State == "approval_required" && result.Status == ""
		request.Status, request.Error = "unknown", executionUnknown("任务需要在电脑端处理审批，当前入口不自动授权")
	default:
		valid = false
	}
	if !valid {
		return false, domain.ErrConflict("任务事件类型、状态或结果不一致")
	}
	if identity {
		execution.TurnID = *event.TurnID
	}
	if result.Status != "" {
		value := string(event.Result)
		execution.Result = &value
	}
	execution.LastEvent = event.EventSequence
	if err := s.executions.AddEvent(ctx, tx, execution.TaskID, event.EventID, event.EventSequence, hash); err != nil {
		return false, err
	}
	return true, nil
}

func executionUnknown(message string) *contracts.Error {
	return &contracts.Error{Code: "DEPENDENCY_UNAVAILABLE", Message: message}
}

// Recover 启动时将未确认终态的投递收敛为 unknown，保留身份，不重放。
func (s *RequestService) Recover(ctx context.Context) error {
	return s.markUncertain(ctx, "", 0)
}

// DesktopDisconnected 只影响该断开代次；旧连接退出不能覆盖新连接的事实。
func (s *RequestService) DesktopDisconnected(ctx context.Context, desktop string, generation uint64) error {
	return s.markUncertain(ctx, desktop, generation)
}

func (s *RequestService) markUncertain(ctx context.Context, desktop string, generation uint64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	var notices []*storage.RequestRow
	err := s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		ids, err := s.executions.ActiveIDs(ctx, tx, desktop, generation)
		if err != nil {
			return err
		}
		for _, id := range ids {
			r, err := s.requests.Get(ctx, tx, id)
			if err != nil {
				return err
			}
			r.Status, r.Error = "unknown", executionUnknown("连接中断或 Gateway 重启，执行结果未确认；不会自动重发")
			if err := s.requests.Save(ctx, tx, r); err != nil {
				return err
			}
			if _, err := s.ownedRequest(ctx, tx, domain.DeviceID(r.DeviceID), id); err == nil {
				notices = append(notices, r)
			}
		}
		return nil
	})
	if err == nil {
		for _, r := range notices {
			s.notifier.NotifyRequest(r.DeviceID, "request.updated", r.Request)
		}
	}
	return err
}
