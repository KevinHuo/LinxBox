package transport

import (
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func loggedMCPEnv(t *testing.T) (*testEnv, *logCapture) {
	t.Helper()
	capture := &logCapture{}
	env := newTestEnvWithLogger(t, func(cfg *config.Config) {
		cfg.Limits.MaxFrameBytes = 65536
		cfg.Credentials.MCP = []config.MCPCredential{{Token: "mcp-token", DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"}}
	}, slog.New(slog.NewJSONHandler(capture, &slog.HandlerOptions{Level: slog.LevelDebug})))
	return env, capture
}

func TestMCPLogsCorrelateASRAndOfflineRecovery(t *testing.T) {
	env, capture := loggedMCPEnv(t)
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if reply := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
		t.Fatal(reply)
	}
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	if reply := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
		t.Fatal(reply)
	}
	if reply := dt.dtReq(t, "agent_sessions.sync", map[string]any{"items": []any{sessionPayload(1)}}); !reply.OK {
		t.Fatal(reply)
	}
	if reply := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "req-1", "agent_session_id": "sess-1"}); !reply.OK {
		t.Fatal(reply)
	}
	client, headers := newMCPClient(t, env)
	// 外部提供的诊断头不能冒充网关的日志关联元数据。
	headers.set(mcpLogTraceHeader, "forged-trace")
	headers.set(mcpLogConnectionHeader, "99999")
	headers.set(mcpLogRPCHeader, "forged-rpc")
	const text = "  私人的识别文字🙂，不要写进日志 /private/audio\n"
	result := callMCP(t, env, client, map[string]any{"text": text}, "")
	if result.Text == nil || *result.Text != text {
		t.Fatal("识别原文保存行为不能改变")
	}
	callMCP(t, env, client, map[string]any{"text": text}, "")
	headers.set("X-Linx-Request-Id", "")
	callMCP(t, env, client, map[string]any{"text": text}, "INVALID_ARGUMENT")
	headers.set("X-Linx-Request-Id", "missing-request")
	callMCP(t, env, client, map[string]any{"text": text}, "NOT_FOUND")
	headers.set("X-Linx-Request-Id", "req-1")
	callMCP(t, env, client, map[string]any{"text": " \n"}, "INVALID_ARGUMENT")
	headers.set("X-Linx-Agent-Id", "other-agent")
	callMCP(t, env, client, map[string]any{"text": text}, "PERMISSION_DENIED")
	headers.set("X-Linx-Agent-Id", "agent")
	if reply := fw.fwReq(t, "request.cancel", map[string]any{"request_id": "req-1"}); !reply.OK {
		t.Fatal(reply)
	}
	if reply := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "req-2", "agent_session_id": "sess-1"}); !reply.OK {
		t.Fatal(reply)
	}
	fw.conn.Close()
	deadline := time.Now().Add(2 * time.Second)
	for env.server.conns.Device("AA:BB:CC:DD:EE:01") != nil {
		if time.Now().After(deadline) {
			t.Fatal("固件未完成离线清理")
		}
		time.Sleep(5 * time.Millisecond)
	}
	headers.set("X-Linx-Request-Id", "req-2")
	callMCP(t, env, client, map[string]any{"text": text}, "")
	client.Close()
	records := waitLog(t, capture, "mcp.connection.closed")
	seen := map[string]bool{}
	received := map[string]bool{}
	validated := map[string]bool{}
	completed, notifications, deferred, contextFailure, requestFailure, argumentFailure, sourceFailure := 0, 0, false, false, false, false, false
	for _, record := range records {
		event, _ := record["event"].(string)
		seen[event] = true
		if event == "mcp.call.received" {
			received[record["trace_id"].(string)] = true
			if record["text_non_blank"] == true && record["text_bytes"] == float64(len(text)) {
				if record["text_codepoints"] != float64(utf8.RuneCountInString(text)) || record["body"].(map[string]any)["text"] != "[已脱敏]" {
					t.Fatal("MCP 日志需要长度及脱敏 body", record)
				}
			}
		}
		if event == "mcp.call.validated" {
			validated[record["trace_id"].(string)] = true
		}
		if event == "mcp.call.completed" {
			trace := record["trace_id"].(string)
			if !received[trace] || record["mcp_connection_id"] == float64(0) || record["rpc_id"] == "" {
				t.Fatal("接收与结果必须共享真实调用及连接关联", record)
			}
			if record["ok"] == true {
				completed++
				if !validated[trace] || record["status"] != "awaiting_confirmation" || record["agent_session_id"] != "sess-1" {
					t.Fatal("成功应能识别已校验和保存的目标状态", record)
				}
			} else {
				if record["error_message"] == "" || record["error_hint"] == "" {
					t.Fatal("失败必须有原因和提示", record)
				}
				switch record["failed_stage"] {
				case "context":
					missing, _ := record["missing_headers"].([]any)
					contextFailure = contextFailure || len(missing) > 0
				case "request_state":
					requestFailure = record["error_code"] == "NOT_FOUND"
				case "arguments":
					argumentFailure = true
				}
				if record["error_code"] == "PERMISSION_DENIED" && record["credential_linx_agent_id"] == "agent" && record["linx_agent_id"] == "other-agent" {
					sourceFailure = true
				}
			}
		}
		if event == "request.notification.enqueued" && record["type"] == "request.proposed" && record["request_id"] == "req-1" {
			notifications++
		}
		if event == "request.notification.deferred" && record["request_id"] == "req-2" && record["status"] == "awaiting_confirmation" {
			deferred = true
		}
	}
	for _, event := range []string{"mcp.connection.opened", "mcp.call.received", "mcp.call.validated", "mcp.call.completed", "mcp.rpc.accepted", "request.notification.enqueued", "request.notification.deferred"} {
		if !seen[event] {
			t.Error("缺少阶段", event)
		}
	}
	if completed != 3 || notifications != 1 || !deferred || !contextFailure || !requestFailure || !argumentFailure || !sourceFailure {
		t.Fatal("成功、幂等、离线及失败阶段不完整", completed, notifications, deferred, contextFailure, requestFailure, argumentFailure, sourceFailure)
	}
	for _, secret := range []string{text, "私人的识别文字", "/private/audio", "mcp-token", "dev-token", "dt-token", "forged-trace", "forged-rpc"} {
		if strings.Contains(capture.snapshot(), secret) {
			t.Fatal("MCP 日志泄露正文、凭据或伪造上下文", secret)
		}
	}
}

