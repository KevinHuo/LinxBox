package transport

import (
	"fmt"
	"strings"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestMCPSubjectAcrossDevicesAndTransports(t *testing.T) {
	for _, transport := range []string{"streamable_http", "sse"} {
		for _, implicit := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/implicit=%t", transport, implicit), func(t *testing.T) {
				env := newTestEnv(t, func(cfg *config.Config) {
					cfg.MCP.AllowImplicitRequestID = implicit
					cfg.Limits.MaxFrameBytes = 65536
					cfg.Credentials.MCP = []config.MCPCredential{{Token: "mcp-token", Subject: "alice", AgentID: "agent"}}
				})
				dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
				var devices []*wsClient
				for i := 1; i <= 2; i++ {
					mac := fmt.Sprintf("AA:BB:CC:DD:EE:%02d", i)
					fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
					if r := fw.fwReq(t, "device.hello", map[string]any{"mac_address": mac}); !r.OK {
						t.Fatal(r)
					}
					if r := dt.dtReq(t, "device.bind", map[string]any{"mac_address": mac}); !r.OK {
						t.Fatal(r)
					}
					devices = append(devices, fw)
				}
				if r := dt.dtReq(t, "agent_sessions.sync", map[string]any{"items": []any{sessionPayload(1), sessionPayload(2)}}); !r.OK {
					t.Fatal(r)
				}
				for i, fw := range devices {
					if r := fw.fwReq(t, "request.prepare", map[string]any{"request_id": fmt.Sprintf("req-%d", i+1), "agent_session_id": fmt.Sprintf("sess-%d", i+1)}); !r.OK {
						t.Fatal(r)
					}
				}
				var client *mcp.ClientSession
				var headers *mcpHeaders
				if transport == "sse" {
					client, headers = newMCPClient(t, env)
				} else {
					client, headers = newStreamableClient(t, env)
				}
				for i, fw := range devices {
					id := fmt.Sprintf("req-%d", i+1)
					headers.set("X-Linx-Device-Id", fmt.Sprintf("AA:BB:CC:DD:EE:%02d", i+1))
					headers.set("X-Linx-Session-Id", fmt.Sprintf("voice-%d", i+1))
					headers.set("X-Linx-Request-Id", id)
					if implicit {
						headers.set("X-Linx-Request-Id", "")
					}
					headers.set("X-Linx-Agent-Id", "wrong-agent")
					callMCP(t, env, client, map[string]any{"text": "原文"}, "PERMISSION_DENIED")
					headers.set("X-Linx-Agent-Id", "agent")
					r := callMCP(t, env, client, map[string]any{"text": "原文"}, "")
					if r.RequestID != id || r.Status != "awaiting_confirmation" {
						t.Fatal(r)
					}
					fw.expectPush(t, "request.proposed")
				}
				headers.set("X-Linx-Device-Id", "AA:BB:CC:DD:EE:09")
				callMCP(t, env, client, map[string]any{"text": "原文"}, "PERMISSION_DENIED")
			})
		}
	}
}

func TestMCPSubjectRejectsOtherAccountAcrossTransports(t *testing.T) {
	for _, transport := range []string{"streamable_http", "sse"} {
		t.Run(transport, func(t *testing.T) {
			env := newTestEnv(t, func(cfg *config.Config) {
				cfg.Limits.MaxFrameBytes = 65536
				cfg.Credentials.MCP = []config.MCPCredential{{Token: "mcp-token", Subject: "bob", AgentID: "agent"}}
			})
			fw := prepareStreamableRequest(t, env)
			var client *mcp.ClientSession
			var headers *mcpHeaders
			if transport == "sse" {
				client, headers = newMCPClient(t, env)
			} else {
				client, headers = newStreamableClient(t, env)
			}
			for key, value := range map[string]string{"X-Linx-Device-Id": "AA:BB:CC:DD:EE:01", "X-Linx-Agent-Id": "agent", "X-Linx-Session-Id": "voice", "X-Linx-Request-Id": "req-1"} {
				headers.set(key, value)
			}
			callMCP(t, env, client, map[string]any{"text": "不能写入其他账号"}, "PERMISSION_DENIED")
			r := fw.fwReq(t, "request.get", map[string]any{"request_id": "req-1"})
			if !r.OK || !strings.Contains(string(r.RawData), `"text":null`) {
				t.Fatal("跨账号调用修改了原请求", r)
			}
		})
	}
}
