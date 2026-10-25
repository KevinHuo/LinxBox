package main

import (
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/http/httputil"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// faultGateway 在真实 Gateway 前注入错误应答，验证 smoke 会拒绝错误数据，
// 而不是只检查传输成功。正常响应继续来自真实服务，避免维护另一套 Gateway 实现。
func faultGateway(t *testing.T, addr string, alter func(string, map[string]any)) string {
	t.Helper()
	proxy := httputil.NewSingleHostReverseProxy(&url.URL{Scheme: "http", Host: addr})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !websocket.IsWebSocketUpgrade(r) {
			proxy.ServeHTTP(w, r)
			return
		}
		header := http.Header{"Sec-Websocket-Protocol": r.Header.Values("Sec-WebSocket-Protocol")}
		upstream, _, err := websocket.DefaultDialer.Dial("ws://"+addr+r.URL.Path, header)
		if err != nil {
			http.Error(w, "模拟代理连接失败", http.StatusBadGateway)
			return
		}
		defer upstream.Close()
		upgrader := websocket.Upgrader{Subprotocols: []string{"linxbox.v1"}}
		client, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		var operations sync.Map
		done := make(chan struct{})
		go func() {
			defer close(done)
			defer upstream.Close()
			for {
				var frame map[string]any
				if err := client.ReadJSON(&frame); err != nil {
					return
				}
				id, _ := frame["id"].(string)
				if id == "" {
					id, _ = frame["message_id"].(string)
				}
				operations.Store(id, frame["type"])
				if err := upstream.WriteJSON(frame); err != nil {
					return
				}
			}
		}()
		defer func() { client.Close(); <-done }()
		for {
			var frame map[string]any
			if err := upstream.ReadJSON(&frame); err != nil {
				if closeError, ok := err.(*websocket.CloseError); ok {
					_ = client.WriteControl(websocket.CloseMessage, websocket.FormatCloseMessage(closeError.Code, ""), time.Now().Add(time.Second))
				}
				return
			}
			id, _ := frame["reply_to"].(string)
			if payload, ok := frame["payload"].(map[string]any); ok && id == "" {
				id, _ = payload["reply_to_message_id"].(string)
			}
			operation, _ := operations.Load(id)
			name, _ := operation.(string)
			if name == "" {
				name, _ = frame["type"].(string)
			}
			alter(name, frame)
			if err := client.WriteJSON(frame); err != nil {
				return
			}
		}
	}))
	t.Cleanup(server.Close)
	return "ws" + strings.TrimPrefix(server.URL, "http")
}

