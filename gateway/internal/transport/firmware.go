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

const deviceHelloTimeout = 10 * time.Second

// serveFirmware 处理固件连接的消息循环。
func (s *Server) serveFirmware(conn *websocket.Conn, p Principal) {
	generation := s.conns.NextGeneration()
	wsConn := lifecycle.NewConnection(conn, s.conns.SendQueueSize(), 10*time.Second, 30*time.Second, 60*time.Second, generation)
	if !s.conns.RegisterPending(wsConn) {
		s.logger.Warn("待登记连接数达上限，拒绝", "event", "device.pending_rejected", "connection_id", generation, "subject", p.Subject)
		wsConn.Close()
		return
	}
	defer s.conns.UnregisterPending(wsConn)
	defer func() {
		if p.DeviceID != "" {
			s.conns.UnregisterDevice(string(p.DeviceID), wsConn)
			s.queries.Disconnect(true, string(p.DeviceID), generation)
		}
	}()
	defer wsConn.Close()
	helloTimer := time.AfterFunc(s.helloTimeout, func() {
		s.logger.Warn("设备 hello 超时，关闭连接", "event", "device.hello_timeout", "connection_id", generation, "subject", p.Subject)
		wsConn.Close()
	})
	defer helloTimer.Stop()
	conn.SetReadLimit(int64(s.cfg.Limits.MaxFrameBytes) + frameReadMargin)
	_ = conn.SetReadDeadline(time.Now().Add(60 * time.Second))

	s.logger.Info("设备连接等待 hello", "event", "device.awaiting_hello", "connection_id", generation, "subject", p.Subject, "remote_addr", conn.RemoteAddr().String())
	ctx := context.Background()
	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			if !websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseNormalClosure, websocket.CloseNoStatusReceived) {
				s.logger.Info("设备断开", "event", "connection.closed", "kind", "device", "connection_id", generation, "device_id", p.DeviceID, "close_code", websocketCloseCode(err))
			} else {
				s.logger.Warn("设备连接异常", "event", "connection.closed", "kind", "device", "connection_id", generation, "device_id", p.DeviceID, "close_code", websocketCloseCode(err))
			}
			return
		}
		if len(data) > s.cfg.Limits.MaxFrameBytes {
			s.logger.Warn("固件帧超限", "device_id", p.DeviceID, "bytes", len(data))
			_ = conn.WriteControl(websocket.CloseMessage,
				websocket.FormatCloseMessage(websocket.CloseMessageTooBig, "frame too large"), time.Now().Add(time.Second))
			return
		}
		if p.DeviceID == "" {
			if s.identifyFirmware(ctx, wsConn, &p, data) {
				helloTimer.Stop()
			}
			continue
		}
		if err := s.handleFirmwareMessage(ctx, wsConn, p, data); err != nil {
			s.logger.Debug("固件消息处理已返回错误，业务原因见对应结果", "event", "request.failed", "connection_id", generation, "device_id", p.DeviceID, "error_code", domain.CodeOf(err))
		}
	}
}

