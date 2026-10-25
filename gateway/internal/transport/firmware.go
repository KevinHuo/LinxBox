package transport

import (
	"context"
	"encoding/json"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
)

// serveFirmware 处理固件连接的消息循环。
func (s *Server) serveFirmware(conn *websocket.Conn, p Principal) {
	generation := s.conns.NextGeneration()
	wsConn := lifecycle.NewConnection(conn, s.conns.SendQueueSize(), 10*time.Second, 30*time.Second, 60*time.Second, generation)
	if !s.conns.RegisterDevice(string(p.DeviceID), wsConn) {
		s.logger.Warn("设备连接数达上限，拒绝", "device_id", p.DeviceID)
		wsConn.Close()
		return
	}
	defer s.conns.UnregisterDevice(string(p.DeviceID), wsConn)
	defer wsConn.Close()
	defer s.queries.Disconnect(true, string(p.DeviceID), generation)
	conn.SetReadLimit(int64(s.cfg.Limits.MaxFrameBytes) + frameReadMargin)
	_ = conn.SetReadDeadline(time.Now().Add(60 * time.Second))

	s.logger.Info("设备已连接", "device_id", p.DeviceID, "subject", p.Subject)
	ctx := context.Background()
	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			if !websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseNormalClosure, websocket.CloseNoStatusReceived) {
				s.logger.Info("设备断开", "device_id", p.DeviceID)
			} else {
				s.logger.Warn("设备连接异常", "device_id", p.DeviceID, "err", err)
			}
			return
		}
		if len(data) > s.cfg.Limits.MaxFrameBytes {
			s.logger.Warn("固件帧超限", "device_id", p.DeviceID, "bytes", len(data))
			_ = conn.WriteControl(websocket.CloseMessage,
				websocket.FormatCloseMessage(websocket.CloseMessageTooBig, "frame too large"), time.Now().Add(time.Second))
			return
		}
		if err := s.handleFirmwareMessage(ctx, wsConn, p, data); err != nil {
			s.logger.Warn("固件消息处理失败", "device_id", p.DeviceID, "err", err)
		}
	}
}

// handleFirmwareMessage 校验并分发一条固件消息。
func (s *Server) handleFirmwareMessage(ctx context.Context, wsConn *lifecycle.Connection, p Principal, data []byte) error {
	return s.conns.WithDevice(string(p.DeviceID), wsConn, func() error {
		return s.dispatchFirmwareMessage(ctx, wsConn, p, data)
	})
}

func (s *Server) dispatchFirmwareMessage(ctx context.Context, wsConn *lifecycle.Connection, p Principal, data []byte) error {
	if err := s.validator.ValidateFirmwareIncoming(data); err != nil {
		s.logger.Warn("固件消息未通过 Schema 校验", "device_id", p.DeviceID, "err", err)
		return nil
	}
	var msg contracts.FirmwareMessage
	if err := json.Unmarshal(data, &msg); err != nil {
		return err
	}
	switch msg.Type {
	case "device.hello":
		var payload contracts.DeviceHelloPayload
		if err := json.Unmarshal(msg.Payload, &payload); err != nil {
			return err
		}
		binding, err := s.binding.Hello(ctx, p.DeviceID, payload.MacAddress)
		s.respondFirmware(wsConn, "device.hello", msg.ID, binding, err)
	case "agent_sessions.list":
		var payload contracts.AgentSessionsListPayload
		if err := json.Unmarshal(msg.Payload, &payload); err != nil {
			return err
		}
		var cursor string
		if payload.Cursor != nil {
			cursor = *payload.Cursor
		}
		page, err := s.sessions.List(ctx, p.DeviceID, cursor)
		s.respondFirmware(wsConn, "agent_sessions.list", msg.ID, page, err)
	case "agent_session.get":
		var payload contracts.AgentSessionGetPayload
		if err := json.Unmarshal(msg.Payload, &payload); err != nil {
			return err
		}
		session, err := s.sessions.Get(ctx, p.DeviceID, payload.AgentSessionID)
		s.respondFirmware(wsConn, "agent_session.get", msg.ID, session, err)
	case "messages.list", "message.content.get":
		return s.forwardQuery(ctx, wsConn, p, msg)
	default:
		// 首版未开放的操作（request.* 等）：明确拒绝，不假装成功。
		s.logger.Info("固件请求暂未开放", "type", msg.Type, "device_id", p.DeviceID)
		s.respondFirmware(wsConn, msg.Type, msg.ID, nil, domain.ErrInternal("该操作在首版未开放"))
	}
	return nil
}

// respondFirmware 发送固件 response 并按原操作对应的 response 定义校验。
func (s *Server) respondFirmware(wsConn *lifecycle.Connection, op, replyTo string, data any, err error) {
	body := contracts.FirmwareResponseBody{}
	if err != nil {
		wire := toWireError(err)
		body.OK = false
		body.Error = &wire
	} else {
		body.OK = true
		body.Data = data
	}
	resp := contracts.FirmwareResponse{
		ID:      contracts.NewMessageID("msg_"),
		Type:    "response",
		Payload: body,
		ReplyTo: replyTo,
	}
	raw, merr := json.Marshal(resp)
	if merr != nil {
		s.logger.Error("响应编码失败", "op", op, "err", merr)
		return
	}
	if verr := s.validator.ValidateFirmwareResponse(op, raw); verr != nil {
		s.logger.Error("响应未通过 Schema 校验", "op", op, "err", verr)
		// 兜底：发送内部错误，仍按原操作配对。
		fallback := contracts.FirmwareResponse{
			ID:      contracts.NewMessageID("msg_"),
			Type:    "response",
			Payload: contracts.FirmwareResponseBody{OK: false, Error: wireError(domain.ErrInternal("内部错误"))},
			ReplyTo: replyTo,
		}
		if fb, e := json.Marshal(fallback); e == nil {
			_ = wsConn.Send(fb, s.conns.SendTimeout())
		}
		return
	}
	if err := wsConn.Send(raw, s.conns.SendTimeout()); err != nil {
		s.logger.Warn("固件响应发送失败", "op", op, "err", err)
	}
}

func wireError(err error) *contracts.Error {
	e := toWireError(err)
	return &e
}
