package transport

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
)

// 每次 POST 快照独立 Header，模拟灵矽在复用 SSE 时透传逐轮 ID。
type mcpHeaders struct {
	mu     sync.Mutex
	header http.Header
}

func (h *mcpHeaders) RoundTrip(r *http.Request) (*http.Response, error) {
	r = r.Clone(r.Context())
	h.mu.Lock()
	for key, values := range h.header {
		r.Header[key] = append([]string(nil), values...)
	}
	h.mu.Unlock()
	return http.DefaultTransport.RoundTrip(r)
}
func (h *mcpHeaders) set(key, value string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if value == "" {
		h.header.Del(key)
	} else {
		h.header.Set(key, value)
	}
}

func newMCPClient(t *testing.T, env *testEnv) (*mcp.ClientSession, *mcpHeaders) {
	t.Helper()
	headers := &mcpHeaders{header: http.Header{}}
	for key, value := range map[string]string{
		"Authorization": "Bearer mcp-token", "X-Linx-Device-Id": "aa-bb-cc-dd-ee-01",
		"X-Linx-Agent-Id": "agent", "X-Linx-Session-Id": "voice-session", "X-Linx-Request-Id": "req-1",
	} {
		headers.set(key, value)
	}
	client := mcp.NewClient(&mcp.Implementation{Name: "灵矽模拟端", Version: "1"}, nil)
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	t.Cleanup(cancel)
	session, err := client.Connect(ctx, &mcp.SSEClientTransport{Endpoint: env.baseHTTP + "/mcp", HTTPClient: &http.Client{Transport: headers}}, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { session.Close() })
	return session, headers
}

func newMCPEnv(t *testing.T) *testEnv {
	t.Helper()
	return newTestEnv(t, func(cfg *config.Config) {
		cfg.Limits.MaxFrameBytes = 65536
		cfg.Credentials.MCP = []config.MCPCredential{{Token: "mcp-token", DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"}}
	})
}

func callMCP(t *testing.T, env *testEnv, session *mcp.ClientSession, args any, code string) contracts.Request {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 3*time.Second)
	defer cancel()
	r, err := session.CallTool(ctx, &mcp.CallToolParams{Name: "propose_task_reply", Arguments: args})
	if err != nil {
		t.Fatal(err)
	}
	if len(r.Content) != 1 || r.IsError != (code != "") {
		t.Fatalf("MCP envelope 错误: %+v", r)
	}
	text, ok := r.Content[0].(*mcp.TextContent)
	if !ok {
		t.Fatal("MCP 返回不是文字")
	}
	if err := env.server.validator.ValidateMCPResult([]byte(text.Text)); err != nil {
		t.Fatal(err)
	}
	var body struct {
		OK    bool              `json:"ok"`
		Data  contracts.Request `json:"data"`
		Error *contracts.Error  `json:"error"`
	}
	if err := json.Unmarshal([]byte(text.Text), &body); err != nil {
		t.Fatal(err)
	}
	if body.OK != (code == "") || (code != "" && (body.Error == nil || body.Error.Code != code)) {
		t.Fatalf("业务返回错误: %s", text.Text)
	}
	return body.Data
}

func TestMCPSSEOriginalASRFlow(t *testing.T) {
	env := newMCPEnv(t)
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if reply := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
		t.Fatal("设备 hello 失败", reply)
	}
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	if r := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !r.OK {
		t.Fatal(r)
	}
	if r := dt.dtReq(t, "agent_sessions.sync", map[string]any{"items": []any{sessionPayload(1)}}); !r.OK {
		t.Fatal(r)
	}
	if r := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "req-1", "agent_session_id": "sess-1"}); !r.OK {
		t.Fatal(r)
	}
	session, headers := newMCPClient(t, env)
	list, err := session.ListTools(t.Context(), nil)
	if err != nil || len(list.Tools) != 1 || list.Tools[0].Name != "propose_task_reply" {
		t.Fatalf("工具注册错误: %+v %v", list, err)
	}
	input, err := json.Marshal(list.Tools[0].InputSchema)
	if err != nil || strings.Contains(string(input), "$ref") || strings.Contains(string(input), "request_id") {
		t.Fatalf("inputSchema 不能依赖远程引用或包含上下文: %s %v", input, err)
	}
	// 普通闲聊也作为原文保存，没有意图分类或改写。
	text := "  你好，今天聊点什么？🙂\n"
	r := callMCP(t, env, session, map[string]any{"text": text}, "")
	if r.Text == nil || *r.Text != text || r.Status != "awaiting_confirmation" {
		t.Fatalf("原文未保存: %+v", r)
	}
	push := fw.expectPush(t, "request.proposed")
	if err := env.server.validator.ValidateFirmwarePush("request.proposed", push); err != nil {
		t.Fatal(err)
	}
	callMCP(t, env, session, map[string]any{"text": text}, "")
	callMCP(t, env, session, map[string]any{"text": "改写"}, "CONFLICT")
	for _, args := range []any{map[string]any{}, map[string]any{"text": " \n"}, map[string]any{"text": "你好", "request_id": "req-1"}, map[string]any{"text": strings.Repeat("中", 5462)}} {
		callMCP(t, env, session, args, "INVALID_ARGUMENT")
	}
	for _, entry := range []struct{ key, value, code string }{
		{"X-Linx-Request-Id", "", "INVALID_ARGUMENT"},
		{"X-Linx-Request-Id", "missing", "NOT_FOUND"},
		{"X-Linx-Device-Id", "AA:BB:CC:DD:EE:02", "PERMISSION_DENIED"},
		{"X-Linx-Agent-Id", "other", "PERMISSION_DENIED"},
		{"X-Linx-Session-Id", "other", "CONFLICT"},
		{"X-Linx-Session-Id", "", "INVALID_ARGUMENT"},
	} {
		headers.mu.Lock()
		old := headers.header.Get(entry.key)
		headers.mu.Unlock()
		headers.set(entry.key, entry.value)
		callMCP(t, env, session, map[string]any{"text": text}, entry.code)
		headers.set(entry.key, old)
	}
	if response := fw.fwReq(t, "request.get", map[string]any{"request_id": "req-1"}); !response.OK {
		t.Fatal("未确认草稿应可读取", response)
	}
	if response := fw.fwReq(t, "request.cancel", map[string]any{"request_id": "req-1"}); !response.OK {
		t.Fatal(response)
	}
	if response := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "req-2", "agent_session_id": "sess-1"}); !response.OK {
		t.Fatal(response)
	}
	callMCP(t, env, session, map[string]any{"text": text}, "CONFLICT")
	headers.set("X-Linx-Request-Id", "req-2")
	r = callMCP(t, env, session, map[string]any{"text": "第二条语音"}, "")
	if r.RequestID != "req-2" {
		t.Fatal("复用 SSE 后用了旧请求 ID")
	}
	if response := fw.fwReq(t, "request.get", map[string]any{"request_id": "req-2"}); !response.OK {
		t.Fatal(response)
	}
}