func TestSmokerRejectsGatewayFailures(t *testing.T) {
	cfg := smokeConfig(t)
	startSmokeServer(t, cfg)
	for _, tc := range []struct {
		name, operation, want string
		alter                 func(map[string]any)
	}{
		{"hello-error", "device.hello", "device.hello 失败", rejectSmokeResponse},
		{"hello-transport", "device.hello", "device.hello:", func(p map[string]any) { p["data"] = 123 }},
		{"binding-error", "device.bind", "device.bind 失败", rejectSmokeResponse},
		{"binding-transport", "device.bind", "device.bind:", func(p map[string]any) { p["data"] = 123 }},
		{"binding-mismatch", "device.bind", "Desktop 不匹配", func(p map[string]any) { p["data"].(map[string]any)["desktop_client_id"] = "other" }},
		{"sync-error", "agent_sessions.sync", "agent_sessions.sync 失败", rejectSmokeResponse},
		{"sync-transport", "agent_sessions.sync", "agent_sessions.sync:", func(p map[string]any) { p["data"] = 123 }},
		{"first-page-error", "agent_sessions.list", "第一页:", rejectSmokeResponse},
		{"first-page", "agent_sessions.list", "第一页应为 5 项", func(p map[string]any) { p["data"].(map[string]any)["items"] = []any{} }},
		{"last-page", "agent_sessions.list", "第二页应为 1 项", func(p map[string]any) {
			d := p["data"].(map[string]any)
			if len(d["items"].([]any)) == 1 {
				d["next_cursor"] = "never-ending"
			}
		}},
		{"last-page-error", "agent_sessions.list", "第二页:", func(p map[string]any) {
			if len(p["data"].(map[string]any)["items"].([]any)) == 1 {
				rejectSmokeResponse(p)
			}
		}},
		{"session-error", "agent_session.get", "agent_session.get 失败", rejectSmokeResponse},
		{"session-mismatch", "agent_session.get", "单条会话与模拟目录不一致", func(p map[string]any) { p["data"].(map[string]any)["title"] = "错误标题" }},
		{"missing-session-success", "agent_session.get", "未知会话应返回错误", func(p map[string]any) {
			if p["ok"] == false {
				p["ok"] = true
				delete(p, "error")
			}
		}},
		{"missing-session-code", "agent_session.get", "错误码应为 NOT_FOUND", func(p map[string]any) {
			if p["ok"] == false {
				p["error"].(map[string]any)["code"] = "INTERNAL"
			}
		}},
		{"notification-mismatch", "message.available", "新消息通知与模拟会话不一致", func(p map[string]any) { p["agent_session_id"] = "other-session" }},
		{"history-error", "messages.list", "历史查询转发失败", rejectSmokeResponse},
		{"history-mismatch", "messages.list", "历史应答与模拟消息不一致", func(p map[string]any) { p["data"].(map[string]any)["items"] = []any{} }},
		{"content-error", "message.content.get", "正文查询转发失败", rejectSmokeResponse},
		{"content-offset", "message.content.get", "UTF-8 字节偏移错误", func(p map[string]any) { p["data"].(map[string]any)["next_offset"] = 2 }},
		{"last-chunk-error", "message.content.get", "正文末块查询失败", func(p map[string]any) {
			if p["data"].(map[string]any)["next_offset"] == nil {
				rejectSmokeResponse(p)
			}
		}},
		{"last-chunk-offset", "message.content.get", "正文末块必须结束分页", func(p map[string]any) {
			if p["data"].(map[string]any)["next_offset"] == nil {
				p["data"].(map[string]any)["next_offset"] = 16
			}
		}},
		{"forged-mac-success", "device.hello", "伪造 MAC 应返回 PERMISSION_DENIED", func(p map[string]any) {
			if p["ok"] == false {
				p["ok"] = true
				delete(p, "error")
			}
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			base := faultGateway(t, cfg.Listen.Addr, func(operation string, frame map[string]any) {
				kind, _ := frame["type"].(string)
				if operation == tc.operation && (kind == "response" || kind == "binding.result" || kind == "agent_sessions.sync.result" || kind == "message.available") {
					tc.alter(frame["payload"].(map[string]any))
				}
			})
			s := &smoker{cfg: cfg, base: base, logger: slog.New(slog.DiscardHandler)}
			if err := s.run(); err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("故障未被预期检查捕获，期望 %q，得到 %v", tc.want, err)
			}
		})
	}
}

func rejectSmokeResponse(payload map[string]any) {
	payload["ok"] = false
	payload["error"] = map[string]any{"code": "INTERNAL", "message": "模拟服务错误"}
	delete(payload, "data")
}

func TestMockQueryRejectsMismatchedForward(t *testing.T) {
	cfg := smokeConfig(t)
	startSmokeServer(t, cfg)
	base := faultGateway(t, cfg.Listen.Addr, func(operation string, frame map[string]any) {
		if operation == "messages.list" {
			frame["payload"].(map[string]any)["agent_session_id"] = "other-session"
		}
	})
	s := &smoker{cfg: cfg, base: base, logger: slog.New(slog.DiscardHandler)}
	if err := s.run(); err == nil || !strings.Contains(err.Error(), "转发参数不正确") {
		t.Fatalf("转发参数篡改未被捕获: %v", err)
	}
}

func TestSmokerRejectsUnavailableEndpoints(t *testing.T) {
	cfg := smokeConfig(t)
	startSmokeServer(t, cfg)
	for _, endpoint := range []string{"/readyz", "/v1/devices/connect", "/v1/clients/connect"} {
		t.Run(endpoint, func(t *testing.T) {
			proxy := httputil.NewSingleHostReverseProxy(&url.URL{Scheme: "http", Host: cfg.Listen.Addr})
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == endpoint {
					w.WriteHeader(http.StatusServiceUnavailable)
					return
				}
				proxy.ServeHTTP(w, r)
			}))
			defer server.Close()
			s := &smoker{cfg: cfg, base: "ws" + strings.TrimPrefix(server.URL, "http"), logger: slog.New(slog.DiscardHandler)}
			if err := s.run(); err == nil {
				t.Fatalf("不可用入口 %s 被误报成功", endpoint)
			}
		})
	}
}
