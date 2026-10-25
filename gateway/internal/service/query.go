package service

import (
	"encoding/json"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

// Query 固定认证来源、连接代次、两端包 ID 和原始查询参数。
type Query struct {
	ID, DeviceID, DesktopID, FirmwareID, Operation string
	DeviceGeneration, DesktopGeneration            uint64
	Payload                                        json.RawMessage
	Offset                                         int64
	StartedAt                                      time.Time
	Deadline                                       time.Time
	EndReason                                      string // 固定内部结果分类，仅用于联调，不携带对端文案。
}

type queryEntry struct {
	query Query
	timer *time.Timer
}

// QueryRouter 只管理有界、短期读取关联；不重试、不保存正文，不处理编码任务。
type QueryRouter struct {
	mu                  sync.Mutex
	pending             map[string]*queryEntry
	byFirmware          map[string]string
	counts              map[string]int
	validator           *contracts.Validator
	timeout             time.Duration
	maxTotal, maxDevice int
	reply               func(Query, json.RawMessage, error)
}

func NewQueryRouter(v *contracts.Validator, timeout time.Duration, maxTotal, maxDevice int, reply func(Query, json.RawMessage, error)) *QueryRouter {
	if timeout <= 0 {
		timeout = 15 * time.Second
	}
	if maxTotal <= 0 {
		maxTotal = 128
	}
	if maxDevice <= 0 {
		maxDevice = 4
	}
	return &QueryRouter{pending: map[string]*queryEntry{}, byFirmware: map[string]string{}, counts: map[string]int{}, validator: v, timeout: timeout, maxTotal: maxTotal, maxDevice: maxDevice, reply: reply}
}

func firmwareKey(q Query) string { return q.DeviceID + ":" + q.FirmwareID }

// Begin 先保存关联再发送。原固件 ID 在应答前不能复用于另一条查询。
func (r *QueryRouter) Begin(q Query) (Query, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if q.Operation != "messages.list" && q.Operation != "message.content.get" {
		return Query{}, domain.ErrInvalidArgument("查询操作不合法")
	}
	if _, ok := r.byFirmware[firmwareKey(q)]; ok {
		return Query{}, domain.ErrConflict("查询 ID 正在使用")
	}
	if len(r.pending) >= r.maxTotal || r.counts[q.DeviceID] >= r.maxDevice {
		return Query{}, domain.ErrResourceExhausted("在途查询数达到上限")
	}
	q.ID = contracts.NewMessageID("qry_")
	q.StartedAt = time.Now()
	q.Deadline = q.StartedAt.Add(r.timeout)
	q.Payload = append(json.RawMessage(nil), q.Payload...)
	e := &queryEntry{query: q}
	r.pending[q.ID] = e
	r.byFirmware[firmwareKey(q)] = q.ID
	r.counts[q.DeviceID]++
	e.timer = time.AfterFunc(r.timeout, func() { r.fail(q.ID, domain.ErrDependencyUnavailable("Desktop 查询超时"), "timeout") })
	return q, nil
}

// Complete 只接收固定 Desktop 及代次的原操作应答。无效应答保留关联到超时。
func (r *QueryRouter) Complete(desktopID string, generation uint64, raw []byte) error {
	var envelope struct {
		Payload struct {
			ReplyTo string           `json:"reply_to_message_id"`
			OK      bool             `json:"ok"`
			Data    json.RawMessage  `json:"data"`
			Error   *contracts.Error `json:"error"`
		} `json:"payload"`
	}
	if err := json.Unmarshal(raw, &envelope); err != nil {
		return domain.ErrInvalidArgument("查询应答不是合法 JSON")
	}
	r.mu.Lock()
	e := r.pending[envelope.Payload.ReplyTo]
	if e == nil || e.query.DesktopID != desktopID || e.query.DesktopGeneration != generation {
		r.mu.Unlock()
		return domain.ErrNotFound("查询已结束或来源不匹配")
	}
	q := e.query
	if !time.Now().Before(q.Deadline) {
		r.removeLocked(q.ID)
		r.mu.Unlock()
		q.EndReason = "timeout"
		r.reply(q, nil, domain.ErrDependencyUnavailable("Desktop 查询超时"))
		return domain.ErrNotFound("查询已过期")
	}
	if err := r.validator.ValidateDesktopQueryResult(q.Operation, raw); err != nil {
		r.mu.Unlock()
		return domain.ErrInvalidArgument("查询应答不符合原操作")
	}
	if envelope.Payload.OK && q.Operation == "message.content.get" {
		var chunk struct {
			Text string `json:"text"`
			Next *int64 `json:"next_offset"`
		}
		if err := json.Unmarshal(envelope.Payload.Data, &chunk); err != nil || !utf8.ValidString(chunk.Text) || len(chunk.Text) > 1024 ||
			(chunk.Next != nil && (len(chunk.Text) == 0 || *chunk.Next <= q.Offset || *chunk.Next-q.Offset != int64(len(chunk.Text)))) {
			r.mu.Unlock()
			return domain.ErrInvalidArgument("正文块大小或字节偏移不合法")
		}
	}
	r.removeLocked(envelope.Payload.ReplyTo)
	r.mu.Unlock()
	q.EndReason = "desktop_result"
	if !envelope.Payload.OK {
		r.reply(q, nil, domain.NewError(domain.Code(envelope.Payload.Error.Code), envelope.Payload.Error.Message))
	} else {
		r.reply(q, envelope.Payload.Data, nil)
	}
	return nil
}

func (r *QueryRouter) removeLocked(id string) {
	e := r.pending[id]
	if e == nil {
		return
	}
	if e.timer != nil {
		e.timer.Stop()
	}
	delete(r.pending, id)
	delete(r.byFirmware, firmwareKey(e.query))
	r.counts[e.query.DeviceID]--
	if r.counts[e.query.DeviceID] == 0 {
		delete(r.counts, e.query.DeviceID)
	}
}

func (r *QueryRouter) Fail(id string, err error) {
	r.fail(id, err, "forward_failed")
}

func (r *QueryRouter) fail(id string, err error, reason string) {
	r.mu.Lock()
	e := r.pending[id]
	if e == nil {
		r.mu.Unlock()
		return
	}
	r.removeLocked(id)
	r.mu.Unlock()
	q := e.query
	q.EndReason = reason
	r.reply(q, nil, err)
}

// Disconnect 按连接代次回收关联；设备离线不向其新连接投递旧应答。
func (r *QueryRouter) Disconnect(device bool, id string, generation uint64) {
	r.mu.Lock()
	var cancelled []Query
	for key, e := range r.pending {
		q := e.query
		if (device && q.DeviceID == id && q.DeviceGeneration == generation) || (!device && q.DesktopID == id && q.DesktopGeneration == generation) {
			cancelled = append(cancelled, q)
			r.removeLocked(key)
		}
	}
	r.mu.Unlock()
	if !device {
		for _, q := range cancelled {
			q.EndReason = "desktop_disconnected"
			r.reply(q, nil, domain.ErrDependencyUnavailable("Desktop 已断开连接"))
		}
	}
}
