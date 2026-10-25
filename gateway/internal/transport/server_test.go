package transport

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

// testServerConfig 构造单测用配置（内存数据库路径 + 测试凭据）。
func testServerConfig(t *testing.T) *config.Config {
	t.Helper()
	return &config.Config{
		Database: config.DatabaseConfig{Path: filepath.Join(t.TempDir(), "gw.db")},
		Credentials: config.CredentialsConfig{
			Devices: []config.DeviceCredential{
				{Token: "dev-token", DeviceID: "AA:BB:CC:DD:EE:01", Subject: "alice"},
			},
			Desktops: []config.DesktopCredential{
				{Token: "dt-token", ClientID: "desktop-1", Subject: "alice"},
				{Token: "bob-token", ClientID: "desktop-bob", Subject: "bob"},
				{Token: "dt2-token", ClientID: "desktop-2", Subject: "alice"},
			},
		},
		Pairing: config.PairingConfig{Rules: []config.PairingRule{
			{Subject: "alice", DeviceIDs: []string{"AA:BB:CC:DD:EE:01"}},
		}},
		Limits: config.LimitsConfig{
			MaxFrameBytes: 4096, SessionPageSize: 5, MaxCatalogItems: 200,
			MaxOnlineConnections: 16, SendQueueSize: 8, SendTimeout: config.Duration(2 * time.Second),
		},
		Timeouts: config.TimeoutConfig{ShutdownGrace: config.Duration(2 * time.Second)},
	}
}

// testEnv 是传输层测试环境。
type testEnv struct {
	cfg      *config.Config
	server   *Server
	ts       *httptest.Server
	baseWS   string
	baseHTTP string
}

func newTestEnv(t *testing.T, mutate func(*config.Config)) *testEnv {
	return newTestEnvWithLogger(t, mutate, slog.New(slog.DiscardHandler))
}

