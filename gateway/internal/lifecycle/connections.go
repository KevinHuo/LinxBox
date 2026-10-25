package lifecycle

import (
	"errors"
	"log/slog"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
)

// Connection 是一条已认证的 WebSocket 连接，由单一写入循环串行发送。
type Connection struct {
	Conn       *websocket.Conn
	send       chan []byte
	done       chan struct{}
	closeOnce  sync.Once
	closed     atomic.Bool
	generation uint64
	nextSeq    atomic.Int64
}

// NewConnection 构造连接并启动写入循环。
func NewConnection(conn *websocket.Conn, queueSize int, writeWait time.Duration, pingPeriod time.Duration, pongWait time.Duration, generation uint64) *Connection {
	c := &Connection{
		Conn:       conn,
		send:       make(chan []byte, queueSize),
		done:       make(chan struct{}),
		generation: generation,
	}
	conn.SetPongHandler(func(string) error {
		_ = conn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})
	go c.writeLoop(writeWait, pingPeriod)
	return c
}

func (c *Connection) writeLoop(writeWait, pingPeriod time.Duration) {
	ticker := time.NewTicker(pingPeriod)
	defer ticker.Stop()
	for {
		select {
		case msg := <-c.send:
			_ = c.Conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.Conn.WriteMessage(websocket.TextMessage, msg); err != nil {
				c.close()
				return
			}
		case <-ticker.C:
			_ = c.Conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.Conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				c.close()
				return
			}
		case <-c.done:
			return
		}
	}
}

// Send 将消息加入有界发送队列；慢消费者导致队列满时关闭连接，不无限积压。
func (c *Connection) Send(data []byte, timeout time.Duration) error {
	if c.closed.Load() {
		return errors.New("连接已关闭")
	}
	select {
	case c.send <- data:
		// 已关闭的连接不会再被写入循环消费；此时如实报错。
		if c.closed.Load() {
			return errors.New("连接已关闭")
		}
		return nil
	case <-c.done:
		return errors.New("连接已关闭")
	case <-time.After(timeout):
		c.close()
		return errors.New("发送队列已满，连接已关闭")
	}
}

// Generation 返回连接代次；旧代次回调不得覆盖新连接。
func (c *Connection) Generation() uint64 { return c.generation }

// NextSequence 返回本连接下一条出站消息序号（非负递增）。
func (c *Connection) NextSequence() int64 { return c.nextSeq.Add(1) }

// Close 关闭连接与写入循环。
func (c *Connection) Close() {
	c.close()
}

func (c *Connection) close() {
	c.closeOnce.Do(func() {
		c.closed.Store(true)
		close(c.done)
		if c.Conn != nil {
			_ = c.Conn.Close()
		}
	})
}

// Connections 管理设备与 Desktop 的在线连接表；同一标识的新连接取代旧连接。
type Connections struct {
	mu          sync.Mutex
	operations  sync.Map // 已认证主体的操作锁，与连接替换使用同一边界。
	devices     map[string]*Connection
	desktops    map[string]*Connection
	logger      *slog.Logger
	maxTotal    int
	closing     bool
	sendQueue   int
	sendTimeout time.Duration
	writeWait   time.Duration
	pingPeriod  time.Duration
	pongWait    time.Duration
	nextGen     atomic.Uint64
}

// NewConnections 构造连接表。
func NewConnections(logger *slog.Logger, limits config.LimitsConfig) *Connections {
	return &Connections{
		devices:     map[string]*Connection{},
		desktops:    map[string]*Connection{},
		logger:      logger,
		maxTotal:    limits.MaxOnlineConnections,
		sendQueue:   limits.SendQueueSize,
		sendTimeout: limits.SendTimeout.Std(),
		writeWait:   10 * time.Second,
		pingPeriod:  30 * time.Second,
		pongWait:    60 * time.Second,
	}
}

// RegisterDevice 注册或替换设备连接；连接数达上限时拒绝。
func (c *Connections) RegisterDevice(deviceID string, conn *Connection) bool {
	guard := c.operationLock("device:" + deviceID)
	guard.Lock()
	defer guard.Unlock()
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closing || (c.devices[deviceID] == nil && len(c.devices)+len(c.desktops) >= c.maxTotal) {
		return false
	}
	if old := c.devices[deviceID]; old != nil {
		old.Close()
	}
	c.devices[deviceID] = conn
	return true
}

