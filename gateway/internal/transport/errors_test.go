package transport

import (
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
)

// closedConn 构造一条已关闭的真实连接，用于发送失败分支。
func closedConn() *lifecycle.Connection {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		if _, err := up.Upgrade(w, r, nil); err != nil {
			return
		}
		// 服务端仅持有连接；测试结束后由客户端关闭结束。
		select {}
	}))
	defer srv.Close()
	url := "ws" + strings.TrimPrefix(srv.URL, "http")
	client, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		panic(err)
	}
	defer client.Close()
	conn := lifecycle.NewConnection(client, 4, 100*time.Millisecond, 30*time.Second, 60*time.Second, 1)
	conn.Close()
	return conn
}

func TestSendDesktopReplyErrorBranches(t *testing.T) {
	env := newTestEnv(t, nil)
	// 未知类型：出站校验失败分支。
	env.server.sendDesktopReply(closedConn(), "no.such.type", "m1", map[string]any{})
	// 已关闭连接：发送失败分支。
	env.server.sendDesktopReply(closedConn(), "binding.result", "m1",
		contracts.BindingResultPayload{ReplyToMessageID: "m1", OK: true, Data: &contracts.BoundBinding{DeviceID: "AA:BB:CC:DD:EE:01", DesktopClientID: "d1"}})
}

func TestSyncResultErrorBranch(t *testing.T) {
	env := newTestEnv(t, nil)
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	// 重复 agent_session_id → 整批拒绝 → 错误应答。
	items := []any{
		map[string]any{"agent_session_id": "s1", "title": "t", "project_name": "p", "agent": "codex", "status": "idle", "summary": "", "thread_id": "t1", "project_id": "p1"},
		map[string]any{"agent_session_id": "s1", "title": "t", "project_name": "p", "agent": "codex", "status": "idle", "summary": "", "thread_id": "t2", "project_id": "p1"},
	}
	resp := dt.dtReq(t, "agent_sessions.sync", map[string]any{"items": items})
	if resp.OK || resp.Code != "INVALID_ARGUMENT" {
		t.Fatalf("重复会话应返回 INVALID_ARGUMENT: %+v", resp)
	}
}

func TestDesktopOversizedFrame(t *testing.T) {
	env := newTestEnv(t, func(c *config.Config) { c.Limits.MaxFrameBytes = 1024 })
	dt := dialRawWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	big := make(map[string]any)
	big["protocol_version"] = "1.0"
	big["message_id"] = contracts.NewMessageID("m_")
	big["sequence"] = 1
	big["type"] = "client.hello"
	big["sent_at"] = time.Now().UTC().Format(time.RFC3339)
	big["payload"] = map[string]any{"pad": strings.Repeat("x", 4096)}
	_ = dt.WriteJSON(big)
	_ = dt.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, _, err := dt.ReadMessage(); err == nil {
		t.Fatal("超限帧应关闭连接")
	}
}

func TestDesktopNotYetOpenTypes(t *testing.T) {
	env := newTestEnv(t, nil)
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if hello := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !hello.OK {
		t.Fatal("设备应先上线")
	}
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	// task.ack / message.available 等契约已识别但首版未开放：连接不退出、不应答。
	dt.sendRaw(t, "task.ack", map[string]any{
		"message_id": "m1", "sequence": 1, "task_id": "t1", "disposition": "accepted",
	})
	dt.sendRaw(t, "message.available", map[string]any{"agent_session_id": "s1", "message_id": "m1"})
	dt.sendRaw(t, "no.such.type", map[string]any{})
	// 连接仍可用。
	bind := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "aa:bb:cc:dd:ee:01"})
	if !bind.OK {
		t.Fatalf("未开放消息后连接应可用: %s %s", bind.Code, bind.Message)
	}
}