func newTestEnvWithLogger(t *testing.T, mutate func(*config.Config), logger *slog.Logger) *testEnv {
	t.Helper()
	cfg := testServerConfig(t)
	if mutate != nil {
		mutate(cfg)
	}
	validator, err := contracts.NewValidator()
	if err != nil {
		t.Fatal(err)
	}
	db, err := storage.Open(cfg.Database.Path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	repos := storage.NewRepositories(db.DB())
	conns := lifecycle.NewConnections(logger, cfg.Limits)
	server := NewServer(cfg, logger, validator, NewAuthenticator(cfg), conns)
	binding := service.NewBindingService(repos, cfg, server, logger)
	sessions := service.NewSessionService(repos, cfg, server, logger)
	server.SetServices(binding, sessions)
	if err := server.SetRequests(service.NewRequestService(repos, cfg, server)); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(server.CloseMCP)

	ts := httptest.NewServer(server.Handler())
	t.Cleanup(ts.Close)
	return &testEnv{
		cfg:      cfg,
		server:   server,
		ts:       ts,
		baseWS:   "ws" + trimHTTP(ts.URL),
		baseHTTP: "http" + trimHTTP(ts.URL),
	}
}

func trimHTTP(s string) string { return s[len("http"):] }

// wsClient 是测试 WebSocket 客户端，response 按 reply 键分发。
type wsClient struct {
	pendingMu sync.Mutex
	writeMu   sync.Mutex
	conn      *websocket.Conn
	pending   map[string]chan json.RawMessage
	pushes    chan json.RawMessage
}

func dialWS(t *testing.T, url, token string) *wsClient {
	t.Helper()
	header := http.Header{}
	header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token."+token)
	conn, _, err := websocket.DefaultDialer.Dial(url, header)
	if err != nil {
		t.Fatalf("拨号失败: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	c := &wsClient{conn: conn, pending: map[string]chan json.RawMessage{}, pushes: make(chan json.RawMessage, 64)}
	go c.readLoop()
	return c
}

// dialRawWS 拨号但不启动后台读协程；供需要自行 ReadMessage 的测试使用。
func dialRawWS(t *testing.T, url, token string) *websocket.Conn {
	t.Helper()
	header := http.Header{}
	header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token."+token)
	conn, _, err := websocket.DefaultDialer.Dial(url, header)
	if err != nil {
		t.Fatalf("拨号失败: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return conn
}

func (c *wsClient) readLoop() {
	for {
		_, data, err := c.conn.ReadMessage()
		if err != nil {
			return
		}
		var env struct {
			Type    string `json:"type"`
			ReplyTo string `json:"reply_to"`
			Payload struct {
				ReplyToMessageID string `json:"reply_to_message_id"`
			} `json:"payload"`
		}
		if err := json.Unmarshal(data, &env); err != nil {
			continue
		}
		if env.Type == "response" || env.Payload.ReplyToMessageID != "" {
			key := env.ReplyTo
			if key == "" {
				key = env.Payload.ReplyToMessageID
			}
			c.pendingMu.Lock()
			ch := c.pending[key]
			c.pendingMu.Unlock()
			if ch != nil {
				select {
				case ch <- data:
				default:
				}
			}
			continue
		}
		select {
		case c.pushes <- data:
		default:
		}
	}
}

// fwReq 发送固件请求并返回解析后的响应。
func (c *wsClient) fwReq(t *testing.T, typ string, payload map[string]any) *fwResp {
	t.Helper()
	id := contracts.NewMessageID("t_")
	ch := make(chan json.RawMessage, 1)
	c.pendingMu.Lock()
	c.pending[id] = ch
	c.pendingMu.Unlock()
	defer func() { c.pendingMu.Lock(); delete(c.pending, id); c.pendingMu.Unlock() }()
	frame := map[string]any{"id": id, "type": typ, "payload": payload}
	if err := c.writeFrame(frame); err != nil {
		t.Fatal(err)
	}
	select {
	case raw := <-ch:
		var env struct {
			Payload struct {
				OK    bool `json:"ok"`
				Error *struct {
					Code    string `json:"code"`
					Message string `json:"message"`
				} `json:"error"`
				Data json.RawMessage `json:"data"`
			} `json:"payload"`
		}
		if err := json.Unmarshal(raw, &env); err != nil {
			t.Fatal(err)
		}
		out := &fwResp{OK: env.Payload.OK}
		if env.Payload.Error != nil {
			out.Code = env.Payload.Error.Code
			out.Message = env.Payload.Error.Message
		}
		out.RawData = env.Payload.Data
		return out
	case <-time.After(5 * time.Second):
		t.Fatalf("请求 %s 超时", typ)
		return nil
	}
}

// dtReq 发送 Desktop 消息并等待应答。
func (c *wsClient) dtReq(t *testing.T, typ string, payload map[string]any) *dtResp {
	t.Helper()
	mid := contracts.NewMessageID("m_")
	ch := make(chan json.RawMessage, 1)
	c.pendingMu.Lock()
	c.pending[mid] = ch
	c.pendingMu.Unlock()
	defer func() { c.pendingMu.Lock(); delete(c.pending, mid); c.pendingMu.Unlock() }()
	frame := map[string]any{
		"protocol_version": "1.0",
		"message_id":       mid,
		"sequence":         1,
		"type":             typ,
		"sent_at":          time.Now().UTC().Format(time.RFC3339),
		"payload":          payload,
	}
	if err := c.writeFrame(frame); err != nil {
		t.Fatal(err)
	}
	select {
	case raw := <-ch:
		var env struct {
			Payload struct {
				ReplyToMessageID string `json:"reply_to_message_id"`
				OK               bool   `json:"ok"`
				Error            *struct {
					Code    string `json:"code"`
					Message string `json:"message"`
				} `json:"error"`
				Data json.RawMessage `json:"data"`
			} `json:"payload"`
		}
		if err := json.Unmarshal(raw, &env); err != nil {
			t.Fatal(err)
		}
		out := &dtResp{OK: env.Payload.OK, ReplyTo: env.Payload.ReplyToMessageID}
		if env.Payload.Error != nil {
			out.Code = env.Payload.Error.Code
			out.Message = env.Payload.Error.Message
		}
		out.RawData = env.Payload.Data
		return out
	case <-time.After(5 * time.Second):
		t.Fatalf("消息 %s 应答超时", typ)
		return nil
	}
}

// expectPush 等待固件推送。
func (c *wsClient) expectPush(t *testing.T, typ string) json.RawMessage {
	t.Helper()
	deadline := time.After(3 * time.Second)
	for {
		select {
		case raw := <-c.pushes:
			var env struct {
				Type string `json:"type"`
			}
			_ = json.Unmarshal(raw, &env)
			if env.Type == typ {
				return raw
			}
		case <-deadline:
			t.Fatalf("未收到推送 %s", typ)
			return nil
		}
	}
}

// dtNoReply 发送 Desktop 消息并断言指定 message_id 不应收到应答（校验拒绝场景）。
func (c *wsClient) dtNoReply(t *testing.T, typ string, payload map[string]any, mid string) {
	t.Helper()
	ch := make(chan json.RawMessage, 1)
	c.pendingMu.Lock()
	c.pending[mid] = ch
	c.pendingMu.Unlock()
	defer func() { c.pendingMu.Lock(); delete(c.pending, mid); c.pendingMu.Unlock() }()
	frame := map[string]any{
		"protocol_version": "1.0",
		"message_id":       mid,
		"sequence":         1,
		"type":             typ,
		"sent_at":          time.Now().UTC().Format(time.RFC3339),
		"payload":          payload,
	}
	if err := c.writeFrame(frame); err != nil {
		t.Fatal(err)
	}
	select {
	case raw := <-ch:
		t.Fatalf("不应收到应答: %s", raw)
	case <-time.After(800 * time.Millisecond):
	}
}

// sendRaw 发送无应答期望的 Desktop 消息。
func (c *wsClient) sendRaw(t *testing.T, typ string, payload map[string]any) {
	t.Helper()
	frame := map[string]any{
		"protocol_version": "1.0",
		"message_id":       contracts.NewMessageID("m_"),
		"sequence":         1,
		"type":             typ,
		"sent_at":          time.Now().UTC().Format(time.RFC3339),
		"payload":          payload,
	}
	if err := c.writeFrame(frame); err != nil {
		t.Fatal(err)
	}
}

type fwResp struct {
	OK      bool
	Code    string
	Message string
	RawData json.RawMessage
}

type dtResp struct {
	OK      bool
	Code    string
	Message string
	ReplyTo string
	RawData json.RawMessage
}

func sessionPayload(i int) map[string]any {
	return map[string]any{
		"agent_session_id": "sess-" + string(rune('0'+i)),
		"title":            "会话",
		"project_name":     "proj",
		"agent":            "codex",
		"status":           "idle",
		"summary":          "",
		"thread_id":        "thread",
		"project_id":       "prj",
	}
}

func TestHTTPHealth(t *testing.T) {
	env := newTestEnv(t, nil)
	// readyz 在未启动时为 503。
	resp, err := http.Get(env.baseHTTP + "/readyz")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("未启动 readyz 应为 503: %d", resp.StatusCode)
	}
	// healthz 始终 200。
	resp, err = http.Get(env.baseHTTP + "/healthz")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("healthz 应为 200: %d", resp.StatusCode)
	}
}

func TestFullFirmwareDesktopFlow(t *testing.T) {
	env := newTestEnv(t, nil)

	// 固件连接：hello 未绑定。
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	resp := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "aa:bb:cc:dd:ee:01"})
	if !resp.OK {
		t.Fatalf("hello 失败: %s %s", resp.Code, resp.Message)
	}
	var binding contracts.Binding
	if err := json.Unmarshal(resp.RawData, &binding); err != nil {
		t.Fatal(err)
	}
	if binding.DesktopClientID != nil {
		t.Fatalf("未绑定应为 null: %v", *binding.DesktopClientID)
	}

	// Desktop 连接：hello（无应答）、bind。
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	dt.sendRaw(t, "client.hello", map[string]any{
		"desktop_version":     "t",
		"protocol_versions":   []string{"1.0"},
		"last_acked_sequence": 0,
		"capabilities":        []string{},
	})
	bindResp := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "aa:bb:cc:dd:ee:01"})
	if !bindResp.OK {
		t.Fatalf("bind 失败: %s %s", bindResp.Code, bindResp.Message)
	}
	if bindResp.ReplyTo == "" {
		t.Fatal("应答缺少 reply_to_message_id")
	}
	var bound contracts.BoundBinding
	if err := json.Unmarshal(bindResp.RawData, &bound); err != nil {
		t.Fatal(err)
	}
	if bound.DesktopClientID != "desktop-1" {
		t.Fatalf("绑定 Desktop 错误: %+v", bound)
	}

	// 固件收到 device.bound。
	raw := fw.expectPush(t, "device.bound")
	var push struct {
		Payload contracts.BoundBinding `json:"payload"`
	}
	if err := json.Unmarshal(raw, &push); err != nil {
		t.Fatal(err)
	}
	if push.Payload.DesktopClientID != "desktop-1" {
		t.Fatalf("推送 Desktop 错误: %+v", push.Payload)
	}

	// Desktop 按非 ID 顺序同步目录；固件收到 updated 推送。
	items := make([]any, 0, 6)
	for _, i := range []int{6, 2, 5, 1, 4, 3} {
		items = append(items, sessionPayload(i))
	}
	syncResp := dt.dtReq(t, "agent_sessions.sync", map[string]any{"items": items})
	if !syncResp.OK {
		t.Fatalf("sync 失败: %s %s", syncResp.Code, syncResp.Message)
	}
	_ = fw.expectPush(t, "agent_sessions.updated")

	// 固件分页读取。
	page1 := fw.fwReq(t, "agent_sessions.list", map[string]any{})
	if !page1.OK {
		t.Fatalf("list 失败: %s %s", page1.Code, page1.Message)
	}
	var p1 struct {
		Items      []contracts.AgentSession `json:"items"`
		NextCursor *string                  `json:"next_cursor"`
	}
	if err := json.Unmarshal(page1.RawData, &p1); err != nil {
		t.Fatal(err)
	}
	if len(p1.Items) != 5 || p1.NextCursor == nil {
		t.Fatalf("第一页错误: %d 项, next=%v", len(p1.Items), p1.NextCursor)
	}
	page2 := fw.fwReq(t, "agent_sessions.list", map[string]any{"cursor": *p1.NextCursor})
	var p2 struct {
		Items      []contracts.AgentSession `json:"items"`
		NextCursor *string                  `json:"next_cursor"`
	}
	if err := json.Unmarshal(page2.RawData, &p2); err != nil {
		t.Fatal(err)
	}
	if len(p2.Items) != 1 || p2.NextCursor != nil {
		t.Fatalf("第二页错误: %d 项, next=%v", len(p2.Items), p2.NextCursor)
	}
	for i, item := range append(p1.Items, p2.Items...) {
		if want := items[i].(map[string]any)["agent_session_id"]; item.AgentSessionID != want {
			t.Fatalf("固件目录未保留 Desktop 顺序：第 %d 项 got=%s want=%s", i, item.AgentSessionID, want)
		}
	}

	// 单条会话。
	get := fw.fwReq(t, "agent_session.get", map[string]any{"agent_session_id": "sess-3"})
	if !get.OK {
		t.Fatalf("get 失败: %s %s", get.Code, get.Message)
	}
	// 缺失 → NOT_FOUND。
	missing := fw.fwReq(t, "agent_session.get", map[string]any{"agent_session_id": "nope"})
	if missing.OK || missing.Code != "NOT_FOUND" {
		t.Fatalf("缺失会话错误码错误: %+v", missing)
	}
	// 未开放操作 → 明确拒绝（入站消息合法但操作未开放）。
	unopened := fw.fwReq(t, "request.get", map[string]any{"request_id": "not-started"})
	if unopened.OK {
		t.Fatal("未开放操作不应成功")
	}
}

