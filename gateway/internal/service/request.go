package service

import (
	"context"
	"database/sql"
	"errors"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

// RequestNotifier 在提交后推送快照；断线时由 request.get 恢复。
type RequestNotifier interface {
	NotifyRequest(deviceID, event string, request contracts.Request)
}

// RequestConflict 是内部排查信息，不扩展固件的共享 Error 字段。
type RequestConflict struct {
	Blockers     []storage.RequestBlocker
	TargetStatus string
}

func (*RequestConflict) Error() string { return "设备或目标会话存在未结束请求" }

// RequestService 实现 #14 的准备、原文、确认投递与执行事实持久化。
// 锁覆盖提交与通知，避免同一进程中旧通知越过取消/过期事件。
type RequestService struct {
	mu         sync.Mutex
	repos      *storage.Repositories
	requests   storage.RequestRepo
	executions storage.ExecutionRepo
	dispatcher TaskDispatcher
	cfg        *config.Config
	notifier   RequestNotifier
	now        func() time.Time
}

// NewRequestService 构造持久化录音请求服务。
func NewRequestService(repos *storage.Repositories, cfg *config.Config, notifier RequestNotifier) *RequestService {
	dispatcher, _ := notifier.(TaskDispatcher)
	return &RequestService{repos: repos, cfg: cfg, notifier: notifier, dispatcher: dispatcher, now: time.Now}
}

// Prepare 固定已绑定 Desktop 的会话、thread/project/title；重试不延长超时。
func (s *RequestService) Prepare(ctx context.Context, device domain.DeviceID, id, session string) (contracts.Request, error) {
	if !domain.ValidateID(id) || !domain.ValidateID(session) {
		return contracts.Request{}, domain.ErrInvalidArgument("请求或会话标识无效")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.expireLocked(ctx); err != nil {
		return contracts.Request{}, err
	}
	var result contracts.Request
	err := s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		binding, err := s.repos.Bindings.GetByDevice(ctx, tx, string(device))
		if errors.Is(err, sql.ErrNoRows) {
			return domain.ErrPermissionDenied("设备未绑定 Desktop")
		}
		if err != nil {
			return err
		}
		if err := validateBindingOwner(ctx, tx, s.cfg, binding); err != nil {
			return err
		}
		existing, err := s.requests.Get(ctx, tx, id)
		if err == nil {
			if existing.DeviceID != string(device) || existing.DesktopID != binding.DesktopClientID || existing.AgentSessionID != session {
				return domain.ErrConflict("request_id 已固定其他目标")
			}
			result = existing.Request
			return nil
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
		target, err := s.repos.Sessions.Get(ctx, tx, binding.DesktopClientID, session)
		if errors.Is(err, sql.ErrNoRows) {
			return domain.ErrNotFound("目标会话不存在")
		}
		if err != nil {
			return err
		}
		blockers, err := s.requests.Blockers(ctx, tx, string(device), binding.DesktopClientID, session)
		if err != nil {
			return err
		}
		if len(blockers) > 0 || target.Status == "running" || target.Status == "unknown" {
			return domain.WrapError(domain.CodeConflict, "设备或目标会话存在未结束请求",
				&RequestConflict{Blockers: blockers, TargetStatus: target.Status})
		}
		r := &storage.RequestRow{
			Request:  contracts.Request{RequestID: id, AgentSessionID: session, TargetTitle: target.Title, Status: "awaiting_text"},
			DeviceID: string(device), DesktopID: binding.DesktopClientID, ThreadID: target.ThreadID, ProjectID: target.ProjectID,
			ExpiresAt: s.now().Add(120 * time.Second).UnixMilli(),
		}
		if err := s.requests.Insert(ctx, tx, r); err != nil {
			return err
		}
		result = r.Request
		return nil
	})
	return result, err
}

// Propose 原样保存最终 ASR；显式 request_id 无论联调开关如何均精确定位，不回退其他请求。
func (s *RequestService) Propose(ctx context.Context, device domain.DeviceID, id, agent, voiceSession, text string) (contracts.Request, error) {
	return s.propose(ctx, device, id, agent, voiceSession, text, false, "")
}

// ProposeImplicit 在明确开启的联调模式中，事务内定位唯一待处理请求；不新建或改投目标。
func (s *RequestService) ProposeImplicit(ctx context.Context, device domain.DeviceID, agent, voiceSession, text string) (contracts.Request, error) {
	if !s.cfg.MCP.AllowImplicitRequestID {
		return contracts.Request{}, domain.ErrPermissionDenied("MCP 请求自动关联未开启")
	}
	return s.propose(ctx, device, "", agent, voiceSession, text, true, "")
}

// ProposeForSubject 对账号范围的 MCP 凭据，在保存原文的同一事务内核对设备账号。
func (s *RequestService) ProposeForSubject(ctx context.Context, device domain.DeviceID, subject domain.Subject, id, agent, voiceSession, text string) (contracts.Request, error) {
	if !domain.ValidateSubject(string(subject)) {
		return contracts.Request{}, domain.ErrPermissionDenied("MCP 账号范围无效")
	}
	return s.propose(ctx, device, id, agent, voiceSession, text, false, subject)
}

// ProposeImplicitForSubject 仅在已有演示开关开启时，关联该账号设备的唯一请求。
func (s *RequestService) ProposeImplicitForSubject(ctx context.Context, device domain.DeviceID, subject domain.Subject, agent, voiceSession, text string) (contracts.Request, error) {
	if !s.cfg.MCP.AllowImplicitRequestID {
		return contracts.Request{}, domain.ErrPermissionDenied("MCP 请求自动关联未开启")
	}
	if !domain.ValidateSubject(string(subject)) {
		return contracts.Request{}, domain.ErrPermissionDenied("MCP 账号范围无效")
	}
	return s.propose(ctx, device, "", agent, voiceSession, text, true, subject)
}

func (s *RequestService) propose(ctx context.Context, device domain.DeviceID, id, agent, voiceSession, text string, implicit bool, subject domain.Subject) (contracts.Request, error) {
	if (!implicit && !domain.ValidateID(id)) || !domain.ValidateID(agent) || !domain.ValidateID(voiceSession) {
		return contracts.Request{}, domain.ErrInvalidArgument("缺少本轮请求或灵矽上下文")
	}
	if !utf8.ValidString(text) || strings.TrimSpace(text) == "" || utf8.RuneCountInString(text) > 16000 || len(text) > 16*1024 ||
		(s.cfg.Limits.DraftMaxCodepoints > 0 && utf8.RuneCountInString(text) > s.cfg.Limits.DraftMaxCodepoints) ||
		(s.cfg.Limits.DraftMaxUTF8Bytes > 0 && len(text) > s.cfg.Limits.DraftMaxUTF8Bytes) {
		return contracts.Request{}, domain.ErrInvalidArgument("最终识别文字为空、无效或超过长度限制")
	}
	selectRequest := func(tx *sql.Tx) (*storage.RequestRow, error) {
		if subject != "" {
			registered, err := s.repos.Devices.Get(ctx, tx, string(device))
			if errors.Is(err, sql.ErrNoRows) {
				return nil, domain.ErrPermissionDenied("MCP 设备尚未登记")
			}
			if err != nil {
				return nil, err
			}
			current, valid := s.cfg.DeviceSubjectForCredential(registered.CredentialKey)
			if !valid || current != subject || registered.Subject != string(subject) {
				return nil, domain.ErrPermissionDenied("设备与 MCP 凭据不属于同一账号")
			}
		}
		if !implicit {
			return s.requests.Get(ctx, tx, id)
		}
		ids, err := s.requests.PendingIDsForDevice(ctx, tx, string(device), s.now().UnixMilli())
		if err != nil {
			return nil, err
		}
		if len(ids) == 0 {
			return nil, domain.ErrNotFound("没有可关联的待处理请求，请先 request.prepare")
		}
		if len(ids) != 1 {
			return nil, domain.ErrConflict("设备存在多个可关联请求，不能自动选择")
		}
		return s.requests.Get(ctx, tx, ids[0])
	}
	return s.changeSelected(ctx, device, selectRequest, func(tx *sql.Tx, r *storage.RequestRow) (string, error) {
		if r.Status == "cancelled" || r.Expired {
			return "", domain.ErrConflict("请求已取消或过期")
		}
		if r.Text != nil {
			if *r.Text != text || r.LinxAgentID != agent || r.LinxSessionID != voiceSession {
				return "", domain.ErrConflict("请求文字或灵矽来源已固定")
			}
			return "", nil
		}
		if r.Status != "awaiting_text" {
			return "", domain.ErrConflict("请求不再接收文字")
		}
		// 目录移除或更换 thread/project 后不能接受新的草稿，也不能改投新目标。
		target, err := s.repos.Sessions.Get(ctx, tx, r.DesktopID, r.AgentSessionID)
		if errors.Is(err, sql.ErrNoRows) {
			return "", domain.ErrPermissionDenied("原目标已不在授权目录")
		}
		if err != nil {
			return "", err
		}
		if target.ThreadID != r.ThreadID || target.ProjectID != r.ProjectID {
			return "", domain.ErrConflict("原目标路由已变化")
		}
		r.Text, r.Status = &text, "awaiting_confirmation"
		r.LinxAgentID, r.LinxSessionID = agent, voiceSession
		r.ExpiresAt = s.now().Add(300 * time.Second).UnixMilli()
		return "request.proposed", nil
	})
}

// Get 只返回 Gateway 已保存快照，不向 Desktop 查询执行结果。
func (s *RequestService) Get(ctx context.Context, device domain.DeviceID, id string) (contracts.Request, error) {
	request, err := s.change(ctx, device, id, func(*sql.Tx, *storage.RequestRow) (string, error) { return "", nil })
	if domain.CodeOf(err) == domain.CodeNotFound {
		return request, domain.WrapError(domain.CodeNotFound, domain.RequestRecoveryMessage, err)
	}
	return request, err
}

// Cancel 只取消确认前请求；终态保留原文及 request_id。
func (s *RequestService) Cancel(ctx context.Context, device domain.DeviceID, id string) (contracts.Request, error) {
	return s.change(ctx, device, id, func(_ *sql.Tx, r *storage.RequestRow) (string, error) {
		if r.Status == "cancelled" {
			return "", nil
		}
		if r.Status != "awaiting_text" && r.Status != "awaiting_confirmation" {
			return "", domain.ErrConflict("请求已结束或已确认，不能取消")
		}
		r.Status = "cancelled"
		return "request.updated", nil
	})
}

func (s *RequestService) change(ctx context.Context, device domain.DeviceID, id string, fn func(*sql.Tx, *storage.RequestRow) (string, error)) (contracts.Request, error) {
	return s.changeSelected(ctx, device, func(tx *sql.Tx) (*storage.RequestRow, error) { return s.requests.Get(ctx, tx, id) }, fn)
}

func (s *RequestService) changeSelected(ctx context.Context, device domain.DeviceID, selectRequest func(*sql.Tx) (*storage.RequestRow, error), fn func(*sql.Tx, *storage.RequestRow) (string, error)) (contracts.Request, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.expireLocked(ctx); err != nil {
		return contracts.Request{}, err
	}
	var result contracts.Request
	var event string
	err := s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		r, err := selectRequest(tx)
		if errors.Is(err, sql.ErrNoRows) {
			return domain.ErrNotFound("请求不存在")
		}
		if err != nil {
			return err
		}
		binding, err := s.repos.Bindings.GetByDevice(ctx, tx, string(device))
		if errors.Is(err, sql.ErrNoRows) || (err == nil && (r.DeviceID != string(device) || r.DesktopID != binding.DesktopClientID)) {
			return domain.ErrPermissionDenied("无权访问此请求")
		}
		if err != nil {
			return err
		}
		if err := validateBindingOwner(ctx, tx, s.cfg, binding); err != nil {
			return err
		}
		event, err = fn(tx, r)
		if err != nil {
			return err
		}
		if event != "" {
			if err := s.requests.Save(ctx, tx, r); err != nil {
				return err
			}
		}
		result = r.Request
		return nil
	})
	if err == nil && event != "" {
		s.notifier.NotifyRequest(string(device), event, result)
	}
	return result, err
}

