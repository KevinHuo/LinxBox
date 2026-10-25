package transport

import (
	"strings"
	"testing"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestMCPImplicitRequestIDAcrossTransports(t *testing.T) {
	for _, transport := range []string{"streamable_http", "sse"} {
		t.Run(transport, func(t *testing.T) {
			env, capture := streamableEnv(t, 16, true)
			var client *mcp.ClientSession
			var headers *mcpHeaders
			if transport == "sse" {
				client, headers = newMCPClient(t, env)
			} else {
				client, headers = newStreamableClient(t, env)
			}
			for key, value := range map[string]string{
				"X-Linx-Device-Id": "aa:bb:cc:dd:ee:01", "X-Linx-Agent-Id": "agent", "X-Linx-Session-Id": "voice-demo",
			} {
				headers.set(key, value)
			}
			headers.set("X-Linx-Request-Id", "")
			callMCP(t, env, client, map[string]any{"text": "演示文字"}, "NOT_FOUND")
			fw := prepareStreamableRequest(t, env)
			// 显式错误、空、重复的 Header 都不能被演示开关改成隐式定位。
			headers.set("X-Linx-Request-Id", "missing")
			callMCP(t, env, client, map[string]any{"text": "演示文字"}, "NOT_FOUND")
			for _, values := range [][]string{{""}, {"req-1", "other"}, {strings.Repeat("a", 129)}} {
				headers.mu.Lock()
				headers.header["X-Linx-Request-Id"] = values
				headers.mu.Unlock()
				callMCP(t, env, client, map[string]any{"text": "演示文字"}, "INVALID_ARGUMENT")
			}
			headers.set("X-Linx-Request-Id", "")
			headers.set("X-Linx-Agent-Id", "wrong-agent")
			callMCP(t, env, client, map[string]any{"text": "演示文字"}, "PERMISSION_DENIED")
			headers.set("X-Linx-Agent-Id", "agent")
			headers.set("X-Linx-Session-Id", "")
			callMCP(t, env, client, map[string]any{"text": "演示文字"}, "INVALID_ARGUMENT")
			headers.set("X-Linx-Session-Id", "voice-demo")
			callMCP(t, env, client, map[string]any{"text": " \n"}, "INVALID_ARGUMENT")
			const text = "  这轮演示的原文🙂\n"
			for range 2 {
				r := callMCP(t, env, client, map[string]any{"text": text}, "")
				if r.RequestID != "req-1" || r.Status != "awaiting_confirmation" || r.Text == nil || *r.Text != text {
					t.Fatal(r)
				}
			}
			fw.expectPush(t, "request.proposed")
			callMCP(t, env, client, map[string]any{"text": "修改文字"}, "CONFLICT")
			if r := fw.fwReq(t, "request.cancel", map[string]any{"request_id": "req-1"}); !r.OK {
				t.Fatal(r)
			}
			callMCP(t, env, client, map[string]any{"text": text}, "NOT_FOUND")
			if r := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "req-2", "agent_session_id": "sess-1"}); !r.OK {
				t.Fatal(r)
			}
			headers.set("X-Linx-Request-Id", "req-1")
			callMCP(t, env, client, map[string]any{"text": text}, "CONFLICT")
			headers.set("X-Linx-Request-Id", "")
			headers.set("X-Linx-Session-Id", "voice-next")
			if r := callMCP(t, env, client, map[string]any{"text": "下一轮"}, ""); r.RequestID != "req-2" {
				t.Fatal(r)
			}
			client.Close()
			records := waitLog(t, capture, "mcp.request.resolved")
			resolved, notifications, readableFailure := 0, 0, false
			for _, record := range records {
				if record["event"] == "mcp.request.resolved" {
					resolved++
					if record["request_id_source"] != "unique_pending" || record["request_id"] == "" || record["implicit_request_id_enabled"] != true {
						t.Fatal(record)
					}
				}
				if record["event"] == "request.notification.enqueued" && record["type"] == "request.proposed" && record["request_id"] == "req-1" {
					notifications++
				}
				if record["event"] == "mcp.call.completed" && record["error_code"] == "NOT_FOUND" && record["request_id_source"] == "unique_pending" {
					body := record["response_body"].(map[string]any)
					message := body["error"].(map[string]any)["message"]
					if message != "没有可关联的待处理请求，请先 request.prepare" {
						t.Fatal(message)
					}
					readableFailure = true
				}
			}
			if resolved != 3 || notifications != 1 || !readableFailure {
				t.Fatal(resolved, notifications, readableFailure)
			}
			if strings.Contains(capture.snapshot(), text) || strings.Contains(capture.snapshot(), "mcp-token") {
				t.Fatal("联调日志泄露内容")
			}
		})
	}
}
