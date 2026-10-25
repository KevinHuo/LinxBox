package transport

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
)

func streamableEnv(t *testing.T, maxConnections int) (*testEnv, *logCapture) {
	t.Helper()
	capture := &logCapture{}
	env := newTestEnvWithLogger(t, func(cfg *config.Config) {
		cfg.Limits.MaxFrameBytes = 65536
		cfg.Limits.MaxOnlineConnections = maxConnections
		cfg.Credentials.MCP = []config.MCPCredential{
			{Token: "mcp-token", DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"},
			{Token: "other-token", DeviceID: "AA:BB:CC:DD:EE:02", AgentID: "other"},
		}
	}, slog.New(slog.NewJSONHandler(capture, &slog.HandlerOptions{Level: slog.LevelDebug})))
	return env, capture
}

func newStreamableClient(t *testing.T, env *testEnv) (*mcp.ClientSession, *mcpHeaders) {
	t.Helper()
	headers := &mcpHeaders{header: http.Header{}}
	headers.set("Authorization", "Bearer mcp-token")
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	t.Cleanup(cancel)
	client := mcp.NewClient(&mcp.Implementation{Name: "Streamable 联调端", Version: "1"}, nil)
	session, err := client.Connect(ctx, &mcp.StreamableClientTransport{
		Endpoint: env.baseHTTP + "/mcp", HTTPClient: &http.Client{Transport: headers},
	}, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { session.Close() })
	return session, headers
}

func prepareStreamableRequest(t *testing.T, env *testEnv) *wsClient {
	t.Helper()
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
	return fw
}

func streamHTTP(t *testing.T, env *testEnv, method, sessionID, token, body string, change func(*http.Request)) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, "/mcp", strings.NewReader(body))
	r.Header.Set("Authorization", "Bearer "+token)
	r.Header.Set("Content-Type", "application/json")
	r.Header.Set("Accept", "application/json, text/event-stream")
	if sessionID != "" {
		r.Header.Set(mcpSessionHeader, sessionID)
	}
	if change != nil {
		change(r)
	}
	w := httptest.NewRecorder()
	env.server.Handler().ServeHTTP(w, r)
	return w
}

const streamInitialize = `{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"diagnostic","version":"1"}}}`
const streamToolsList = `{"jsonrpc":"2.0","id":2,"method":"tools/list"}`

