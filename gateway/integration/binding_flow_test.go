package integration

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
	"github.com/KevinHuo/LinxBox/gateway/internal/transport"
)

// flowConfig 构造全链路配置：两个主体（alice/bob）、两个设备、三个 Desktop。
func flowConfig(t *testing.T) *config.Config {
	t.Helper()
	return &config.Config{
		Database: config.DatabaseConfig{Path: filepath.Join(t.TempDir(), "gw.db")},
		Credentials: config.CredentialsConfig{
			Devices: []config.DeviceCredential{
				{Token: "alice-dev", DeviceID: "AA:BB:CC:DD:EE:01", Subject: "alice"},
				{Token: "bob-dev", DeviceID: "AA:BB:CC:DD:EE:02", Subject: "bob"},
			},
			Desktops: []config.DesktopCredential{
				{Token: "alice-dt", ClientID: "desktop-alice", Subject: "alice"},
				{Token: "bob-dt", ClientID: "desktop-bob", Subject: "bob"},
			},
		},
		Pairing: config.PairingConfig{Rules: []config.PairingRule{
			{Subject: "alice", DeviceIDs: []string{"AA:BB:CC:DD:EE:01"}},
			{Subject: "bob", DeviceIDs: []string{"AA:BB:CC:DD:EE:02"}},
		}},
		Limits: config.LimitsConfig{
			MaxFrameBytes: 64 * 1024, SessionPageSize: 5, MaxCatalogItems: 200,
			MaxOnlineConnections: 16, SendQueueSize: 8, SendTimeout: config.Duration(2 * time.Second),
		},
	}
}

// flowEnv 是集成测试环境。
type flowEnv struct {
	baseWS   string
	baseHTTP string
}

func newFlowEnv(t *testing.T) *flowEnv {
	t.Helper()
	cfg := flowConfig(t)
	logger := slog.New(slog.DiscardHandler)
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
	server := transport.NewServer(cfg, logger, validator, transport.NewAuthenticator(cfg), conns)
	server.SetServices(
		service.NewBindingService(repos, cfg, server, logger),
		service.NewSessionService(repos, cfg, server, logger),
	)
	ts := httptest.NewServer(server.Handler())
	t.Cleanup(ts.Close)
	return &flowEnv{baseWS: "ws" + ts.URL[len("http"):], baseHTTP: "http" + ts.URL[len("http"):]}
}

func dial(t *testing.T, url, token string) *websocket.Conn {
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

func writeJSON(t *testing.T, conn *websocket.Conn, frame any) {
	t.Helper()
	if err := conn.WriteJSON(frame); err != nil {
		t.Fatal(err)
	}
}

// readUntil 读取消息直到类型/负载匹配谓词。
func readUntil(t *testing.T, conn *websocket.Conn, pred func(env map[string]any) bool) map[string]any {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			t.Fatalf("读取失败: %v", err)
		}
		var env map[string]any
		if err := json.Unmarshal(data, &env); err != nil {
			t.Fatal(err)
		}
		if pred(env) {
			return env
		}
	}
}

func readReply(t *testing.T, conn *websocket.Conn, replyTo string) map[string]any {
	t.Helper()
	return readUntil(t, conn, func(env map[string]any) bool {
		if env["type"] == "response" {
			return env["reply_to"] == replyTo
		}
		if payload, ok := env["payload"].(map[string]any); ok {
			return payload["reply_to_message_id"] == replyTo
		}
		return false
	})
}

func readPush(t *testing.T, conn *websocket.Conn, typ string) map[string]any {
	t.Helper()
	return readUntil(t, conn, func(env map[string]any) bool {
		return env["type"] == typ
	})
}