func TestBindConflictAndDenied(t *testing.T) {
	env := newTestEnv(t, nil)
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if hello := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !hello.OK {
		t.Fatal("设备应先上线")
	}
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	bind := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "aa:bb:cc:dd:ee:01"})
	if !bind.OK {
		t.Fatalf("首次绑定失败: %s %s", bind.Code, bind.Message)
	}
	// 幂等。
	bind = dt.dtReq(t, "device.bind", map[string]any{"mac_address": "aa:bb:cc:dd:ee:01"})
	if !bind.OK {
		t.Fatalf("幂等绑定失败: %s %s", bind.Code, bind.Message)
	}
	// 无配对权账号拒绝。
	bob := dialWS(t, env.baseWS+"/v1/clients/connect", "bob-token")
	bind = bob.dtReq(t, "device.bind", map[string]any{"mac_address": "aa:bb:cc:dd:ee:01"})
	if bind.OK {
		t.Fatal("bob 不应有配对权")
	}
	if bind.Code != "PERMISSION_DENIED" {
		t.Fatalf("错误码错误: %s", bind.Code)
	}
	// 非法 MAC：信封层 Schema 校验即拒绝（MAC 模式不匹配），不应答。
	bad := contracts.NewMessageID("m_")
	bob.dtNoReply(t, "device.bind", map[string]any{"mac_address": "bad"}, bad)
}

