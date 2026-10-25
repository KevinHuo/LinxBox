package transport

import (
	"bytes"
	"encoding/json"
	"errors"
	"log/slog"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

type logCapture struct {
	mu     sync.Mutex
	buffer bytes.Buffer
}

func (c *logCapture) Write(data []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.buffer.Write(data)
}

func (c *logCapture) snapshot() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.buffer.String()
}

func waitLog(t *testing.T, capture *logCapture, event string) []map[string]any {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		var records []map[string]any
		found := false
		for _, line := range strings.Split(strings.TrimSpace(capture.snapshot()), "\n") {
			if line == "" {
				continue
			}
			var record map[string]any
			if err := json.Unmarshal([]byte(line), &record); err != nil {
				t.Fatal("日志必须为合法 JSON", err)
			}
			records = append(records, record)
			found = found || record["event"] == event
		}
		if found {
			return records
		}
		if time.Now().After(deadline) {
			t.Fatal("缺少日志事件", event)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestRequestLogsCorrelateWithoutPayloads(t *testing.T) {
	s := newPipeServer(t)
	capture := &logCapture{}
	s.logger = slog.New(slog.NewJSONHandler(capture, nil))
	const private = "不应进入日志的私人正文和凭据"
	bad := pipeDial(t, s, "/v1/devices/connect", private)
	if _, _, err := bad.ReadMessage(); err == nil {
		t.Fatal("错误凭据必须关闭")
	}
	fw := pipeDial(t, s, "/v1/devices/connect", "dev-token")
	pipeWrite(t, fw, firmwareWire("device.hello", "invalid-first", map[string]any{"mac_address": private}))
	pipeWrite(t, fw, firmwareWire("device.hello", "hello-log", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}))
	pipeRead(t, fw, "response")
	dt := pipeDial(t, s, "/v1/clients/connect", "dt-token")
	pipeWrite(t, dt, desktopWire("device.bind", "bind-log", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}))
	pipeRead(t, dt, "binding.result")
	pipeRead(t, fw, "device.bound")
	item := sessionPayload(1)
	item["title"], item["summary"] = private, private
	pipeWrite(t, dt, desktopWire("agent_sessions.sync", "sync-log", map[string]any{"items": []any{item}}))
	pipeRead(t, dt, "agent_sessions.sync.result")
	pipeRead(t, fw, "agent_sessions.updated")
	pipeWrite(t, fw, firmwareWire("messages.list", "history-log", map[string]any{"agent_session_id": "sess-1"}))
	query := pipeRead(t, dt, "messages.list")
	var queryID string
	if err := json.Unmarshal(query["message_id"], &queryID); err != nil {
		t.Fatal(err)
	}
	pipeWrite(t, dt, desktopWire("query.result", "result-log", map[string]any{"reply_to_message_id": queryID, "ok": true,
		"data": map[string]any{"items": []any{map[string]any{"message_id": "message-log", "role": "assistant", "summary": private}}, "next_cursor": nil}}))
	pipeRead(t, fw, "response")
	pipeWrite(t, fw, firmwareWire("message.content.get", "body-log", map[string]any{"agent_session_id": "sess-1", "message_id": "message-log"}))
	query = pipeRead(t, dt, "message.content.get")
	var errorQueryID string
	if err := json.Unmarshal(query["message_id"], &errorQueryID); err != nil {
		t.Fatal(err)
	}
	pipeWrite(t, dt, desktopWire("query.result", "error-log", map[string]any{"reply_to_message_id": errorQueryID, "ok": false,
		"error": map[string]any{"code": "DEPENDENCY_UNAVAILABLE", "message": private}}))
	pipeRead(t, fw, "response")
	pipeWrite(t, fw, firmwareWire("messages.list", "timeout-log", map[string]any{"agent_session_id": "sess-1"}))
	pipeRead(t, dt, "messages.list")
	pipeRead(t, fw, "response")
	pipeWrite(t, fw, firmwareWire("device.hello", "invalid-after", map[string]any{"mac_address": private}))
	pipeWrite(t, dt, desktopWire("device.bind", "invalid-desktop", map[string]any{"mac_address": private}))
	pipeWrite(t, dt, desktopWire("device.bind", "bind-after", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}))
	pipeRead(t, dt, "binding.result")
	if err := fw.WriteControl(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, private), time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	records := waitLog(t, capture, "connection.closed")
	seen := map[string]bool{}
	correlated, failed, timedOut := false, false, false
	for _, record := range records {
		if event, ok := record["event"].(string); ok {
			seen[event] = true
		}
		if record["event"] == "query.completed" {
			if _, ok := record["duration_ms"].(float64); !ok {
				t.Fatal("查询结果须包含耗时", record)
			}
			if record["connection_id"] == nil || record["client_id"] != "desktop-1" || record["device_id"] != "AA:BB:CC:DD:EE:01" {
				t.Fatal("查询须固定双方关联", record)
			}
			if record["request_id"] == "history-log" && record["query_id"] == queryID && record["ok"] == true {
				correlated = true
			}
			if record["request_id"] == "body-log" && record["error_code"] == "DEPENDENCY_UNAVAILABLE" {
				failed = true
			}
			if record["request_id"] == "timeout-log" && record["error_code"] == "DEPENDENCY_UNAVAILABLE" && record["end_reason"] == "timeout" {
				timedOut = true
			}
		}
	}
	for _, event := range []string{"connection.auth_failed", "device.awaiting_hello", "device.registered", "request.received", "request.invalid", "request.dispatched", "device.binding", "catalog.synced", "query.forwarded", "query.result_received", "response.enqueued"} {
		if !seen[event] {
			t.Error("缺少联调事件", event)
		}
	}
	if !correlated || !failed || !timedOut {
		t.Fatal("必须关联成功、对端失败和超时的完整查询", correlated, failed, timedOut)
	}
	for _, secret := range []string{private, "dev-token", "dt-token", "linxbox-token.", "Authorization"} {
		if strings.Contains(capture.snapshot(), secret) {
			t.Fatal("日志泄露凭据、正文或协议头", secret)
		}
	}
}

func TestWebsocketCloseCode(t *testing.T) {
	if websocketCloseCode(errors.New("read failed")) != 0 || websocketCloseCode(&websocket.CloseError{Code: 1000, Text: "private"}) != 1000 {
		t.Fatal("只提取关闭码，不记录对端关闭原因")
	}
}