// identifyFirmware 只允许待登记连接发送 hello，MAC 来自硬件上报而非 token 配置。
func (s *Server) identifyFirmware(ctx context.Context, conn *lifecycle.Connection, p *Principal, data []byte) bool {
	if err := s.validator.ValidateFirmwareIncoming(data); err != nil {
		s.logInvalid(conn, "固件", data)
		return false
	}
	var msg contracts.FirmwareMessage
	if err := json.Unmarshal(data, &msg); err != nil {
		return false
	}
	s.logIncoming(conn, "固件", msg.Type, msg.ID, "", msg.Payload)
	if msg.Type != "device.hello" {
		s.finishFirmware(conn, *p, msg, time.Now(), nil, domain.ErrConflict("请先发送 device.hello 登记设备"))
		return false
	}
	var payload contracts.DeviceHelloPayload
	if err := json.Unmarshal(msg.Payload, &payload); err != nil {
		return false
	}
	started := time.Now()
	binding, err := s.binding.RegisterDevice(ctx, p.Subject, payload.MacAddress, p.CredentialKey)
	if err == nil && !s.conns.IdentifyDevice(binding.DeviceID, conn) {
		err = domain.ErrResourceExhausted("在线连接数已达上限或连接已关闭")
	}
	if err != nil {
		s.logOperation("device.registration", conn, msg.Type, msg.ID, err, "subject", p.Subject, "mac_address", payload.MacAddress, "duration_ms", time.Since(started).Milliseconds())
		s.respondFirmware(conn, msg.Type, msg.ID, nil, err)
		return false
	}
	p.DeviceID = domain.DeviceID(binding.DeviceID)
	s.respondFirmware(conn, msg.Type, msg.ID, binding, nil)
	s.logger.Info("固件 MAC 已登记，设备上线", "event", "device.registered", "trace_id", traceID(conn.Generation(), msg.ID), "connection_id", conn.Generation(), "message_id", msg.ID, "device_id", p.DeviceID, "subject", p.Subject, "bound", binding.DesktopClientID != nil, "duration_ms", elapsedMillis(started))
	return true
}

// handleFirmwareMessage 校验并分发一条固件消息。
func (s *Server) handleFirmwareMessage(ctx context.Context, wsConn *lifecycle.Connection, p Principal, data []byte) error {
	return s.conns.WithDevice(string(p.DeviceID), wsConn, func() error {
		return s.dispatchFirmwareMessage(ctx, wsConn, p, data)
	})
}

