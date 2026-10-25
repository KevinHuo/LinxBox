package transport

import (
	"bufio"
	"context"
	"encoding/json"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
	"github.com/gorilla/websocket"
)

// 使用真实 HTTP upgrade、WebSocket 编解码和 SQLite，但通过内存管道避免依赖监听权限。
type pipeResponseWriter struct {
	*httptest.ResponseRecorder
	conn   net.Conn
	reader *bufio.Reader
}

func (w *pipeResponseWriter) Hijack() (net.Conn, *bufio.ReadWriter, error) {
	return w.conn, bufio.NewReadWriter(w.reader, bufio.NewWriter(w.conn)), nil
}

func pipeDial(t *testing.T, s *Server, path, token string) *websocket.Conn {
	t.Helper()
	a, b := net.Pipe()
	done := make(chan struct{})
	go func() {
		defer close(done)
		reader := bufio.NewReader(b)
		r, err := http.ReadRequest(reader)
		if err != nil {
			return
		}
		s.Handler().ServeHTTP(&pipeResponseWriter{httptest.NewRecorder(), b, reader}, r)
	}()
	dialer := websocket.Dialer{Subprotocols: []string{"linxbox.v1", "linxbox-token." + token}, NetDialContext: func(context.Context, string, string) (net.Conn, error) { return a, nil }, HandshakeTimeout: time.Second}
	conn, _, err := dialer.Dial("ws://localhost"+path, nil)
	if err != nil {
		a.Close()
		b.Close()
		t.Fatal(err)
	}
	if conn.Subprotocol() != "linxbox.v1" {
		t.Fatal("未协商 linxbox.v1")
	}
	t.Cleanup(func() {
		conn.Close()
		b.Close()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("连接处理未退出")
		}
	})
	return conn
}