func TestSubjectIsolation(t *testing.T) {
	env := newFlowEnv(t)

	// alice 固件先连接；Desktop 绑定后固件收到 device.bound 推送。
	aliceFW := dial(t, env.baseWS+"/v1/devices/connect", "alice-dev")

	// alice 的 Desktop 绑定 alice 设备并同步目录。
	aliceDT := dial(t, env.baseWS+"/v1/clients/connect", "alice-dt")
	bindID := "bind-1"
	writeJSON(t, aliceDT, map[string]any{
		"protocol_version": "1.0", "message_id": bindID, "sequence": 1,
		"type": "device.bind", "sent_at": time.Now().UTC().Format(time.RFC3339),
		"payload": map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"},
	})
	bindResp := readReply(t, aliceDT, bindID)
	payload := bindResp["payload"].(map[string]any)
	if payload["ok"] != true {
		t.Fatalf("alice 绑定失败: %v", payload)
	}
	bound := readPush(t, aliceFW, "device.bound")
	if bound["payload"].(map[string]any)["desktop_client_id"] != "desktop-alice" {
		t.Fatalf("绑定推送 Desktop 错误: %v", bound["payload"])
	}
	syncID := "sync-1"
	writeJSON(t, aliceDT, map[string]any{
		"protocol_version": "1.0", "message_id": syncID, "sequence": 2,
		"type": "agent_sessions.sync", "sent_at": time.Now().UTC().Format(time.RFC3339),
		"payload": map[string]any{"items": []map[string]any{
			{"agent_session_id": "alice-s1", "title": "alice 会话", "project_name": "p", "agent": "codex",
				"status": "idle", "summary": "", "thread_id": "t1", "project_id": "prj"},
		}},
	})
	syncResp := readReply(t, aliceDT, syncID)
	if syncResp["payload"].(map[string]any)["ok"] != true {
		t.Fatalf("alice 同步失败: %v", syncResp["payload"])
	}
	_ = readPush(t, aliceFW, "agent_sessions.updated")
	reqID := "req-1"
	writeJSON(t, aliceFW, map[string]any{
		"id": reqID, "type": "agent_sessions.list", "payload": map[string]any{},
	})
	resp := readReply(t, aliceFW, reqID)
	page := resp["payload"].(map[string]any)
	if page["ok"] != true {
		t.Fatalf("alice 目录读取失败: %v", page)
	}
	items := page["data"].(map[string]any)["items"].([]any)
	if len(items) != 1 {
		t.Fatalf("alice 目录应含 1 条: %d", len(items))
	}

	// bob 的固件尝试读取 alice 目录 → 未绑定拒绝。
	bobFW := dial(t, env.baseWS+"/v1/devices/connect", "bob-dev")
	reqID = "req-2"
	writeJSON(t, bobFW, map[string]any{
		"id": reqID, "type": "agent_sessions.list", "payload": map[string]any{},
	})
	resp = readReply(t, bobFW, reqID)
	page = resp["payload"].(map[string]any)
	if page["ok"] == true {
		t.Fatal("bob 固件不应读取 alice 目录")
	}
	if page["error"].(map[string]any)["code"] != "PERMISSION_DENIED" {
		t.Fatalf("错误码错误: %v", page["error"])
	}

	// bob 的 Desktop 不能绑定 alice 的设备。
	bobDT := dial(t, env.baseWS+"/v1/clients/connect", "bob-dt")
	writeJSON(t, bobDT, map[string]any{
		"protocol_version": "1.0", "message_id": "bind-2", "sequence": 1,
		"type": "device.bind", "sent_at": time.Now().UTC().Format(time.RFC3339),
		"payload": map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"},
	})
	bindResp = readReply(t, bobDT, "bind-2")
	payload = bindResp["payload"].(map[string]any)
	if payload["ok"] == true {
		t.Fatal("bob 不应能绑定 alice 设备")
	}
	if payload["error"].(map[string]any)["code"] != "PERMISSION_DENIED" {
		t.Fatalf("错误码错误: %v", payload["error"])
	}

	// 认证失败：未知 token 在握手后被以 4401 关闭。
	header := http.Header{}
	header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.nope")
	conn, _, err := websocket.DefaultDialer.Dial(env.baseWS+"/v1/devices/connect", header)
	if err == nil {
		defer conn.Close()
		if _, _, rerr := conn.ReadMessage(); rerr == nil {
			t.Fatal("未知 token 应被关闭")
		}
	}

	// HTTP 健康检查。
	respHTTP, err := http.Get(env.baseHTTP + "/healthz")
	if err != nil {
		t.Fatal(err)
	}
	respHTTP.Body.Close()
	if respHTTP.StatusCode != http.StatusOK {
		t.Fatalf("healthz 状态码错误: %d", respHTTP.StatusCode)
	}
}
