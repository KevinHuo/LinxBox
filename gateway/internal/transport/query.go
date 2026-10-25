package transport

import (
	"context"
	"encoding/json"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
)

// forwardQuery 校验设备归属后只向固定 Desktop 当前连接转发读取请求。
func (s *Server) forwardQuery(ctx context.Context, source *lifecycle.Connection, p Principal, msg contracts.FirmwareMessage) error {
	var input struct {
		SessionID string `json:"agent_session_id"`
		Offset    int64  `json:"offset"`
	}
	if err := json.Unmarshal(msg.Payload, &input); err != nil {
		s.respondFirmware(source, msg.Type, msg.ID, nil, domain.ErrInvalidArgument("查询参数超出支持范围"))
		return nil
	}
	desktopID, err := s.sessions.DesktopForSession(ctx, p.DeviceID, input.SessionID)
	if err != nil {
		s.respondFirmware(source, msg.Type, msg.ID, nil, err)
		return nil
	}
	destination := s.conns.Desktop(desktopID)
	if destination == nil {
		s.respondFirmware(source, msg.Type, msg.ID, nil, domain.ErrDependencyUnavailable("Desktop 未连接"))
		return nil
	}
	q, err := s.queries.Begin(service.Query{DeviceID: string(p.DeviceID), DesktopID: desktopID, FirmwareID: msg.ID,
		Operation: msg.Type, DeviceGeneration: source.Generation(), DesktopGeneration: destination.Generation(), Payload: msg.Payload, Offset: input.Offset})
	if err != nil {
		s.logOperation("query.rejected", source, msg.Type, msg.ID, err, "device_id", p.DeviceID)
		if domain.CodeOf(err) != domain.CodeConflict {
			s.respondFirmware(source, msg.Type, msg.ID, nil, err)
		}
		return err
	}
	expires := q.Deadline.UTC().Format(time.RFC3339Nano)
	frame := contracts.DesktopEnvelope{ProtocolVersion: contracts.ProtocolVersion, MessageID: q.ID, Sequence: destination.NextSequence(), Type: msg.Type,
		SentAt: time.Now().UTC().Format(time.RFC3339Nano), ExpiresAt: &expires, Payload: msg.Payload}
	raw, err := json.Marshal(frame)
	if err == nil {
		err = s.validator.ValidateDesktopType(msg.Type, raw)
	}
	if err == nil && len(raw) > s.cfg.Limits.MaxFrameBytes {
		err = domain.ErrResourceExhausted("查询帧超过上限")
	}
	if err == nil {
		err = destination.Send(raw, s.conns.SendTimeout())
	}
	if err != nil {
		s.logOperation("query.forward_failed", source, msg.Type, msg.ID, domain.ErrDependencyUnavailable(""), "query_id", q.ID, "device_id", p.DeviceID, "client_id", desktopID)
		s.queries.Fail(q.ID, domain.ErrDependencyUnavailable("无法向 Desktop 发送查询"))
	} else {
		s.logger.Info("查询已转发 Desktop", "event", "query.forwarded", "request_id", msg.ID, "query_id", q.ID, "type", msg.Type,
			"device_id", p.DeviceID, "client_id", desktopID, "connection_id", source.Generation(), "desktop_connection_id", destination.Generation(), "expires_at", expires)
	}
	return nil
}

func (s *Server) notifyMessage(ctx context.Context, p Principal, raw json.RawMessage) error {
	var message struct {
		SessionID string `json:"agent_session_id"`
		MessageID string `json:"message_id"`
	}
	if err := json.Unmarshal(raw, &message); err != nil {
		return err
	}
	devices, err := s.sessions.DevicesForSession(ctx, p.DesktopID, message.SessionID)
	if err != nil {
		return err
	}
	for _, id := range devices {
		if conn := s.conns.Device(id); conn != nil {
			s.sendFirmwarePush(conn, "message.available", struct {
				ID      string          `json:"id"`
				Type    string          `json:"type"`
				Payload json.RawMessage `json:"payload"`
			}{contracts.NewMessageID("msg_"), "message.available", raw})
		}
	}
	s.logger.Info("新消息通知已处理", "event", "message.notified", "client_id", p.DesktopID, "agent_session_id", message.SessionID, "message_id", message.MessageID, "bound_device_count", len(devices))
	return nil
}