// RegisterDesktop 注册或替换 Desktop 连接。
func (c *Connections) RegisterDesktop(clientID string, conn *Connection) bool {
	guard := c.operationLock("desktop:" + clientID)
	guard.Lock()
	defer guard.Unlock()
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closing || (c.desktops[clientID] == nil && len(c.devices)+len(c.desktops) >= c.maxTotal) {
		return false
	}
	if old := c.desktops[clientID]; old != nil {
		old.Close()
	}
	c.desktops[clientID] = conn
	return true
}

// UnregisterDevice 仅当连接仍为当前连接时移除。
func (c *Connections) UnregisterDevice(deviceID string, conn *Connection) {
	guard := c.operationLock("device:" + deviceID)
	guard.Lock()
	defer guard.Unlock()
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.devices[deviceID] == conn {
		delete(c.devices, deviceID)
	}
}

// UnregisterDesktop 仅当连接仍为当前连接时移除。
func (c *Connections) UnregisterDesktop(clientID string, conn *Connection) {
	guard := c.operationLock("desktop:" + clientID)
	guard.Lock()
	defer guard.Unlock()
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.desktops[clientID] == conn {
		delete(c.desktops, clientID)
	}
}

// Device 返回设备当前在线连接；不存在返回 nil。
func (c *Connections) Device(deviceID string) *Connection {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.devices[deviceID]
}

// DeviceOnline 排除已经关闭但尚未完成注销的设备连接。
func (c *Connections) DeviceOnline(deviceID string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	conn := c.devices[deviceID]
	return !c.closing && conn != nil && !conn.closed.Load()
}

// Desktop 返回 Desktop 当前在线连接；不存在返回 nil。
func (c *Connections) Desktop(clientID string) *Connection {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.desktops[clientID]
}

// CloseAll 关闭全部在线连接（进程收尾）。
func (c *Connections) CloseAll() {
	c.mu.Lock()
	c.closing = true
	keys := make([]string, 0, len(c.devices)+len(c.desktops))
	for id := range c.devices {
		keys = append(keys, "device:"+id)
	}
	for id := range c.desktops {
		keys = append(keys, "desktop:"+id)
	}
	c.mu.Unlock()
	for _, key := range keys {
		guard := c.operationLock(key)
		guard.Lock()
		c.mu.Lock()
		if strings.HasPrefix(key, "device:") {
			if conn := c.devices[strings.TrimPrefix(key, "device:")]; conn != nil {
				conn.Close()
			}
		} else {
			if conn := c.desktops[strings.TrimPrefix(key, "desktop:")]; conn != nil {
				conn.Close()
			}
		}
		c.mu.Unlock()
		guard.Unlock()
	}
}

// NextGeneration 分配连接代次。
func (c *Connections) NextGeneration() uint64 { return c.nextGen.Add(1) }

// SendTimeout 返回发送超时。
func (c *Connections) SendTimeout() time.Duration { return c.sendTimeout }

// SendQueueSize 返回发送队列容量。
func (c *Connections) SendQueueSize() int { return c.sendQueue }

func (c *Connections) operationLock(key string) *sync.Mutex {
	lock, _ := c.operations.LoadOrStore(key, &sync.Mutex{})
	return lock.(*sync.Mutex)
}

// WithDevice / WithDesktop 将当前代次检查和业务写入与同一身份的重连替换串行化。
// fn 内可读取连接表，但不可再次替换、注销同一身份或嵌套调用 With*。
func (c *Connections) WithDevice(id string, conn *Connection, fn func() error) error {
	return c.withCurrent("device:"+id, func() *Connection { return c.Device(id) }, conn, fn)
}

func (c *Connections) WithDesktop(id string, conn *Connection, fn func() error) error {
	return c.withCurrent("desktop:"+id, func() *Connection { return c.Desktop(id) }, conn, fn)
}

func (c *Connections) withCurrent(key string, current func() *Connection, conn *Connection, fn func() error) error {
	guard := c.operationLock(key)
	guard.Lock()
	defer guard.Unlock()
	c.mu.Lock()
	closing := c.closing
	c.mu.Unlock()
	if closing || conn == nil || conn.closed.Load() || current() != conn {
		return errors.New("连接代次已失效")
	}
	return fn()
}