func TestMCPHTTPGuards(t *testing.T) {
	env := newMCPEnv(t)
	for _, tc := range []struct {
		method, path, token, origin, body string
		code                              int
	}{
		{"GET", "/mcp", "", "", "", 401},
		{"GET", "/mcp", "dev-token", "", "", 401},
		{"GET", "/mcp", "dt-token", "", "", 401},
		{"GET", "/mcp", "mcp-token", "https://example.com", "", 403},
		{"DELETE", "/mcp", "mcp-token", "", "", 400},
		{"GET", "/mcp/message", "mcp-token", "", "", 405},
		{"POST", "/mcp/message", "mcp-token", "", "{}", 404},
	} {
		r := httptest.NewRequest(tc.method, tc.path, strings.NewReader(tc.body))
		if tc.token != "" {
			r.Header.Set("Authorization", "Bearer "+tc.token)
		}
		r.Header.Set("Origin", tc.origin)
		w := httptest.NewRecorder()
		env.server.Handler().ServeHTTP(w, r)
		if w.Code != tc.code {
			t.Fatalf("%s %s: %d != %d", tc.method, tc.path, w.Code, tc.code)
		}
	}
	// 尚未组装服务的入口必须明确失败。
	w := httptest.NewRecorder()
	(&Server{}).handleMCP(w, httptest.NewRequest("GET", "/mcp", nil))
	if w.Code != 503 {
		t.Fatal(w.Code)
	}
}

func TestMCPToolMissingContext(t *testing.T) {
	env := newMCPEnv(t)
	r, err := env.server.mcp.propose(t.Context(), &mcp.CallToolRequest{})
	if err != nil || !r.IsError {
		t.Fatal("缺少上下文未拒绝")
	}
	if singleHeader(http.Header{"X-Linx-Request-Id": []string{"one", "two"}}, "X-Linx-Request-Id") != "" {
		t.Fatal("重复头不应被接受")
	}
}

func TestMCPSSESessionIsolationAndLimits(t *testing.T) {
	// 配置在建连前固定，避免活跃 SSE goroutine 与测试中的配置写入竞争。
	env := newTestEnv(t, func(cfg *config.Config) {
		cfg.Limits.MaxFrameBytes = 65536
		cfg.Limits.MaxOnlineConnections = 1
		cfg.Credentials.MCP = []config.MCPCredential{
			{Token: "mcp-token", DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"},
			{Token: "other-token", DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "other"},
		}
	})
	session, headers := newMCPClient(t, env)
	_ = session
	env.server.mcp.mu.Lock()
	var id string
	for key := range env.server.mcp.sessions {
		id = key
	}
	env.server.mcp.mu.Unlock()
	for _, tc := range []struct {
		body string
		code int
	}{
		{"[", 400}, {`{"jsonrpc":"1.0"}`, 400}, {strings.Repeat("x", 65537), 413},
	} {
		r := httptest.NewRequest("POST", "/mcp/message?sessionid="+id, strings.NewReader(tc.body))
		r.Header.Set("Authorization", "Bearer mcp-token")
		w := httptest.NewRecorder()
		env.server.Handler().ServeHTTP(w, r)
		if w.Code != tc.code {
			t.Fatalf("%d != %d", w.Code, tc.code)
		}
	}
	// 同设备的另一个智能体凭据也不能投递到已建立的 MCP 会话。
	r := httptest.NewRequest("POST", "/mcp/message?sessionid="+id, strings.NewReader("{}"))
	r.Header.Set("Authorization", "Bearer other-token")
	w := httptest.NewRecorder()
	env.server.Handler().ServeHTTP(w, r)
	if w.Code != 403 {
		t.Fatal(w.Code)
	}
	req, err := http.NewRequestWithContext(t.Context(), "GET", env.baseHTTP+"/mcp", nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer mcp-token")
	response, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	io.Copy(io.Discard, response.Body)
	response.Body.Close()
	if response.StatusCode != 503 {
		t.Fatal(response.StatusCode)
	}
	headers.set("Authorization", "Bearer wrong")
	ctx, cancel := context.WithTimeout(t.Context(), time.Second)
	defer cancel()
	if _, err := session.ListTools(ctx, nil); err == nil {
		t.Fatal("无效凭据仍能访问已存在会话")
	}
}
