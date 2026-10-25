package lifecycle

import (
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
)

func testLogger() *slog.Logger {
	l, _ := NewLogger("warn", io.Discard)
	return l
}

func testLimits() config.LimitsConfig {
	return config.LimitsConfig{
		MaxOnlineConnections: 8,
		SendQueueSize:        4,
		SendTimeout:          config.Duration(2 * time.Second),
	}
}

// pair 建立一条测试连接：serverConn 由服务端 Connection 管理，client 是客户端。
type pair struct {
	server *Connection
	client *websocket.Conn
	url    string
}

func newPair(t *testing.T, conns *Connections) *pair {
	t.Helper()
	got := make(chan *Connection, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		raw, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		conn := NewConnection(raw, conns.SendQueueSize(), 2*time.Second, 30*time.Second, 60*time.Second, conns.NextGeneration())
		got <- conn
	}))
	t.Cleanup(srv.Close)
	url := "ws" + strings.TrimPrefix(srv.URL, "http")
	client, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		t.Fatalf("客户端拨号失败: %v", err)
	}
	t.Cleanup(func() { _ = client.Close() })
	server := <-got
	return &pair{server: server, client: client, url: url}
}

func TestConnectionSendAndClose(t *testing.T) {
	conns := NewConnections(testLogger(), testLimits())
	p := newPair(t, conns)
	if err := p.server.Send([]byte(`{"a":1}`), time.Second); err != nil {
		t.Fatal(err)
	}
	_, data, err := p.client.ReadMessage()
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != `{"a":1}` {
		t.Fatalf("消息不匹配: %s", data)
	}
	// Close 后发送失败。
	p.server.Close()
	if err := p.server.Send([]byte(`x`), time.Millisecond); err == nil {
		t.Fatal("关闭后发送应失败")
	}
	if err := p.server.Send([]byte(`x`), time.Millisecond); err == nil {
		t.Fatal("已关闭连接发送应继续失败")
	}
	// Close 幂等。
	p.server.Close()
	// 代次递增。
	if p.server.Generation() == 0 {
		t.Fatal("代次应为正数")
	}
	// NextSequence 递增。
	s1 := p.server.NextSequence()
	s2 := p.server.NextSequence()
	if s2 <= s1 || s1 < 1 {
		t.Fatalf("序号应递增: %d %d", s1, s2)
	}
}

func TestConnectionSendQueueFull(t *testing.T) {
	// 手工构造无写入循环的连接：队列只进不出，验证有界背压。
	conn := &Connection{send: make(chan []byte, 4), done: make(chan struct{})}
	for i := 0; i < 4; i++ {
		if err := conn.Send([]byte("x"), 100*time.Millisecond); err != nil {
			t.Fatalf("队列未满时发送失败: %v", err)
		}
	}
	// 第 5 条触发超时并关闭连接。
	if err := conn.Send([]byte("y"), 300*time.Millisecond); err == nil {
		t.Fatal("队列满应报错并关闭")
	}
	if !conn.closed.Load() {
		t.Fatal("队列满应关闭连接")
	}
	// 关闭后发送立即失败。
	if err := conn.Send([]byte("z"), time.Millisecond); err == nil {
		t.Fatal("关闭后发送应立即失败")
	}
}

func TestConnectionsRegistry(t *testing.T) {
	logger := testLogger()
	conns := NewConnections(logger, testLimits())
	p1 := newPair(t, conns)
	p2 := newPair(t, conns)

	if !conns.RegisterDevice("AA:BB:CC:DD:EE:01", p1.server) {
		t.Fatal("注册应成功")
	}
	if got := conns.Device("AA:BB:CC:DD:EE:01"); got != p1.server {
		t.Fatal("读取应命中新连接")
	}
	// 替换：旧连接关闭。
	if !conns.RegisterDevice("AA:BB:CC:DD:EE:01", p2.server) {
		t.Fatal("替换注册应成功")
	}
	if got := conns.Device("AA:BB:CC:DD:EE:01"); got != p2.server {
		t.Fatal("替换后应命中新连接")
	}
	// 旧连接已关闭。
	if err := p1.server.Send([]byte("x"), 100*time.Millisecond); err == nil {
		t.Fatal("被替换的连接应已关闭")
	}
	// 注销：旧连接调用注销不影响新连接。
	conns.UnregisterDevice("AA:BB:CC:DD:EE:01", p1.server)
	if got := conns.Device("AA:BB:CC:DD:EE:01"); got != p2.server {
		t.Fatal("旧连接注销不应移除新连接")
	}
	conns.UnregisterDevice("AA:BB:CC:DD:EE:01", p2.server)
	if conns.Device("AA:BB:CC:DD:EE:01") != nil {
		t.Fatal("注销后应为空")
	}
	// Desktop 注册与替换。
	p3 := newPair(t, conns)
	if !conns.RegisterDesktop("d1", p3.server) {
		t.Fatal("Desktop 注册应成功")
	}
	if conns.Desktop("d1") != p3.server {
		t.Fatal("Desktop 读取失败")
	}
	conns.UnregisterDesktop("d1", p3.server)
	if conns.Desktop("d1") != nil {
		t.Fatal("Desktop 注销失败")
	}
}