func newPipeServer(t *testing.T) *Server {
	t.Helper()
	cfg := testServerConfig(t)
	cfg.Limits.MaxFrameBytes = 65536
	cfg.Timeouts.QueryTimeout = config.Duration(200 * time.Millisecond)
	logger := slog.New(slog.DiscardHandler)
	v, err := contracts.NewValidator()
	if err != nil {
		t.Fatal(err)
	}
	db, err := storage.Open(cfg.Database.Path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	repos := storage.NewRepositories(db.DB())
	conns := lifecycle.NewConnections(logger, cfg.Limits)
	s := NewServer(cfg, logger, v, NewAuthenticator(cfg), conns)
	s.SetServices(service.NewBindingService(repos, cfg, s, logger), service.NewSessionService(repos, cfg, s, logger))
	return s
}

func desktopWire(typ, id string, payload any) map[string]any {
	return map[string]any{"protocol_version": "1.0", "message_id": id, "sequence": 1, "type": typ, "sent_at": time.Now().UTC().Format(time.RFC3339Nano), "payload": payload}
}
func firmwareWire(typ, id string, payload any) map[string]any {
	return map[string]any{"id": id, "type": typ, "payload": payload}
}
func pipeWrite(t *testing.T, c *websocket.Conn, data any) {
	t.Helper()
	c.SetWriteDeadline(time.Now().Add(time.Second))
	if err := c.WriteJSON(data); err != nil {
		t.Fatal(err)
	}
}
func pipeRead(t *testing.T, c *websocket.Conn, typ string) map[string]json.RawMessage {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	for {
		var frame map[string]json.RawMessage
		if err := c.ReadJSON(&frame); err != nil {
			t.Fatal(err)
		}
		var got string
		json.Unmarshal(frame["type"], &got)
		if got == typ {
			return frame
		}
	}
}
func setupPipe(t *testing.T) (*Server, *websocket.Conn, *websocket.Conn) {
	t.Helper()
	s := newPipeServer(t)
	fw := pipeDial(t, s, "/v1/devices/connect", "dev-token")
	pipeWrite(t, fw, firmwareWire("device.hello", "hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}))
	pipeRead(t, fw, "response")
	dt := pipeDial(t, s, "/v1/clients/connect", "dt-token")
	pipeWrite(t, dt, desktopWire("device.bind", "bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}))
	pipeRead(t, dt, "binding.result")
	pipeRead(t, fw, "device.bound")
	pipeWrite(t, dt, desktopWire("agent_sessions.sync", "sync", map[string]any{"items": []any{sessionPayload(1)}}))
	pipeRead(t, dt, "agent_sessions.sync.result")
	pipeRead(t, fw, "agent_sessions.updated")
	return s, fw, dt
}

func TestInteropReadFlow(t *testing.T) {
	s, fw, dt := setupPipe(t)
	pipeWrite(t, fw, firmwareWire("messages.list", "history", map[string]any{"agent_session_id": "sess-1"}))
	query := pipeRead(t, dt, "messages.list")
	var id string
	json.Unmarshal(query["message_id"], &id)
	pipeWrite(t, dt, desktopWire("query.result", "result", map[string]any{"reply_to_message_id": id, "ok": true, "data": map[string]any{"items": []any{map[string]any{"message_id": "answer-1", "role": "assistant", "summary": "最终回复"}}, "next_cursor": nil}}))
	response := pipeRead(t, fw, "response")
	raw, _ := json.Marshal(response)
	if err := s.validator.ValidateFirmwareResponse("messages.list", raw); err != nil {
		t.Fatal(err)
	}
	var replyTo string
	json.Unmarshal(response["reply_to"], &replyTo)
	if replyTo != "history" {
		t.Fatal("固件包 ID 未关联")
	}
	pipeWrite(t, fw, firmwareWire("message.content.get", "body", map[string]any{"agent_session_id": "sess-1", "message_id": "answer-1", "offset": 0}))
	query = pipeRead(t, dt, "message.content.get")
	json.Unmarshal(query["message_id"], &id)
	pipeWrite(t, dt, desktopWire("query.result", "body-result", map[string]any{"reply_to_message_id": id, "ok": true, "data": map[string]any{"text": "你好🙂", "next_offset": 10}}))
	response = pipeRead(t, fw, "response")
	raw, _ = json.Marshal(response)
	if err := s.validator.ValidateFirmwareResponse("message.content.get", raw); err != nil {
		t.Fatal(err)
	}
	pipeWrite(t, dt, desktopWire("message.available", "notice", map[string]any{"agent_session_id": "sess-1", "message_id": "answer-1"}))
	notice := pipeRead(t, fw, "message.available")
	raw, _ = json.Marshal(notice)
	if err := s.validator.ValidateFirmwarePush("message.available", raw); err != nil {
		t.Fatal(err)
	}
}

func TestInteropStaleConnectionRejected(t *testing.T) {
	s, fw, _ := setupPipe(t)
	old := s.conns.Desktop("desktop-1")
	newer := pipeDial(t, s, "/v1/clients/connect", "dt-token")
	pipeWrite(t, newer, desktopWire("agent_sessions.sync", "sync-new", map[string]any{"items": []any{sessionPayload(2)}}))
	pipeRead(t, newer, "agent_sessions.sync.result")
	raw, _ := json.Marshal(desktopWire("agent_sessions.sync", "stale", map[string]any{"items": []any{}}))
	if err := s.handleDesktopMessage(t.Context(), old, Principal{DesktopID: domain.DesktopClientID("desktop-1")}, raw); err == nil {
		t.Fatal("旧连接仍可修改目录")
	}
	pipeWrite(t, fw, firmwareWire("agent_session.get", "get", map[string]any{"agent_session_id": "sess-2"}))
	frame := pipeRead(t, fw, "response")
	var body struct {
		OK bool `json:"ok"`
	}
	json.Unmarshal(frame["payload"], &body)
	if !body.OK {
		t.Fatal("新连接目录被覆盖")
	}
}

func TestInteropOfflineAndTimeout(t *testing.T) {
	_, fw, dt := setupPipe(t)
	pipeWrite(t, fw, firmwareWire("messages.list", "timeout", map[string]any{"agent_session_id": "sess-1"}))
	pipeRead(t, dt, "messages.list")
	frame := pipeRead(t, fw, "response")
	var body struct {
		OK    bool            `json:"ok"`
		Error contracts.Error `json:"error"`
	}
	json.Unmarshal(frame["payload"], &body)
	if body.OK || body.Error.Code != "DEPENDENCY_UNAVAILABLE" {
		t.Fatalf("查询超时未明确失败：%s", frame["payload"])
	}
}

func requirePipeResponse(t *testing.T, s *Server, fw *websocket.Conn, operation, id, code string) json.RawMessage {
	t.Helper()
	frame := pipeRead(t, fw, "response")
	raw, err := json.Marshal(frame)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.validator.ValidateFirmwareResponse(operation, raw); err != nil {
		t.Fatal(err)
	}
	var response struct {
		ReplyTo string `json:"reply_to"`
		Payload struct {
			OK    bool             `json:"ok"`
			Data  json.RawMessage  `json:"data"`
			Error *contracts.Error `json:"error"`
		} `json:"payload"`
	}
	if err := json.Unmarshal(raw, &response); err != nil {
		t.Fatal(err)
	}
	if response.ReplyTo != id || response.Payload.OK != (code == "") {
		t.Fatalf("应答未正确关联或成功状态不符: %s", raw)
	}
	if code != "" && (response.Payload.Error == nil || response.Payload.Error.Code != code) {
		t.Fatalf("期望错误码 %s，得到 %s", code, raw)
	}
	return response.Payload.Data
}

func waitDesktopOffline(t *testing.T, s *Server) {
	t.Helper()
	deadline := time.Now().Add(time.Second)
	for s.conns.Desktop("desktop-1") != nil {
		if time.Now().After(deadline) {
			t.Fatal("Desktop 断开后连接未释放")
		}
		time.Sleep(time.Millisecond)
	}
}

func TestInteropQueryFailures(t *testing.T) {
	t.Run("unbound", func(t *testing.T) {
		s := newPipeServer(t)
		fw := pipeDial(t, s, "/v1/devices/connect", "dev-token")
		pipeWrite(t, fw, firmwareWire("messages.list", "unbound", map[string]any{"agent_session_id": "sess-1"}))
		requirePipeResponse(t, s, fw, "messages.list", "unbound", "PERMISSION_DENIED")
	})
	t.Run("unknown-session", func(t *testing.T) {
		s, fw, _ := setupPipe(t)
		pipeWrite(t, fw, firmwareWire("messages.list", "missing", map[string]any{"agent_session_id": "missing"}))
		requirePipeResponse(t, s, fw, "messages.list", "missing", "NOT_FOUND")
	})
	t.Run("offline", func(t *testing.T) {
		s, fw, dt := setupPipe(t)
		dt.Close()
		waitDesktopOffline(t, s)
		pipeWrite(t, fw, firmwareWire("messages.list", "offline", map[string]any{"agent_session_id": "sess-1"}))
		requirePipeResponse(t, s, fw, "messages.list", "offline", "DEPENDENCY_UNAVAILABLE")
	})
	t.Run("disconnect-pending", func(t *testing.T) {
		s, fw, dt := setupPipe(t)
		pipeWrite(t, fw, firmwareWire("message.content.get", "pending", map[string]any{"agent_session_id": "sess-1", "message_id": "answer-1", "offset": 0}))
		pipeRead(t, dt, "message.content.get")
		dt.Close()
		requirePipeResponse(t, s, fw, "message.content.get", "pending", "DEPENDENCY_UNAVAILABLE")
	})
	t.Run("desktop-error", func(t *testing.T) {
		s, fw, dt := setupPipe(t)
		pipeWrite(t, fw, firmwareWire("message.content.get", "missing-message", map[string]any{"agent_session_id": "sess-1", "message_id": "missing", "offset": 0}))
		query := pipeRead(t, dt, "message.content.get")
		var id string
		if err := json.Unmarshal(query["message_id"], &id); err != nil {
			t.Fatal(err)
		}
		pipeWrite(t, dt, desktopWire("query.result", "error", map[string]any{"reply_to_message_id": id, "ok": false, "error": map[string]any{"code": "NOT_FOUND", "message": "消息不存在"}}))
		requirePipeResponse(t, s, fw, "message.content.get", "missing-message", "NOT_FOUND")
	})
	t.Run("offset-overflow", func(t *testing.T) {
		s, fw, _ := setupPipe(t)
		pipeWrite(t, fw, firmwareWire("message.content.get", "overflow", map[string]any{"agent_session_id": "sess-1", "message_id": "answer-1", "offset": uint64(1) << 63}))
		requirePipeResponse(t, s, fw, "message.content.get", "overflow", "INVALID_ARGUMENT")
	})
}

func TestInteropDeviceReconnectDropsOldReply(t *testing.T) {
	s, old, dt := setupPipe(t)
	pipeWrite(t, old, firmwareWire("messages.list", "reused", map[string]any{"agent_session_id": "sess-1"}))
	oldQuery := pipeRead(t, dt, "messages.list")
	newer := pipeDial(t, s, "/v1/devices/connect", "dev-token")
	pipeWrite(t, newer, firmwareWire("device.hello", "hello-new", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}))
	requirePipeResponse(t, s, newer, "device.hello", "hello-new", "")
	pipeWrite(t, newer, firmwareWire("messages.list", "fresh", map[string]any{"agent_session_id": "sess-1"}))
	newQuery := pipeRead(t, dt, "messages.list")
	for _, query := range []map[string]json.RawMessage{oldQuery, newQuery} {
		var id string
		if err := json.Unmarshal(query["message_id"], &id); err != nil {
			t.Fatal(err)
		}
		pipeWrite(t, dt, desktopWire("query.result", "result-"+id, map[string]any{"reply_to_message_id": id, "ok": true,
			"data": map[string]any{"items": []any{}, "next_cursor": nil}}))
	}
	// 迟到结果先到达 Desktop 入站循环；新设备只能收到自身查询的应答。
	requirePipeResponse(t, s, newer, "messages.list", "fresh", "")
}
