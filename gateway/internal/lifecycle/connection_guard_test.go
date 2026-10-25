package lifecycle

import (
	"testing"
	"time"
)

func guardedConnection(gen uint64) *Connection {
	return &Connection{send: make(chan []byte, 1), done: make(chan struct{}), generation: gen}
}

func TestDeviceOnlineRejectsClosedAndStoppingConnections(t *testing.T) {
	c := NewConnections(testLogger(), testLimits())
	if c.DeviceOnline("device") {
		t.Fatal("未连接设备不能在线")
	}
	conn := guardedConnection(1)
	if !c.RegisterDevice("device", conn) || !c.DeviceOnline("device") {
		t.Fatal("有效设备连接应在线")
	}
	conn.Close()
	if c.DeviceOnline("device") {
		t.Fatal("已关闭但未注销的连接不能在线")
	}
	if !c.RegisterDevice("device", guardedConnection(2)) {
		t.Fatal("设备应可重新连接")
	}
	c.CloseAll()
	if c.DeviceOnline("device") {
		t.Fatal("服务停止后设备不能在线")
	}
}

func TestConnectionMutationGuard(t *testing.T) {
	limits := testLimits()
	limits.MaxOnlineConnections = 1
	c := NewConnections(testLogger(), limits)
	old := guardedConnection(1)
	next := guardedConnection(2)
	if !c.RegisterDesktop("d", old) {
		t.Fatal("注册失败")
	}
	entered := make(chan struct{})
	release := make(chan struct{})
	finished := make(chan error, 1)
	go func() { finished <- c.WithDesktop("d", old, func() error { close(entered); <-release; return nil }) }()
	<-entered
	replaced := make(chan bool, 1)
	go func() { replaced <- c.RegisterDesktop("d", next) }()
	select {
	case <-replaced:
		t.Fatal("重连必须等待在途事务结束")
	case <-time.After(10 * time.Millisecond):
	}
	close(release)
	if err := <-finished; err != nil {
		t.Fatal(err)
	}
	if !<-replaced {
		t.Fatal("达到容量时同一身份应可重连")
	}
	if err := c.WithDesktop("d", old, func() error { t.Fatal("旧代次不能执行"); return nil }); err == nil {
		t.Fatal("旧连接应失效")
	}
	if c.RegisterDevice("other", guardedConnection(3)) {
		t.Fatal("不同身份仍受容量上限限制")
	}
	c.UnregisterDesktop("d", next)
	device := guardedConnection(4)
	if !c.RegisterDevice("device", device) {
		t.Fatal("设备注册失败")
	}
	if err := c.WithDevice("device", device, func() error { return nil }); err != nil {
		t.Fatal(err)
	}
	if err := c.WithDevice("device", nil, func() error { return nil }); err == nil {
		t.Fatal("空连接应拒绝")
	}
	c.CloseAll()
	if c.RegisterDesktop("after-stop", guardedConnection(5)) {
		t.Fatal("停止后不能注册新连接")
	}
	if err := c.WithDevice("device", device, func() error { return nil }); err == nil {
		t.Fatal("关闭后不能执行")
	}
}
