package lifecycle

import "testing"

func TestPendingCapacityAndIdentification(t *testing.T) {
	limits := testLimits()
	limits.MaxOnlineConnections = 1
	connections := NewConnections(testLogger(), limits)
	first := guardedConnection(1)
	if !connections.RegisterPending(first) || connections.DeviceOnline("mac") {
		t.Fatal("待 hello 连接不能成为在线设备")
	}
	if connections.RegisterPending(guardedConnection(2)) {
		t.Fatal("待 hello 连接数必须有界")
	}
	if connections.IdentifyDevice("mac", guardedConnection(9)) {
		t.Fatal("不能登记不在等待集合中的连接")
	}
	if !connections.IdentifyDevice("mac", first) || !connections.DeviceOnline("mac") {
		t.Fatal("hello 后应在线")
	}
	second := guardedConnection(2)
	if !connections.RegisterPending(second) {
		t.Fatal("满员时仍应允许有界待登记连接，以便原设备重连")
	}
	if connections.IdentifyDevice("another", second) {
		t.Fatal("新设备不得超过在线连接限制")
	}
	if !connections.IdentifyDevice("mac", second) || !first.closed.Load() {
		t.Fatal("同 MAC 重连应在满员时替换旧连接")
	}
	third := guardedConnection(3)
	if !connections.RegisterPending(third) {
		t.Fatal("等待注册失败")
	}
	third.Close()
	if connections.IdentifyDevice("mac", third) {
		t.Fatal("已关闭连接不可登记")
	}
	connections.UnregisterPending(third)
	fourth := guardedConnection(4)
	if !connections.RegisterPending(fourth) {
		t.Fatal("释放后应可接入")
	}
	connections.CloseAll()
	if !fourth.closed.Load() || connections.RegisterPending(guardedConnection(5)) || connections.IdentifyDevice("mac", fourth) {
		t.Fatal("停止应关闭待登记连接并拒绝新登记")
	}
}