func TestSchemaInvalidAndUnknown(t *testing.T) {
	env := newTestEnv(t, nil)
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if hello := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !hello.OK {
		t.Fatal("设备应先上线")
	}
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	// 缺 sent_at 等必填 → 校验拒绝，无应答。
	mid := contracts.NewMessageID("m_")
	frame := map[string]any{
		"protocol_version": "1.0",
		"message_id":       mid,
		"sequence":         1,
		"type":             "client.hello",
		"payload":          map[string]any{},
	}
	if err := dt.conn.WriteJSON(frame); err != nil {
		t.Fatal(err)
	}
	// 未知类型：被记录，不崩溃、不退出。
	frame = map[string]any{
		"protocol_version": "1.0",
		"message_id":       contracts.NewMessageID("m_"),
		"sequence":         2,
		"type":             "no.such.type",
		"sent_at":          time.Now().UTC().Format(time.RFC3339),
		"payload":          map[string]any{},
	}
	if err := dt.conn.WriteJSON(frame); err != nil {
		t.Fatal(err)
	}
	// 连接仍可用：bind 成功。
	bind := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "aa:bb:cc:dd:ee:01"})
	if !bind.OK {
		t.Fatalf("异常消息后连接应可用: %s %s", bind.Code, bind.Message)
	}
}

