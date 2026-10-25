package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
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
	"github.com/KevinHuo/LinxBox/gateway/internal/transport"
)

// smokeConfig 构造与 smoke 流程一致的测试配置。
func smokeConfig(t *testing.T) *config.Config {
	t.Helper()
	return &config.Config{
		Database: config.DatabaseConfig{Path: filepath.Join(t.TempDir(), "gw.db")},
		Credentials: config.CredentialsConfig{
			Devices: []config.DeviceCredential{
				{Token: "dev-token", Subject: "alice"},
			},
			Desktops: []config.DesktopCredential{
				{Token: "dt-token", ClientID: "desktop-1", Subject: "alice"},
			},
		},
		Limits: config.LimitsConfig{
			MaxFrameBytes: 64 * 1024, SessionPageSize: 5, MaxCatalogItems: 200,
			MaxOnlineConnections: 16, SendQueueSize: 8, SendTimeout: config.Duration(2 * time.Second),
		},
		Timeouts: config.TimeoutConfig{ShutdownGrace: config.Duration(2 * time.Second)},
	}
}

// startSmokeServer 使用临时数据库、专用凭据和回环 TCP 端口。
func startSmokeServer(t *testing.T, cfg *config.Config) func() {
	t.Helper()
	// 选择可用端口并真实启动服务（readyz 需要 ready 状态）。
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	_ = ln.Close()
	cfg.Listen.Addr = fmt.Sprintf("127.0.0.1:%d", port)

	logger := slog.New(slog.DiscardHandler)
	validator, err := contracts.NewValidator()
	if err != nil {
		t.Fatal(err)
	}
	db, err := storage.Open(cfg.Database.Path)
	if err != nil {
		t.Fatal(err)
	}
	repos := storage.NewRepositories(db.DB())
	conns := lifecycle.NewConnections(logger, cfg.Limits)
	server := transport.NewServer(cfg, logger, validator, transport.NewAuthenticator(cfg), conns)
	server.SetServices(
		service.NewBindingService(repos, cfg, server, logger),
		service.NewSessionService(repos, cfg, server, logger),
	)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- server.Run(ctx) }()
	var once sync.Once
	stop := func() {
		once.Do(func() {
			cancel()
			select {
			case err := <-done:
				if err != nil {
					t.Errorf("Gateway 退出失败: %v", err)
				}
			case <-time.After(5 * time.Second):
				t.Error("Gateway 未按时退出")
			}
			if err := db.Close(); err != nil {
				t.Errorf("关闭数据库失败: %v", err)
			}
		})
	}
	t.Cleanup(stop)

	// 等待 readyz 可用。
	httpBase := fmt.Sprintf("http://127.0.0.1:%d", port)
	deadline := time.Now().Add(5 * time.Second)
	for {
		resp, err := http.Get(httpBase + "/readyz")
		if err == nil {
			resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				break
			}
		}
		if time.Now().After(deadline) {
			t.Fatal("网关未就绪")
		}
		time.Sleep(50 * time.Millisecond)
	}

	return stop
}

