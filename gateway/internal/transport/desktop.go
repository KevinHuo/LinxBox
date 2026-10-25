package transport

import (
	"context"
	"encoding/json"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
)

// serveDesktop 处理 Desktop 连接的消息循环。
func (s *Server) serveDesktop(conn *websocket.Conn, p Principal) {
	generation := s.conns.NextGeneration()
	wsConn := lifecycle.NewConnection(conn, s.conns.SendQueueSize(), 10*time.Second, 30*time.Second, 60*time.Second, generation)
	if !s.conns.RegisterDesktop(string(p.DesktopID), wsConn) {
		s.logger.Warn("Desktop 连接数达上限，拒绝", "client_id", p.DesktopID)
		wsConn.Close()
		return
	}
	defer s.conns.UnregisterDesktop(string(p.DesktopID), wsConn)
	defer wsConn.Close()
	defer s.queries.Disconnect(false, string(p.DesktopID), generation)
	conn.SetReadLimit(int64(s.cfg.Limits.MaxFrameBytes) + frameReadMargin)
	_ = conn.SetReadDeadline(time.Now().Add(60 * time.Second))

	s.logger.Info("Desktop 已连接", "client_id", p.DesktopID, "subject", p.Subject)
	ctx := context.Background()
	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			if !websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseNormalClosure, websocket.CloseNoStatusReceived) {
				s.logger.Info("Desktop 断开", "client_id", p.DesktopID)
			} else {
				s.logger.Warn("Desktop 连接异常", "client_id", p.DesktopID, "err", err)
			}
			return
		}
		if len(data) > s.cfg.Limits.MaxFrameBytes {
			s.logger.Warn("Desktop 帧超限", "client_id", p.DesktopID, "bytes", len(data))
			_ = conn.WriteControl(websocket.CloseMessage,
				websocket.FormatCloseMessage(websocket.CloseMessageTooBig, "frame too large"), time.Now().Add(time.Second))
			return
		}
		if err := s.handleDesktopMessage(ctx, wsConn, p, data); err != nil {
			s.logger.Warn("Desktop 消息处理失败", "client_id", p.DesktopID, "err", err)
		}
	}
}

// handleDesktopMessage 校验并分发一条 Desktop 消息。
func (s *Server) handleDesktopMessage(ctx context.Context, wsConn *lifecycle.Connection, p Principal, data []byte) error {
	return s.conns.WithDesktop(string(p.DesktopID), wsConn, func() error {
		return s.dispatchDesktopMessage(ctx, wsConn, p, data)
	})
}

func (s *Server) dispatchDesktopMessage(ctx context.Context, wsConn *lifecycle.Connection, p Principal, data []byte) error {
	if err := s.validator.ValidateDesktopIncoming(data); err != nil {
		s.logger.Warn("Desktop 消息未通过 Schema 校验", "client_id", p.DesktopID, "err", err)
		return nil
	}
	var msg contracts.DesktopMessage
	if err := json.Unmarshal(data, &msg); err != nil {
		return err
	}
	if msg.ExpiresAt != nil {
		expires, err := time.Parse(time.RFC3339, *msg.ExpiresAt)
		if err != nil || !time.Now().Before(expires) {
			return nil
		}
	}
	switch msg.Type {
	case "client.hello":
		s.logger.Info("Desktop hello", "client_id", p.DesktopID, "message_id", msg.MessageID)
	case "client.heartbeat":
		s.logger.Debug("Desktop heartbeat", "client_id", p.DesktopID)
	case "device.bind":
		var payload contracts.DeviceBindPayload
		if err := json.Unmarshal(msg.Payload, &payload); err != nil {
			return err
		}
		binding, err := s.binding.Bind(ctx, p.DesktopID, p.Subject, payload.MacAddress)
		s.sendDesktopReply(wsConn, "binding.result", msg.MessageID, bindingResult(binding, err, msg.MessageID))
	case "agent_sessions.sync":
		var payload contracts.SessionSyncPayload
		if err := json.Unmarshal(msg.Payload, &payload); err != nil {
			return err
		}
		_, err := s.sessions.Sync(ctx, p.DesktopID, payload.Items)
		s.sendDesktopReply(wsConn, "agent_sessions.sync.result", msg.MessageID, syncResult(err, msg.MessageID))
	case "query.result":
		return s.queries.Complete(string(p.DesktopID), wsConn.Generation(), data)
	case "message.available":
		return s.notifyMessage(ctx, p, msg.Payload)
	case "task.ack", "task.started", "task.progress",
		"task.approval_required", "task.completed", "task.failed",
		"task.cancelled", "task.interrupted":
		// 契约已识别并记录；对应操作在后续阶段开放，不假装成功。
		s.logger.Info("Desktop 消息暂未开放", "type", msg.Type, "client_id", p.DesktopID)
	default:
		s.logger.Warn("未知 Desktop 消息类型", "type", msg.Type)
	}
	return nil
}

// sendDesktopReply 发送带 reply_to_message_id 的应答并校验。
func (s *Server) sendDesktopReply(wsConn *lifecycle.Connection, typ, replyTo string, payload any) {
	msg := contracts.DesktopEnvelope{
		ProtocolVersion: contracts.ProtocolVersion,
		MessageID:       contracts.NewMessageID("msg_"),
		Sequence:        wsConn.NextSequence(),
		Type:            typ,
		SentAt:          time.Now().UTC().Format(time.RFC3339),
		Payload:         payload,
	}
	raw, err := json.Marshal(msg)
	if err != nil {
		s.logger.Error("应答编码失败", "type", typ, "err", err)
		return
	}
	if err := s.validator.ValidateDesktopType(typ, raw); err != nil {
		s.logger.Error("应答未通过 Schema 校验", "type", typ, "err", err)
		return
	}
	if err := wsConn.Send(raw, s.conns.SendTimeout()); err != nil {
		s.logger.Warn("应答发送失败", "type", typ, "err", err)
	}
}

func bindingResult(binding contracts.BoundBinding, err error, replyTo string) contracts.BindingResultPayload {
	if err != nil {
		wire := toWireError(err)
		return contracts.BindingResultPayload{ReplyToMessageID: replyTo, OK: false, Error: &wire}
	}
	return contracts.BindingResultPayload{ReplyToMessageID: replyTo, OK: true, Data: &binding}
}

func syncResult(err error, replyTo string) contracts.SyncResultPayload {
	if err != nil {
		wire := toWireError(err)
		return contracts.SyncResultPayload{ReplyToMessageID: replyTo, OK: false, Error: &wire}
	}
	return contracts.SyncResultPayload{ReplyToMessageID: replyTo, OK: true}
}