func TestServeFirmwareReconnectAtLimit(t *testing.T) {
	env := newTestEnv(t, func(c *config.Config) { c.Limits.MaxOnlineConnections = 1 })
	first := dialRawWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	second := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if reply := second.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
		t.Fatal("达到上限时同一设备重连应替换旧连接")
	}
	first.SetReadDeadline(time.Now().Add(time.Second))
	if _, _, err := first.ReadMessage(); err == nil {
		t.Fatal("被替换的旧连接应关闭")
	}
}

func TestRespondFirmwareFallback(t *testing.T) {
	env := newTestEnv(t, nil)
	// 未知操作无对应 response 定义：走兜底 INTERNAL 应答。
	conn := closedConn()
	env.server.respondFirmware(conn, "no.such.op", "r1", nil, nil)
	// 不可序列化 data：编码失败分支。
	env.server.respondFirmware(conn, "device.hello", "r2", make(chan int), nil)
	// 可序列化但无错 data：正常路径（连接关闭则发送失败）。
	env.server.respondFirmware(conn, "device.hello", "r3", contracts.Binding{DeviceID: "AA:BB:CC:DD:EE:01"}, nil)
}

func TestSendFirmwarePushErrorBranches(t *testing.T) {
	env := newTestEnv(t, nil)
	// 未知推送类型：校验失败分支。
	env.server.sendFirmwarePush(closedConn(), "no.such.push", map[string]any{})
	// 不可序列化：编码失败分支。
	env.server.sendFirmwarePush(closedConn(), "device.bound", make(chan int))
	// 合法推送但连接已关闭：发送失败分支。
	env.server.sendFirmwarePush(closedConn(), "device.bound",
		contracts.DeviceBoundMessage{ID: "p1", Type: "device.bound", Payload: contracts.BoundBinding{DeviceID: "AA:BB:CC:DD:EE:01", DesktopClientID: "d1"}})
	// 无在线设备：Device() 为空直接返回。
	env.server.NotifyDeviceBound("AA:BB:CC:DD:EE:FF", contracts.BoundBinding{})
	env.server.NotifyCatalogUpdated("AA:BB:CC:DD:EE:FF")
}

func TestRunListenError(t *testing.T) {
	// 先占用一个端口，再让服务在其上启动 → 监听错误。
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	env := newTestEnv(t, func(c *config.Config) { c.Listen.Addr = ln.Addr().String() })
	if err := env.server.Run(t.Context()); err == nil {
		t.Fatal("端口占用应返回错误")
	}
}

func TestDesktopAuthFailureKind(t *testing.T) {
	// Desktop 认证失败路径：覆盖 kindName 的 desktop 分支。
	env := newTestEnv(t, nil)
	header := http.Header{}
	header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.wrong")
	conn, _, err := websocket.DefaultDialer.Dial(env.baseWS+"/v1/clients/connect", header)
	if err == nil {
		defer conn.Close()
		_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, _, rerr := conn.ReadMessage(); rerr == nil {
			t.Fatal("应被关闭")
		}
		return
	}
	closeErr, ok := err.(*websocket.CloseError) //nolint:errorlint // 测试断言
	if !ok || closeErr.Code != 4401 {
		t.Fatalf("应收到 4401: %v", err)
	}
}

func TestAuthErrorBranches(t *testing.T) {
	// 设备凭据 MAC 不合法。
	cfg := testServerConfig(t)
	cfg.Credentials.Devices = []config.DeviceCredential{
		{Token: "bad-mac", DeviceID: "zz", Subject: "alice"},
	}
	auth := NewAuthenticator(cfg)
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	req.Header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.bad-mac")
	if _, err := auth.Authenticate(PrincipalDevice, req); err == nil {
		t.Fatal("非法 MAC 配置应报错")
	}
	// 未知主体类型。
	if _, err := auth.Authenticate(PrincipalKind(99), req); err == nil {
		t.Fatal("未知主体类型应报错")
	}
}