func TestMCPStreamableOriginalTextAndPerCallContext(t *testing.T) {
	env, capture := streamableEnv(t, 16)
	fw := prepareStreamableRequest(t, env)
	client, headers := newStreamableClient(t, env)
	// 初始化与发现工具不需要设备录音的上下文；真正调用才要求四头。
	tools, err := client.ListTools(t.Context(), nil)
	if err != nil || len(tools.Tools) != 1 || tools.Tools[0].Name != "propose_task_reply" {
		t.Fatal(tools, err)
	}
	for key, value := range map[string]string{
		"X-Linx-Device-Id": "aa:bb:cc:dd:ee:01", "X-Linx-Agent-Id": "agent",
		"X-Linx-Session-Id": "voice-1", "X-Linx-Request-Id": "req-1",
		mcpLogTraceHeader: "forged-trace", mcpLogRPCHeader: "forged-rpc", mcpLogConnectionHeader: "99999", mcpLogTransportHeader: "forged-transport",
	} {
		headers.set(key, value)
	}
	const original = "  最终识别原文🙂 /private/audio\n"
	result := callMCP(t, env, client, map[string]any{"text": original}, "")
	if result.Text == nil || *result.Text != original || result.Status != "awaiting_confirmation" {
		t.Fatal(result)
	}
	fw.expectPush(t, "request.proposed")
	callMCP(t, env, client, map[string]any{"text": original}, "")
	callMCP(t, env, client, map[string]any{"text": "变化"}, "CONFLICT")
	headers.set("X-Linx-Request-Id", "")
	callMCP(t, env, client, map[string]any{"text": original}, "INVALID_ARGUMENT")
	headers.set("X-Linx-Request-Id", "req-1")
	headers.set("X-Linx-Agent-Id", "other")
	callMCP(t, env, client, map[string]any{"text": original}, "PERMISSION_DENIED")
	headers.set("X-Linx-Agent-Id", "agent")
	callMCP(t, env, client, map[string]any{"text": " \n"}, "INVALID_ARGUMENT")
	headers.mu.Lock()
	headers.header["X-Linx-Request-Id"] = []string{"req-1", "req-other"}
	headers.mu.Unlock()
	callMCP(t, env, client, map[string]any{"text": original}, "INVALID_ARGUMENT")
	headers.set("X-Linx-Request-Id", "req-1")
	if reply := fw.fwReq(t, "request.cancel", map[string]any{"request_id": "req-1"}); !reply.OK {
		t.Fatal(reply)
	}
	if reply := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "req-2", "agent_session_id": "sess-1"}); !reply.OK {
		t.Fatal(reply)
	}
	callMCP(t, env, client, map[string]any{"text": original}, "CONFLICT")
	headers.set("X-Linx-Request-Id", "req-2")
	headers.set("X-Linx-Session-Id", "voice-2")
	if result := callMCP(t, env, client, map[string]any{"text": original}, ""); result.RequestID != "req-2" {
		t.Fatal(result)
	}
	client.Close()
	records := waitLog(t, capture, "mcp.call.completed")
	received, completed := map[string]bool{}, 0
	for _, record := range records {
		if record["event"] == "mcp.call.received" {
			received[record["trace_id"].(string)] = true
		}
		if record["event"] == "mcp.call.completed" {
			completed++
			if !received[record["trace_id"].(string)] || record["transport"] != "streamable_http" || record["mcp_connection_id"] == float64(0) || record["rpc_id"] == "" {
				t.Fatal("日志未关联到真实工具调用", record)
			}
		}
	}
	if completed != 9 {
		t.Fatal("工具结果条数", completed)
	}
	for _, secret := range []string{original, "最终识别原文", "/private/audio", "mcp-token", "dev-token", "dt-token", client.ID(), "forged-trace", "forged-rpc", "forged-transport"} {
		if secret != "" && strings.Contains(capture.snapshot(), secret) {
			t.Fatal("泄露私人内容或伪造诊断信息", secret)
		}
	}
}

