package transport

import (
	"encoding/json"
	"net"
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
)

func TestDynamicRegistrationWithoutMACConfiguration(t *testing.T) {
	env := newTestEnv(t, func(c *config.Config) {
		c.Credentials.Devices = []config.DeviceCredential{{Token: "dev-token", Subject: "alice"}, {Token: "bob-device", Subject: "bob"}}
		c.Pairing.Rules = nil
	})
	const mac = "34:00:00:00:00:99"
	desktop := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	if r := desktop.dtReq(t, "device.bind", map[string]any{"mac_address": mac}); r.OK || r.Code != "DEPENDENCY_UNAVAILABLE" {
		t.Fatal("Desktop 可先连接，但 hello 前设备不可绑定", r)
	}
	firmware := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if r := firmware.fwReq(t, "agent_sessions.list", map[string]any{}); r.OK || r.Code != "CONFLICT" {
		t.Fatal("hello 前不能读取业务数据", r)
	}
	if env.server.conns.DeviceOnline(mac) {
		t.Fatal("未 hello 的连接不应计作已登记设备")
	}
	response := firmware.fwReq(t, "device.hello", map[string]any{"mac_address": "340000000099"})
	if !response.OK {
		t.Fatal("无需预配置 MAC 即可登记", response)
	}
	var binding struct {
		Device  string  `json:"device_id"`
		Desktop *string `json:"desktop_client_id"`
	}
	if err := json.Unmarshal(response.RawData, &binding); err != nil || binding.Device != mac || binding.Desktop != nil {
		t.Fatal("hello 应返回规范 MAC 和未绑定快照", binding, err)
	}
	if r := desktop.dtReq(t, "device.bind", map[string]any{"mac_address": mac}); !r.OK {
		t.Fatal("同账号在线设备无需白名单即可绑定", r)
	}
	if r := firmware.fwReq(t, "device.hello", map[string]any{"mac_address": mac}); !r.OK {
		t.Fatal("重复 hello 应返回绑定快照", r)
	}
	if r := firmware.fwReq(t, "device.hello", map[string]any{"mac_address": "34:00:00:00:00:98"}); r.OK || r.Code != "PERMISSION_DENIED" {
		t.Fatal("已经登记的连接不可中途更换 MAC", r)
	}
	second := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if r := second.fwReq(t, "device.hello", map[string]any{"mac_address": "34:00:00:00:00:98"}); !r.OK {
		t.Fatal("同账号凭据可登记不同真实设备", r)
	}
	bob := dialWS(t, env.baseWS+"/v1/devices/connect", "bob-device")
	if r := bob.fwReq(t, "device.hello", map[string]any{"mac_address": mac}); r.OK || r.Code != "PERMISSION_DENIED" {
		t.Fatal("另一账号不能抢占已登记 MAC", r)
	}
	if r := firmware.fwReq(t, "agent_sessions.list", map[string]any{}); !r.OK {
		t.Fatal("抢占失败不应关闭原设备连接", r)
	}
	const bobMAC = "34:00:00:00:00:97"
	if r := bob.fwReq(t, "device.hello", map[string]any{"mac_address": bobMAC}); !r.OK {
		t.Fatal("被拒后仍可在期限内登记自己的设备", r)
	}
	if r := desktop.dtReq(t, "device.bind", map[string]any{"mac_address": bobMAC}); r.OK || r.Code != "PERMISSION_DENIED" {
		t.Fatal("Desktop 不可绑定另一账号设备", r)
	}
}

func TestPendingHelloTimeout(t *testing.T) {
	env := newTestEnv(t, nil)
	env.server.helloTimeout = 30 * time.Millisecond
	conn := dialRawWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	conn.SetReadDeadline(time.Now().Add(time.Second))
	if _, _, err := conn.ReadMessage(); err == nil {
		t.Fatal("未 hello 的连接必须到期关闭")
	} else if timeout, ok := err.(net.Error); ok && timeout.Timeout() {
		t.Fatal("应由服务端到期关闭，而非客户端读超时")
	}
}