func TestOversizedFrameCloses(t *testing.T) {
	env := newTestEnv(t, func(c *config.Config) { c.Limits.MaxFrameBytes = 1024 })
	fw := dialRawWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	big := make(map[string]any)
	big["payload"] = map[string]any{"mac_address": "aa:bb:cc:dd:ee:01", "pad": strings.Repeat("x", 4096)}
	big["id"] = "t_big"
	big["type"] = "device.hello"
	_ = fw.WriteJSON(big) // 服务端可能已先行关闭导致写失败
	_ = fw.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, _, err := fw.ReadMessage(); err == nil {
		t.Fatal("超限帧应关闭连接")
	}
}

func TestAuthFailureClose4401(t *testing.T) {
	env := newTestEnv(t, nil)
	header := http.Header{}
	header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.wrong")
	conn, _, err := websocket.DefaultDialer.Dial(env.baseWS+"/v1/devices/connect", header)
	if err == nil {
		defer conn.Close()
		if _, _, cerr := conn.ReadMessage(); cerr == nil {
			t.Fatal("应被关闭")
		}
		return
	}
	// Dial 本身报错（握手被拒绝）也视为通过。
	closeErr, ok := err.(*websocket.CloseError) //nolint:errorlint // 测试断言
	if !ok || closeErr.Code != 4401 {
		t.Fatalf("应收到 4401 关闭码，得到 %v", err)
	}
}

func TestFirmwareCannotChangeRegisteredMAC(t *testing.T) {
	env := newTestEnv(t, nil)
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if response := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !response.OK {
		t.Fatal("初次 hello 应登记成功", response)
	}
	resp := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:00:00:00:00:FF"})
	if resp.OK {
		t.Fatal("伪造 MAC 应被拒绝")
	}
	if resp.Code != "PERMISSION_DENIED" {
		t.Fatalf("错误码错误: %s", resp.Code)
	}
}

func TestUnboundDeviceListDenied(t *testing.T) {
	env := newTestEnv(t, nil)
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if reply := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
		t.Fatal("hello 失败", reply)
	}
	resp := fw.fwReq(t, "agent_sessions.list", map[string]any{})
	if resp.OK {
		t.Fatal("未绑定设备不应读取目录")
	}
	if resp.Code != "PERMISSION_DENIED" {
		t.Fatalf("错误码错误: %s", resp.Code)
	}
}

func TestRunShutdown(t *testing.T) {
	env := newTestEnv(t, func(c *config.Config) {
		c.Listen.Addr = "127.0.0.1:0"
	})
	// Run 在真实监听上启动；此处验证立即取消返回 nil。
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(50 * time.Millisecond)
		cancel()
	}()
	if err := env.server.Run(ctx); err != nil {
		t.Fatalf("Run 应正常退出: %v", err)
	}
}

func (c *wsClient) writeFrame(frame any) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return c.conn.WriteJSON(frame)
}