// TestSmokerAgainstLiveServer 使用模拟两端验证真实 TCP 入口；重复执行不清理数据库，
// Gateway 重启后先读取快照，再同步目录，避免把重写数据当作持久化证据。
func TestSmokerAgainstLiveServer(t *testing.T) {
	const mac = "34:00:00:00:00:77"
	cfg := smokeConfig(t)
	stop := startSmokeServer(t, cfg)
	configPath := filepath.Join(t.TempDir(), "smoke.json")
	raw, err := json.Marshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(configPath, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	t.Run("first", func(t *testing.T) {
		if code := run([]string{"--config", configPath, "--device-mac", mac}); code != 0 {
			t.Fatalf("首次模拟联调退出码: %d", code)
		}
	})
	t.Run("repeat", func(t *testing.T) {
		if code := run([]string{"--config", configPath, "--url", "ws://" + cfg.Listen.Addr, "--device-mac", mac}); code != 0 {
			t.Fatalf("同一数据库重复联调退出码: %d", code)
		}
	})
	stop()
	startSmokeServer(t, cfg)
	t.Run("restart", func(t *testing.T) {
		fw := newWS("ws://" + cfg.Listen.Addr + "/v1/devices/connect")
		if err := fw.dial(cfg.Credentials.Devices[0].Token); err != nil {
			t.Fatal(err)
		}
		defer fw.close()
		hello, err := fw.request("device.hello", map[string]any{"mac_address": mac})
		if err != nil {
			t.Fatal(err)
		}
		var binding contracts.Binding
		if err := json.Unmarshal(hello.RawData, &binding); err != nil || !hello.OK ||
			binding.DesktopClientID == nil || *binding.DesktopClientID != cfg.Credentials.Desktops[0].ClientID {
			t.Fatalf("Gateway 重启后绑定未保留: %+v: %v", hello, err)
		}
		page, err := fw.request("agent_sessions.list", map[string]any{})
		if err != nil {
			t.Fatal(err)
		}
		if !page.OK || len(page.Page.Items) != 5 || page.Page.NextCursor == nil || page.Page.Items[0].AgentSessionID != "sess-01" {
			t.Fatalf("Gateway 重启后目录未保留: %+v", page)
		}
		if code := run([]string{"--config", configPath, "--url", "ws://" + cfg.Listen.Addr, "--device-mac", mac}); code != 0 {
			t.Fatalf("重启后模拟联调退出码: %d", code)
		}
	})
}

func TestSmokerRunBadConfig(t *testing.T) {
	// 配置缺设备凭据时，run 返回错误。
	cfg := smokeConfig(t)
	cfg.Credentials.Devices = nil
	s := &smoker{cfg: cfg, base: "ws://127.0.0.1:1", logger: slog.New(slog.DiscardHandler)}
	if err := s.run(); err == nil {
		t.Fatal("缺凭据应报错")
	}
}

func TestCheckHTTPUnreachable(t *testing.T) {
	cfg := smokeConfig(t)
	s := &smoker{cfg: cfg, base: "ws://127.0.0.1:1", logger: slog.New(slog.DiscardHandler)}
	if err := s.checkHTTP("healthz", "/healthz", 200); err == nil {
		t.Fatal("不可达地址应报错")
	}
}

func TestParseFirmwareResponseMalformed(t *testing.T) {
	if _, err := parseFirmwareResponse([]byte(`{`)); err == nil {
		t.Fatal("非法 JSON 应报错")
	}
	if _, err := parseFirmwareResponse([]byte(`{"payload":123}`)); err == nil {
		t.Fatal("payload 类型错误应报错")
	}
}

func TestWaitReplyTimeout(t *testing.T) {
	ch := make(chan json.RawMessage, 1)
	if _, err := waitReply(ch, 50*time.Millisecond); err == nil {
		t.Fatal("超时应报错")
	}
	ch <- json.RawMessage(`{"a":1}`)
	got, err := waitReply(ch, time.Second)
	if err != nil || string(got) != `{"a":1}` {
		t.Fatalf("正常读取失败: %v %s", err, got)
	}
}

func TestParseFirmwareResponseDataNotPage(t *testing.T) {
	// data 存在但不是 SessionPage 结构。
	if _, err := parseFirmwareResponse([]byte(`{"payload":{"ok":true,"data":{"items":"x"}}}`)); err == nil {
		t.Fatal("data 结构错误应报错")
	}
}

func TestRunMissingFlag(t *testing.T) {
	if code := run([]string{}); code != 2 {
		t.Fatalf("缺少配置参数应返回 2: %d", code)
	}
	if code := run([]string{"--nope"}); code != 2 {
		t.Fatalf("非法参数应返回 2: %d", code)
	}
}

func TestRunBadConfigFile(t *testing.T) {
	if code := run([]string{"--config", filepath.Join(t.TempDir(), "nope.json")}); code != 1 {
		t.Fatalf("配置加载失败应返回 1: %d", code)
	}
}

func TestRunNoCredentials(t *testing.T) {
	cfg := smokeConfig(t)
	cfg.Credentials.Devices = nil
	path := filepath.Join(t.TempDir(), "smoke.json")
	raw, err := json.Marshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	if code := run([]string{"--config", path}); code != 1 {
		t.Fatalf("缺凭据应返回 1: %d", code)
	}
}

func TestCheckHTTPStatusMismatch(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer srv.Close()
	cfg := smokeConfig(t)
	s := &smoker{cfg: cfg, base: "ws" + strings.TrimPrefix(srv.URL, "http"), logger: slog.New(slog.DiscardHandler)}
	if err := s.checkHTTP("healthz", "/healthz", 200); err == nil {
		t.Fatal("状态码不匹配应报错")
	}
}

func TestDialRejectsPlainHTTP(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	defer srv.Close()
	ws := newWS("ws" + strings.TrimPrefix(srv.URL, "http") + "/v1/devices/connect")
	if err := ws.dial("token"); err == nil {
		t.Fatal("普通 HTTP 端点不应握手成功")
	}
}

func TestSmokerRunHealthzFailure(t *testing.T) {
	// healthz 非 200：链路应在最早检查失败。
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer srv.Close()
	cfg := smokeConfig(t)
	s := &smoker{cfg: cfg, base: "ws" + strings.TrimPrefix(srv.URL, "http"), logger: slog.New(slog.DiscardHandler)}
	if err := s.run(); err == nil {
		t.Fatal("healthz 非 200 应报错")
	}
}

func TestRequestWriteError(t *testing.T) {
	// 拨号成功后关闭连接：WriteJSON 应返回错误而非 panic。
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		conn, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		go func() {
			for {
				if _, _, err := conn.ReadMessage(); err != nil {
					return
				}
			}
		}()
	}))
	defer srv.Close()
	c := newWS("ws" + strings.TrimPrefix(srv.URL, "http") + "/v1/devices/connect")
	if err := c.dial("token"); err != nil {
		t.Fatal(err)
	}
	c.close()
	if _, err := c.request("device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); err == nil {
		t.Fatal("已关闭连接写入应失败")
	}
}