func (s *Server) dispatchFirmwareMessage(ctx context.Context, wsConn *lifecycle.Connection, p Principal, data []byte) error {
	if err := s.validator.ValidateFirmwareIncoming(data); err != nil {
		s.logInvalid(wsConn, "固件", data)
		return nil
	}
	var msg contracts.FirmwareMessage
	if err := json.Unmarshal(data, &msg); err != nil {
		return err
	}
	started := time.Now()
	s.logIncoming(wsConn, "固件", msg.Type, msg.ID, string(p.DeviceID), msg.Payload)
	defer func() {
		s.logger.Debug("设备请求分发结束", "event", "request.dispatched", "trace_id", traceID(wsConn.Generation(), msg.ID), "connection_id", wsConn.Generation(), "device_id", p.DeviceID, "message_id", msg.ID, "type", msg.Type, "duration_ms", elapsedMillis(started))
	}()
	switch msg.Type {
	case "device.hello":
		var payload contracts.DeviceHelloPayload
		if err := json.Unmarshal(msg.Payload, &payload); err != nil {
			return err
		}
		binding, err := s.binding.Hello(ctx, p.DeviceID, payload.MacAddress)
		s.finishFirmware(wsConn, p, msg, started, binding, err)
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
		s.finishFirmware(wsConn, p, msg, started, page, err)
	case "agent_session.get":
		var payload contracts.AgentSessionGetPayload
		if err := json.Unmarshal(msg.Payload, &payload); err != nil {
			return err
		}
		session, err := s.sessions.Get(ctx, p.DeviceID, payload.AgentSessionID)
		s.finishFirmware(wsConn, p, msg, started, session, err)
	case "messages.list", "message.content.get":
		return s.forwardQuery(ctx, wsConn, p, msg)
	case "request.prepare", "request.get", "request.cancel", "request.confirm":
		if s.requests == nil {
			s.finishFirmware(wsConn, p, msg, started, nil, domain.ErrDependencyUnavailable("请求服务未配置"))
			break
		}
		var payload contracts.RequestPayload
		if err := json.Unmarshal(msg.Payload, &payload); err != nil {
			return err
		}
		var request contracts.Request
		var err error
		switch msg.Type {
		case "request.prepare":
			request, err = s.requests.Prepare(ctx, p.DeviceID, payload.RequestID, payload.AgentSessionID)
		case "request.get":
			request, err = s.requests.Get(ctx, p.DeviceID, payload.RequestID)
		case "request.cancel":
			request, err = s.requests.Cancel(ctx, p.DeviceID, payload.RequestID)
		case "request.confirm":
			request, err = s.requests.Confirm(ctx, p.DeviceID, payload.RequestID)
		}
		sessionID := request.AgentSessionID
		if sessionID == "" {
			sessionID = payload.AgentSessionID
		}
		fields := []any{"direction", "Gateway→固件", "device_id", p.DeviceID, "request_id", payload.RequestID, "agent_session_id", sessionID, "status", request.Status, "duration_ms", elapsedMillis(started)}
		response := contracts.FirmwareResponseBody{OK: err == nil, Data: request}
		if err != nil {
			response.Data, response.Error = nil, wireError(err)
		}
		fields = append(fields, s.logValueFields("response_body", response)...)
		s.logOperation("request.state", wsConn, msg.Type, msg.ID, err, fields...)
		s.respondFirmware(wsConn, msg.Type, msg.ID, request, err)
	default:
		// 首版未开放的操作（request.* 等）：明确拒绝，不假装成功。
		s.logger.Info("固件请求暂未开放", "type", msg.Type, "device_id", p.DeviceID)
		s.finishFirmware(wsConn, p, msg, started, nil, domain.ErrInternal("该操作在首版未开放"))
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
	if len(raw) > s.cfg.Limits.MaxFrameBytes {
		s.logOperation("response.rejected", wsConn, op, replyTo, domain.ErrResourceExhausted("编码后的响应超过固件帧上限，已改为有界错误响应"), "bytes", len(raw), "max_frame_bytes", s.cfg.Limits.MaxFrameBytes)
		s.respondFirmwareError(wsConn, op, replyTo, domain.ErrResourceExhausted("响应超过帧上限"))
		return
	}
	if verr := s.validator.ValidateFirmwareResponse(op, raw); verr != nil {
		s.logger.Error("响应未通过 Schema 校验", "op", op, "message_id", replyTo, "error_code", domain.CodeInternal)
		s.respondFirmwareError(wsConn, op, replyTo, domain.ErrInternal("内部错误"))
		return
	}
	if err := wsConn.Send(raw, s.conns.SendTimeout()); err != nil {
		s.logOperation("response.send_failed", wsConn, op, replyTo, domain.ErrDependencyUnavailable("响应未入队："+err.Error()), "kind", "device")
		return
	}
	s.logOperation("response.enqueued", wsConn, op, replyTo, err, "kind", "device", "bytes", len(raw))
}

// respondFirmwareError 不携带原始超限内容；兜底也须满足原操作 Schema 和编码后帧上限。
func (s *Server) respondFirmwareError(conn *lifecycle.Connection, op, replyTo string, cause error) {
	fallback := contracts.FirmwareResponse{
		ID:      contracts.NewMessageID("msg_"),
		Type:    "response",
		Payload: contracts.FirmwareResponseBody{OK: false, Error: wireError(cause)},
		ReplyTo: replyTo,
	}
	raw, err := json.Marshal(fallback)
	if err != nil || len(raw) > s.cfg.Limits.MaxFrameBytes {
		s.logger.Error("有界错误响应无法编码或超过限制", "op", op)
		conn.Close()
		return
	}
	if err := s.validator.ValidateFirmwareResponse(op, raw); err != nil {
		s.logger.Error("有界错误响应不符合 Schema", "op", op, "message_id", replyTo, "error_code", domain.CodeInternal)
		conn.Close()
		return
	}
	if err := conn.Send(raw, s.conns.SendTimeout()); err != nil {
		s.logOperation("response.send_failed", conn, op, replyTo, domain.ErrDependencyUnavailable("响应未入队："+err.Error()), "kind", "device")
		return
	}
	s.logOperation("response.enqueued", conn, op, replyTo, cause, "kind", "device", "bytes", len(raw))
}

func wireError(err error) *contracts.Error {
	e := toWireError(err)
	return &e
}