func TestMCPHTTPFailureLogsAndNotificationFailure(t *testing.T) {
	env, capture := loggedMCPEnv(t)
	for _, entry := range []struct{ method, path, token string }{
		{"POST", "/mcp/message?sessionid=private-session", "dev-token"},
		{"POST", "/mcp/message?sessionid=private-session", "mcp-token"},
		{"DELETE", "/mcp", "mcp-token"},
	} {
		req := httptest.NewRequest(entry.method, entry.path, strings.NewReader(`{"text":"不能写入日志"}`))
		req.Header.Set("Authorization", "Bearer "+entry.token)
		req.Header.Set("X-Linx-Request-Id", "mcp-token")
		env.server.Handler().ServeHTTP(httptest.NewRecorder(), req)
	}
	closed := closedConn()
	if !env.server.conns.RegisterDevice("AA:BB:CC:DD:EE:01", closed) {
		t.Fatal("构造连接失败")
	}
	text := "不得记录的原文"
	env.server.NotifyRequest("AA:BB:CC:DD:EE:01", "request.proposed", contracts.Request{RequestID: "notify-fail", AgentSessionID: "sess-1", TargetTitle: text, Text: &text, Status: "awaiting_confirmation"})
	records := waitLog(t, capture, "push.failed")
	for _, record := range records {
		if record["event"] == "mcp.http.rejected" && (record["http_status"] == nil || record["error_hint"] == "") {
			t.Fatal("HTTP 失败也需要可读状态和建议", record)
		}
		if record["event"] == "push.failed" && (record["request_id"] != "notify-fail" || record["notification_status"] != "failed") {
			t.Fatal("通知失败必须关联原业务请求", record)
		}
	}
	for _, secret := range []string{"private-session", "mcp-token", "dev-token", text, "不能写入日志"} {
		if strings.Contains(capture.snapshot(), secret) {
			t.Fatal("拒绝路径也不能泄露", secret)
		}
	}
}

func TestMCPLogContextAndHints(t *testing.T) {
	env, _ := loggedMCPEnv(t)
	for _, value := range []string{"mcp-token", "/private/file", strings.Repeat("a", 129)} {
		if env.server.safeMCPID(value) == value {
			t.Fatal("敏感标识未处理", value)
		}
	}
	if env.server.safeMCPID("") != "" || env.server.mcpRPCID(nil) != "" || env.server.mcpRPCID(float64(3)) != "3" {
		t.Fatal("空标识或 RPC ID 错误")
	}
	entry := env.server.mcpCallContext(&mcp.CallToolRequest{Extra: &mcp.RequestExtra{Header: http.Header{"X-Linx-Request-Id": []string{"one", "two"}, mcpLogTraceHeader: []string{"mcp_bad!bad!"}}}})
	if len(entry.duplicate) != 1 || !strings.HasPrefix(entry.trace, "mcp_") || len(entry.missing) != 3 {
		t.Fatal("重复和缺失的上下文应明确区分", entry)
	}
	for _, code := range []domain.Code{domain.CodeUnauthenticated, domain.CodeInvalidArgument, domain.CodeNotFound, domain.CodeConflict, domain.CodePermissionDenied, domain.CodeInternal} {
		if mcpBusinessHint(code) == "" {
			t.Fatal("缺少业务排查提示", code)
		}
	}
	for _, status := range []int{401, 403, 404, 410, 405, 413, 400, 503} {
		if mcpHTTPHint(status) == "" {
			t.Fatal("缺少 HTTP 排查提示", status)
		}
	}
	var fields []any
	setErrorHint(fields, "unused")
}