func TestConnectionsLimit(t *testing.T) {
	limits := testLimits()
	limits.MaxOnlineConnections = 2
	conns := NewConnections(testLogger(), limits)
	p1 := newPair(t, conns)
	p2 := newPair(t, conns)
	p3 := newPair(t, conns)
	if !conns.RegisterDevice("AA:BB:CC:DD:EE:01", p1.server) {
		t.Fatal("注册 1 应成功")
	}
	if !conns.RegisterDevice("AA:BB:CC:DD:EE:02", p2.server) {
		t.Fatal("注册 2 应成功")
	}
	if conns.RegisterDevice("AA:BB:CC:DD:EE:03", p3.server) {
		t.Fatal("达上限后注册应拒绝")
	}
}

func TestCloseAll(t *testing.T) {
	conns := NewConnections(testLogger(), testLimits())
	p1 := newPair(t, conns)
	p2 := newPair(t, conns)
	_ = conns.RegisterDevice("AA:BB:CC:DD:EE:01", p1.server)
	_ = conns.RegisterDesktop("d1", p2.server)
	conns.CloseAll()
	if err := p1.server.Send([]byte("x"), 100*time.Millisecond); err == nil {
		t.Fatal("CloseAll 后发送应失败")
	}
	if err := p2.server.Send([]byte("x"), 100*time.Millisecond); err == nil {
		t.Fatal("CloseAll 后 Desktop 发送应失败")
	}
}

func TestConnectionsConcurrent(t *testing.T) {
	conns := NewConnections(testLogger(), testLimits())
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			key := "AA:BB:CC:DD:EE:0" + string(rune('0'+i%10))
			_ = conns.NextGeneration()
			// 空连接对象注册/注销并发安全。
			conn := &Connection{done: make(chan struct{}), send: make(chan []byte, 1), generation: uint64(i)}
			_ = conns.RegisterDevice(key, conn)
			conns.UnregisterDevice(key, conn)
			_ = conns.Device(key)
		}(i)
	}
	wg.Wait()
	conns.CloseAll()
}

func TestWriteLoopWriteErrorCloses(t *testing.T) {
	conns := NewConnections(testLogger(), testLimits())
	p := newPair(t, conns)
	// 客户端先关闭，服务端写触发错误并关闭连接。
	_ = p.client.Close()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if err := p.server.Send([]byte("x"), 100*time.Millisecond); err != nil {
			return // 写错误后连接已关闭
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatal("写错误后应关闭连接")
}

func TestPingTicker(t *testing.T) {
	conns := NewConnections(testLogger(), testLimits())
	got := make(chan *Connection, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		raw, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		// 短 ping 周期加速心跳路径。
		conn := NewConnection(raw, conns.SendQueueSize(), 1*time.Second, 100*time.Millisecond, 1*time.Second, conns.NextGeneration())
		got <- conn
	}))
	t.Cleanup(srv.Close)
	url := "ws" + strings.TrimPrefix(srv.URL, "http")
	client, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = client.Close() })
	server := <-got
	defer server.Close()
	// gorilla 默认对控制帧自动处理，需自定义 PingHandler 并在读循环中处理帧。
	pinged := make(chan struct{})
	client.SetPingHandler(func(string) error {
		select {
		case <-pinged:
		default:
			close(pinged)
		}
		return nil
	})
	go func() {
		for {
			if _, _, err := client.ReadMessage(); err != nil {
				return
			}
		}
	}()
	select {
	case <-pinged:
	case <-time.After(3 * time.Second):
		t.Fatal("未在预期时间内收到心跳")
	}
}

func TestRegisterDesktopLimit(t *testing.T) {
	limits := testLimits()
	limits.MaxOnlineConnections = 2
	conns := NewConnections(testLogger(), limits)
	p1 := newPair(t, conns)
	p2 := newPair(t, conns)
	p3 := newPair(t, conns)
	if !conns.RegisterDesktop("d1", p1.server) {
		t.Fatal("注册 1 应成功")
	}
	if !conns.RegisterDesktop("d2", p2.server) {
		t.Fatal("注册 2 应成功")
	}
	if conns.RegisterDesktop("d3", p3.server) {
		t.Fatal("达上限后注册应拒绝")
	}
}

func TestConnectionSendTimeoutConfig(t *testing.T) {
	conns := NewConnections(testLogger(), testLimits())
	if conns.SendTimeout() != 2*time.Second {
		t.Fatalf("SendTimeout 配置错误: %v", conns.SendTimeout())
	}
	if conns.SendQueueSize() != 4 {
		t.Fatalf("SendQueueSize 配置错误: %d", conns.SendQueueSize())
	}
}

func TestRegisterDesktopReplaces(t *testing.T) {
	conns := NewConnections(testLogger(), testLimits())
	p1 := newPair(t, conns)
	p2 := newPair(t, conns)
	if !conns.RegisterDesktop("d1", p1.server) {
		t.Fatal("首次注册失败")
	}
	if !conns.RegisterDesktop("d1", p2.server) {
		t.Fatal("替换注册失败")
	}
	// 旧连接应已关闭：发送立即报错。
	if err := p1.server.Send([]byte("x"), 100*time.Millisecond); err == nil {
		t.Fatal("旧连接应已关闭")
	}
}