func TestMCPStreamableSessionsAndGuards(t *testing.T) {
	env, capture := streamableEnv(t, 1)
	init := streamHTTP(t, env, "POST", "", "mcp-token", streamInitialize, nil)
	if init.Code != 200 || init.Header().Get(mcpSessionHeader) == "" || init.Header().Get("Content-Type") != "application/json" {
		t.Fatal(init.Code, init.Body.String())
	}
	id := init.Header().Get(mcpSessionHeader)
	for _, tc := range []struct {
		name, method, session, token, body string
		status                             int
		change                             func(*http.Request)
	}{
		{"错误凭据", "POST", id, "dev-token", streamToolsList, 401, nil},
		{"跨凭据会话", "POST", id, "other-token", streamToolsList, 403, nil},
		{"不存在会话", "POST", "private-session", "mcp-token", streamToolsList, 404, nil},
		{"缺少初始化", "POST", "", "mcp-token", streamToolsList, 400, nil},
		{"连接上限", "POST", "", "mcp-token", streamInitialize, 503, nil},
		{"非法JSON", "POST", id, "mcp-token", "[", 400, nil},
		{"帧超限", "POST", id, "mcp-token", strings.Repeat("x", 65537), 413, nil},
		{"Origin", "POST", id, "mcp-token", streamToolsList, 403, func(r *http.Request) { r.Header.Set("Origin", "https://example.com") }},
		{"缺少Accept", "POST", id, "mcp-token", streamToolsList, 400, func(r *http.Request) { r.Header.Del("Accept") }},
		{"ContentType", "POST", id, "mcp-token", streamToolsList, 415, func(r *http.Request) { r.Header.Set("Content-Type", "text/plain") }},
		{"不支持版本", "POST", id, "mcp-token", streamToolsList, 400, func(r *http.Request) { r.Header.Set("MCP-Protocol-Version", "invalid") }},
		{"重复传输头", "POST", id, "mcp-token", streamToolsList, 400, func(r *http.Request) { r.Header.Add(mcpSessionHeader, "other") }},
		{"HEAD探测", "HEAD", "", "mcp-token", "", 200, nil},
		{"OPTIONS探测", "OPTIONS", "", "mcp-token", "", 204, nil},
		{"DELETE缺少会话", "DELETE", "", "mcp-token", "", 400, nil},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if w := streamHTTP(t, env, tc.method, tc.session, tc.token, tc.body, tc.change); w.Code != tc.status {
				t.Fatal(w.Code, w.Body.String())
			}
		})
	}
	if w := streamHTTP(t, env, "POST", id, "mcp-token", streamToolsList, nil); w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	if w := streamHTTP(t, env, "GET", "", "mcp-token", "", nil); w.Code != 503 {
		t.Fatal("旧 SSE 应与 Streamable 共用连接上限", w.Code)
	}
	if w := streamHTTP(t, env, "OPTIONS", "", "mcp-token", "", nil); w.Header().Get("Allow") != "GET, POST, DELETE, HEAD, OPTIONS" || w.Body.Len() != 0 {
		t.Fatal(w.Header(), w.Body.String())
	}
	// GET 携带标准会话头进入 Streamable SSE，并在取消 HTTP 请求后释放该流。
	ctx, cancel := context.WithCancel(t.Context())
	r, _ := http.NewRequestWithContext(ctx, "GET", env.baseHTTP+"/mcp", nil)
	r.Header.Set("Authorization", "Bearer mcp-token")
	r.Header.Set("Accept", "text/event-stream")
	r.Header.Set(mcpSessionHeader, id)
	resp, err := http.DefaultClient.Do(r)
	if err != nil {
		cancel()
		t.Fatal(err)
	}
	if resp.StatusCode != 200 {
		cancel()
		t.Fatal(resp.StatusCode)
	}
	cancel()
	resp.Body.Close()
	if w := streamHTTP(t, env, "DELETE", id, "mcp-token", "", nil); w.Code != 204 {
		t.Fatal(w.Code)
	}
	if w := streamHTTP(t, env, "POST", id, "mcp-token", streamToolsList, nil); w.Code != 404 {
		t.Fatal(w.Code)
	}
	if w := streamHTTP(t, env, "POST", "", "mcp-token", streamInitialize, nil); w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	env.server.CloseMCP()
	if w := streamHTTP(t, env, "POST", "", "mcp-token", streamInitialize, nil); w.Code != 503 {
		t.Fatal(w.Code)
	}
	for range env.server.mcp.streamSDK.Sessions() {
		t.Fatal("停止后仍保留 Streamable 会话")
	}
	logs := capture.snapshot()
	for _, method := range []string{"HEAD", "OPTIONS", "DELETE"} {
		if !strings.Contains(logs, fmt.Sprintf(`"method":"%s"`, method)) {
			t.Fatal("缺少真实HTTP方法", method)
		}
	}
	for _, secret := range []string{id, "mcp-token", "other-token", "private-session"} {
		if strings.Contains(logs, secret) {
			t.Fatal("传输日志泄露", secret)
		}
	}
}

func TestMCPStreamableResponseAndIdentifiers(t *testing.T) {
	w := &mcpHTTPResponse{ResponseWriter: httptest.NewRecorder()}
	if w.statusCode() != 200 {
		t.Fatal(w.statusCode())
	}
	if _, err := io.WriteString(w, "ok"); err != nil {
		t.Fatal(err)
	}
	w.WriteHeader(500)
	w.Flush()
	if w.statusCode() != 200 {
		t.Fatal(w.statusCode())
	}
	if mcpSessionLogID("") != 0 || mcpSessionLogID("one") == mcpSessionLogID("two") {
		t.Fatal("诊断编号错误")
	}
	if safeMCPRPCMethod("private-custom-method") != "OTHER" {
		t.Fatal("自定义方法应脱敏")
	}
}