func TestRequestTimeout(t *testing.T) {
	// 服务端升级但不应答：request 等待应答超时。
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		conn, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		// 不返回任何 response；持续读取直到连接关闭。
		go func() {
			for {
				if _, _, err := conn.ReadMessage(); err != nil {
					return
				}
			}
		}()
	}))
	defer srv.Close()
	c := newWS("ws" + strings.TrimPrefix(srv.URL, "http") + "/v1/devices/connect")
	if err := c.dial("token"); err != nil {
		t.Fatal(err)
	}
	defer c.close()
	if _, err := c.request("device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); err == nil {
		t.Fatal("无应答应超时")
	}
	if _, err := c.reply("device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); err == nil {
		t.Fatal("无应答应超时")
	}
	if _, err := c.expectType("device.bound", 50*time.Millisecond); err == nil {
		t.Fatal("无推送应超时")
	}
}

func TestReplyWriteError(t *testing.T) {
	// 拨号成功后关闭连接：WriteJSON 应返回错误而非 panic。
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		conn, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		go func() {
			for {
				if _, _, err := conn.ReadMessage(); err != nil {
					return
				}
			}
		}()
	}))
	defer srv.Close()
	c := newWS("ws" + strings.TrimPrefix(srv.URL, "http") + "/v1/devices/connect")
	if err := c.dial("token"); err != nil {
		t.Fatal(err)
	}
	c.close()
	if _, err := c.reply("device.bind", map[string]any{}); err == nil {
		t.Fatal("已关闭连接写入应失败")
	}
}