func TestExtractTokenMalformed(t *testing.T) {
	r := httptest.NewRequest(http.MethodGet, "/", nil)
	r.Header.Set("Sec-WebSocket-Protocol", "linxbox-token.abc") // 无 linxbox.v1
	if _, ok := extractToken(r); ok {
		t.Fatal("缺 v1 子协议不应通过")
	}
	r.Header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.")
	if _, ok := extractToken(r); ok {
		t.Fatal("空 token 不应通过")
	}
	r.Header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.a b")
	if _, ok := extractToken(r); ok {
		t.Fatal("含空白 token 不应通过")
	}
}

func TestFirmwareOversizedFrame(t *testing.T) {
	env := newTestEnv(t, func(c *config.Config) { c.Limits.MaxFrameBytes = 1024 })
	dev := dialRawWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	big := map[string]any{"id": "f1", "type": "device.hello", "payload": map[string]any{"pad": strings.Repeat("x", 4096)}}
	_ = dev.WriteJSON(big)
	_ = dev.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, _, err := dev.ReadMessage(); err == nil {
		t.Fatal("超限帧应关闭连接")
	}
}

func TestFirmwareInvalidSchemaEnvelope(t *testing.T) {
	env := newTestEnv(t, nil)
	dev := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	// 缺 mac_address 的 device.hello：信封层被 Schema 拦截，不应答但不断开。
	dev.sendRaw(t, "device.hello", map[string]any{"mac_address": ""})
	// 连接仍可用：合法 hello 应得到应答。
	resp := dev.fwReq(t, "device.hello", map[string]any{"mac_address": "aa:bb:cc:dd:ee:01"})
	if !resp.OK {
		t.Fatalf("非法信封后连接应可用: %s %s", resp.Code, resp.Message)
	}
}

func TestFirmwareHelloPairingDenied(t *testing.T) {
	env := newTestEnv(t, nil)
	dev := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	// alice 的凭据设备 1，但 hello 声明设备 2：配对规则拒绝 → 错误应答。
	resp := dev.fwReq(t, "device.hello", map[string]any{"mac_address": "aa:bb:cc:dd:ee:02"})
	if resp.OK {
		t.Fatal("越权 hello 应失败")
	}
}

func TestDesktopSecondConnectionRejected(t *testing.T) {
	env := newTestEnv(t, func(c *config.Config) { c.Limits.MaxOnlineConnections = 1 })
	first := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	first.sendRaw(t, "client.hello", map[string]any{"desktop_version": "0.1.0", "protocol_versions": []string{"1.0"}, "last_acked_sequence": 0, "capabilities": []string{}})
	// 第二台 Desktop（另一主体）应被拒绝。
	header := http.Header{}
	header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.dt2-token")
	conn, _, err := websocket.DefaultDialer.Dial(env.baseWS+"/v1/clients/connect", header)
	if err != nil {
		return // 握手即被拒也视为通过
	}
	defer conn.Close()
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, _, rerr := conn.ReadMessage(); rerr == nil {
		t.Fatal("达上限的 Desktop 连接应被关闭")
	}
}

func TestSendDesktopReplyMarshalFail(t *testing.T) {
	env := newTestEnv(t, nil)
	// 不可序列化 payload：编码失败分支。
	env.server.sendDesktopReply(closedConn(), "binding.result", "m1", make(chan int))
}

func TestRunTLSError(t *testing.T) {
	env := newTestEnv(t, func(c *config.Config) {
		c.Listen.Addr = "127.0.0.1:0"
		c.Listen.TLSCert = "/no/such/cert.pem"
		c.Listen.TLSKey = "/no/such/key.pem"
	})
	if err := env.server.Run(t.Context()); err == nil {
		t.Fatal("TLS 证书缺失应返回错误")
	}
}

func TestUpgradeFailurePlainRequest(t *testing.T) {
	env := newTestEnv(t, nil)
	// 非 WebSocket 握手：升级失败路径。
	resp, err := http.Get(env.baseHTTP + "/v1/devices/connect")
	if err == nil {
		defer resp.Body.Close()
	}
}