// Expire 供启动、周期任务及每次业务操作调用；以持久化截止时间为准。
func (s *RequestService) Expire(ctx context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.expireLocked(ctx)
}

func (s *RequestService) expireLocked(ctx context.Context) error {
	var expired []*storage.RequestRow
	err := s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		ids, err := s.requests.ExpiredIDs(ctx, tx, s.now().UnixMilli())
		if err != nil {
			return err
		}
		for _, id := range ids {
			r, err := s.requests.Get(ctx, tx, id)
			if err != nil {
				return err
			}
			message := "未收到文字"
			if r.Status == "awaiting_confirmation" {
				message = "请求确认已超时"
			}
			r.Status, r.Error = "failed", &contracts.Error{Code: string(domain.CodeDependencyUnavailable), Message: message}
			r.Expired = true
			if err := s.requests.Save(ctx, tx, r); err != nil {
				return err
			}
			binding, err := s.repos.Bindings.GetByDevice(ctx, tx, r.DeviceID)
			if err != nil && !errors.Is(err, sql.ErrNoRows) {
				return err
			}
			if err == nil && binding.DesktopClientID == r.DesktopID && validateBindingOwner(ctx, tx, s.cfg, binding) == nil {
				expired = append(expired, r)
			}
		}
		return nil
	})
	if err == nil {
		for _, r := range expired {
			s.notifier.NotifyRequest(r.DeviceID, "request.updated", r.Request)
		}
	}
	return err
}
