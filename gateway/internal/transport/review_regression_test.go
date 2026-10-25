package transport

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

func TestReviewBindingOwnerAfterCredentialChange(t *testing.T) {
	for _, change := range []string{"device", "desktop", "both"} {
		t.Run(change, func(t *testing.T) {
			old := newTestEnv(t, nil)
			fw := dialWS(t, old.baseWS+"/v1/devices/connect", "dev-token")
			if reply := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
				t.Fatal("设备未就绪")
			}
			dt := dialWS(t, old.baseWS+"/v1/clients/connect", "dt-token")
			if reply := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
				t.Fatal("原账号绑定失败")
			}
			item := sessionPayload(1)
			item["title"], item["project_name"], item["summary"] = "原账号私有标题", "原账号私有项目", "原账号私有摘要"
			if reply := dt.dtReq(t, "agent_sessions.sync", map[string]any{"items": []any{item}}); !reply.OK {
				t.Fatal("原账号目录同步失败")
			}
			if reply := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "private-request", "agent_session_id": "sess-1"}); !reply.OK {
				t.Fatal("原账号请求准备失败")
			}
			fw.conn.Close()
			dt.conn.Close()
			old.server.conns.CloseAll()
			old.ts.Close()

			// 重新组装认证与服务，保留同一 SQLite 文件，复现更换账号配置后重启。
			next := newTestEnv(t, func(c *config.Config) {
				c.Database.Path = old.cfg.Database.Path
				if change != "desktop" {
					c.Credentials.Devices[0].Subject = "bob"
				}
				if change != "device" {
					c.Credentials.Desktops[0].Subject = "bob"
				}
				c.Pairing.Rules = append(c.Pairing.Rules, config.PairingRule{Subject: "bob", DeviceIDs: []string{"AA:BB:CC:DD:EE:01"}})
			})
			device := dialWS(t, next.baseWS+"/v1/devices/connect", "dev-token")
			for _, request := range []struct {
				op      string
				payload map[string]any
			}{
				{"device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}},
				{"agent_sessions.list", map[string]any{}},
				{"agent_session.get", map[string]any{"agent_session_id": "sess-1"}},
				{"messages.list", map[string]any{"agent_session_id": "sess-1"}},
				{"message.content.get", map[string]any{"agent_session_id": "sess-1", "message_id": "private", "offset": 0}},
				{"request.get", map[string]any{"request_id": "private-request"}},
				{"request.prepare", map[string]any{"request_id": "private-request", "agent_session_id": "sess-1"}},
			} {
				reply := device.fwReq(t, request.op, request.payload)
				wantCode := "PERMISSION_DENIED"
				if request.op != "device.hello" {
					wantCode = "CONFLICT" // hello 被拒后连接仍未登记，所有业务请求继续拒绝。
				}
				if reply.OK || reply.Code != wantCode {
					t.Errorf("%s 更换账号后未拒绝旧绑定: %+v", request.op, reply)
				}
				if strings.Contains(string(reply.RawData), "原账号私有") {
					t.Errorf("%s 泄露原账号数据", request.op)
				}
			}
			desktop := dialWS(t, next.baseWS+"/v1/clients/connect", "dt-token")
			if reply := desktop.dtReq(t, "device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); reply.OK || reply.Code != "PERMISSION_DENIED" {
				t.Errorf("旧账号绑定不能被新账号幂等接管: %+v", reply)
			}
		})
	}
}

func TestReviewFirmwareEncodedFrameLimit(t *testing.T) {
	for _, tc := range []struct {
		name, operation string
		limit, chars    int
	}{
		{"64KiB", "messages.list", 64 * 1024, 20000},
		{"configured-limit", "message.content.get", 1024, 400},
	} {
		t.Run(tc.name, func(t *testing.T) {
			env := newTestEnv(t, func(c *config.Config) { c.Limits.MaxFrameBytes = tc.limit })
			fw := dialRawWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
			dt := dialRawWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
			pipeWrite(t, fw, firmwareWire("device.hello", "hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}))
			pipeRead(t, fw, "response")
			pipeWrite(t, dt, desktopWire("device.bind", "bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}))
			pipeRead(t, dt, "binding.result")
			pipeRead(t, fw, "device.bound")
			pipeWrite(t, dt, desktopWire("agent_sessions.sync", "sync", map[string]any{"items": []any{sessionPayload(1)}}))
			pipeRead(t, dt, "agent_sessions.sync.result")
			pipeRead(t, fw, "agent_sessions.updated")
			id := strings.Repeat("<", 128)
			input := map[string]any{"agent_session_id": "sess-1"}
			if tc.operation == "message.content.get" {
				input["message_id"], input["offset"] = "answer", 0
			}
			pipeWrite(t, fw, firmwareWire(tc.operation, id, input))
			query := pipeRead(t, dt, tc.operation)
			var queryID string
			if err := json.Unmarshal(query["message_id"], &queryID); err != nil {
				t.Fatal(err)
			}
			var wire bytes.Buffer
			encoder := json.NewEncoder(&wire)
			encoder.SetEscapeHTML(false)
			if err := encoder.Encode(desktopWire("query.result", "result", map[string]any{
				"reply_to_message_id": queryID, "ok": false,
				"error": map[string]any{"code": "INTERNAL", "message": strings.Repeat("<", tc.chars)},
			})); err != nil {
				t.Fatal(err)
			}
			if wire.Len() > tc.limit {
				t.Fatalf("回归输入必须在 Desktop 帧限制内: %d", wire.Len())
			}
			if err := dt.WriteMessage(websocket.TextMessage, wire.Bytes()); err != nil {
				t.Fatal(err)
			}
			fw.SetReadDeadline(time.Now().Add(2 * time.Second))
			_, response, err := fw.ReadMessage()
			if err != nil {
				t.Fatal(err)
			}
			if len(response) > tc.limit {
				t.Fatalf("固件响应编码后超限: %d > %d", len(response), tc.limit)
			}
			if err := env.server.validator.ValidateFirmwareResponse(tc.operation, response); err != nil {
				t.Fatal(err)
			}
			var result struct {
				ReplyTo string `json:"reply_to"`
				Payload struct {
					OK    bool `json:"ok"`
					Error struct {
						Code string `json:"code"`
					} `json:"error"`
				} `json:"payload"`
			}
			if err := json.Unmarshal(response, &result); err != nil {
				t.Fatal(err)
			}
			if result.ReplyTo != id || result.Payload.OK || result.Payload.Error.Code != "RESOURCE_EXHAUSTED" {
				t.Fatalf("应返回与原请求关联的有界错误: %s", response)
			}
			pipeWrite(t, fw, firmwareWire("agent_session.get", "still-connected", map[string]any{"agent_session_id": "sess-1"}))
			requirePipeResponse(t, env.server, fw, "agent_session.get", "still-connected", "")
		})
	}
}

func TestReviewFirstBindingRequiresOnlineDevice(t *testing.T) {
	env := newTestEnv(t, nil)
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	if reply := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); reply.OK || reply.Code != "DEPENDENCY_UNAVAILABLE" {
		t.Fatalf("设备未上线时首次绑定应失败: %+v", reply)
	}
	database, err := storage.Open(env.cfg.Database.Path)
	if err != nil {
		t.Fatal(err)
	}
	defer database.Close()
	if _, err := storage.NewRepositories(database.DB()).Bindings.GetByDevice(t.Context(), database.DB(), "AA:BB:CC:DD:EE:01"); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("离线绑定失败后不应留下数据库记录: %v", err)
	}
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if reply := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK || strings.Contains(string(reply.RawData), "desktop-1") {
		t.Fatalf("未上线首次绑定失败后必须保持未绑定: %+v", reply)
	}
	if reply := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
		t.Fatalf("设备上线后应可首次绑定: %+v", reply)
	}
	fw.conn.Close()
	deadline := time.Now().Add(time.Second)
	for env.server.conns.Device("AA:BB:CC:DD:EE:01") != nil {
		if time.Now().After(deadline) {
			t.Fatal("设备断线后未释放连接")
		}
		time.Sleep(time.Millisecond)
	}
	if reply := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !reply.OK {
		t.Fatalf("同一 Desktop 离线重复绑定应保留幂等结果: %+v", reply)
	}
}
