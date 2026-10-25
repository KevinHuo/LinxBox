// Command linxbox-smoke 通过真实网络入口运行模拟链路：
// 模拟固件与 Desktop 完成认证、绑定、目录、历史、正文与通知读取。
// 仅针对专用联调实例运行，会覆盖所选 Desktop 的目录。
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"reflect"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
)

func main() {
	os.Exit(run(os.Args[1:]))
}

// run 解析参数并执行模拟链路；使用独立 FlagSet 便于测试。
func run(args []string) int {
	fs := flag.NewFlagSet("linxbox-smoke", flag.ContinueOnError)
	configPath := fs.String("config", "", "网关配置文件路径（读取凭据与监听地址）")
	baseURL := fs.String("url", "", "网关基础 URL（默认 ws://<listen.addr>）")
	deviceMAC := fs.String("device-mac", "AA:BB:CC:DD:EE:01", "模拟固件主动上报的 MAC，与服务端配置无关")
	if err := fs.Parse(args); err != nil {
		fs.Usage()
		return 2
	}
	if *configPath == "" {
		fs.Usage()
		return 2
	}
	cfg, err := config.Load(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "配置加载失败: %v\n", err)
		return 1
	}
	base := *baseURL
	if base == "" {
		base = "ws://" + cfg.Listen.Addr
	}
	logger, _ := lifecycle.NewLogger("warn", os.Stderr)
	s := &smoker{cfg: cfg, base: base, logger: logger, mac: *deviceMAC}

	if err := s.run(); err != nil {
		fmt.Fprintf(os.Stderr, "SMOKE FAIL: %v\n", err)
		return 1
	}
	fmt.Printf("SMOKE PASS: %d 项检查通过\n", s.checks)
	return 0
}

type smoker struct {
	mac    string
	cfg    *config.Config
	base   string
	logger *slog.Logger
	checks int
}

func (s *smoker) ok(name string) {
	s.checks++
	fmt.Printf("  [ok] %s\n", name)
}

func (s *smoker) run() error {
	if len(s.cfg.Credentials.Devices) == 0 || len(s.cfg.Credentials.Desktops) == 0 {
		return fmt.Errorf("配置缺少设备或 Desktop 凭据")
	}
	if err := s.checkHTTP("healthz", "/healthz", 200); err != nil {
		return err
	}
	if err := s.checkHTTP("readyz", "/readyz", 200); err != nil {
		return err
	}

	deviceCred := s.cfg.Credentials.Devices[0]
	desktopCred := s.cfg.Credentials.Desktops[0]
	mac := s.mac
	if mac == "" {
		mac = "AA:BB:CC:DD:EE:01"
	}

	// 固件连接：hello 返回当前绑定。
	fw := newWS(s.base + "/v1/devices/connect")
	if err := fw.dial(deviceCred.Token); err != nil {
		return fmt.Errorf("固件连接失败: %w", err)
	}
	defer fw.close()
	hello, err := fw.request("device.hello", map[string]any{"mac_address": mac})
	if err != nil {
		return fmt.Errorf("device.hello: %w", err)
	}
	if !hello.OK {
		return fmt.Errorf("device.hello 失败: %s %s", hello.Code, hello.Message)
	}
	s.ok("固件 device.hello")

	// Desktop 连接并绑定设备。
	dt := newWS(s.base + "/v1/clients/connect")
	if err := dt.dial(desktopCred.Token); err != nil {
		return fmt.Errorf("Desktop 连接失败: %w", err)
	}
	defer dt.close()
	if err := dt.send("client.hello", map[string]any{
		"desktop_version":     "smoke-0.1.0",
		"protocol_versions":   []string{"1.0"},
		"last_acked_sequence": 0,
		"capabilities":        []string{"smoke"},
	}); err != nil {
		return fmt.Errorf("client.hello: %w", err)
	}
	bindMsg, err := dt.reply("device.bind", map[string]any{"mac_address": mac})
	if err != nil {
		return fmt.Errorf("device.bind: %w", err)
	}
	if !bindMsg.OK {
		return fmt.Errorf("device.bind 失败: %s %s", bindMsg.Code, bindMsg.Message)
	}
	if bindMsg.Bound == nil || bindMsg.Bound.DesktopClientID != desktopCred.ClientID {
		return fmt.Errorf("device.bind 返回 Desktop 不匹配")
	}
	s.ok("Desktop device.bind 成功")

	// 新绑定验证推送；已有同一绑定时验证快照，支持持久数据库重复冒烟。
	var initial contracts.Binding
	if err := json.Unmarshal(hello.RawData, &initial); err != nil {
		return err
	}
	if initial.DesktopClientID == nil {
		bound, err := fw.expectType("device.bound", 3*time.Second)
		if err != nil {
			return err
		}
		var notice struct {
			Payload contracts.BoundBinding `json:"payload"`
		}
		if err := json.Unmarshal(bound, &notice); err != nil {
			return err
		}
		if notice.Payload.DesktopClientID != desktopCred.ClientID {
			return fmt.Errorf("device.bound Desktop 不匹配")
		}
	} else {
		if *initial.DesktopClientID != desktopCred.ClientID {
			return fmt.Errorf("设备已绑定其他 Desktop")
		}
	}
	s.ok("绑定快照与新绑定通知")

	// Desktop 全量同步目录（6 条，验证分页）。
	items := make([]any, 0, 6)
	for i := 1; i <= 6; i++ {
		items = append(items, map[string]any{
			"agent_session_id": fmt.Sprintf("sess-%02d", i),
			"title":            fmt.Sprintf("会话 %02d", i),
			"project_name":     "smoke-project",
			"agent":            "codex",
			"status":           "idle",
			"summary":          "",
			"thread_id":        fmt.Sprintf("thread-%02d", i),
			"project_id":       "prj-smoke",
		})
	}
	syncMsg, err := dt.reply("agent_sessions.sync", map[string]any{"items": items})
	if err != nil {
		return fmt.Errorf("agent_sessions.sync: %w", err)
	}
	if !syncMsg.OK {
		return fmt.Errorf("agent_sessions.sync 失败: %s %s", syncMsg.Code, syncMsg.Message)
	}
	if _, err := fw.expectType("agent_sessions.updated", 3*time.Second); err != nil {
		return err
	}
	s.ok("Desktop 目录同步")

	// 固件分页读取目录。
	page1, err := fw.request("agent_sessions.list", map[string]any{})
	if err != nil || !page1.OK {
		return fmt.Errorf("agent_sessions.list 第一页: %v", err)
	}
	if len(page1.Page.Items) != 5 || page1.Page.NextCursor == nil {
		return fmt.Errorf("第一页应为 5 项且有下一页")
	}
	s.ok("固件目录第一页（5 项）")

	page2, err := fw.request("agent_sessions.list", map[string]any{"cursor": *page1.Page.NextCursor})
	if err != nil || !page2.OK {
		return fmt.Errorf("agent_sessions.list 第二页: %v", err)
	}
	if len(page2.Page.Items) != 1 || page2.Page.NextCursor != nil {
		return fmt.Errorf("第二页应为 1 项且无下一页")
	}
	s.ok("固件目录第二页（1 项，结束）")

	// 固件读取单条会话。
	get, err := fw.request("agent_session.get", map[string]any{"agent_session_id": "sess-03"})
	if err != nil || !get.OK {
		return fmt.Errorf("agent_session.get 失败: %v", err)
	}
	var session contracts.AgentSession
	if err := json.Unmarshal(get.RawData, &session); err != nil || session.AgentSessionID != "sess-03" || session.Title != "会话 03" {
		return fmt.Errorf("单条会话与模拟目录不一致")
	}
	s.ok("固件 agent_session.get")

	// 未知会话返回 NOT_FOUND。
	missing, err := fw.request("agent_session.get", map[string]any{"agent_session_id": "sess-999"})
	if err != nil || missing.OK {
		return fmt.Errorf("未知会话应返回错误")
	}
	if missing.Code != "NOT_FOUND" {
		return fmt.Errorf("未知会话错误码应为 NOT_FOUND，得到 %s", missing.Code)
	}
	s.ok("未知会话 NOT_FOUND")

	// 本机模拟 Desktop 的只读查询应答；不启动 Codex、不调用模型。
	if err := dt.send("message.available", map[string]any{"agent_session_id": "sess-03", "message_id": "smoke-answer"}); err != nil {
		return err
	}
	notice, err := fw.expectType("message.available", 3*time.Second)
	if err != nil {
		return err
	}
	var available struct {
		Payload struct {
			SessionID string `json:"agent_session_id"`
			MessageID string `json:"message_id"`
		} `json:"payload"`
	}
	if err := json.Unmarshal(notice, &available); err != nil || available.Payload.SessionID != "sess-03" || available.Payload.MessageID != "smoke-answer" {
		return fmt.Errorf("新消息通知与模拟会话不一致")
	}
	s.ok("最终消息通知转发")
	history, err := fw.mockQuery(dt, "messages.list", map[string]any{"agent_session_id": "sess-03"},
		map[string]any{"items": []any{map[string]any{"message_id": "smoke-answer", "role": "assistant", "summary": "模拟最终回复"}}, "next_cursor": nil})
	if err != nil || !history.OK {
		return fmt.Errorf("历史查询转发失败: %v", err)
	}
	var messages struct {
		Items []struct {
			MessageID string `json:"message_id"`
			Role      string `json:"role"`
			Summary   string `json:"summary"`
		} `json:"items"`
		Next *string `json:"next_cursor"`
	}
	if err := json.Unmarshal(history.RawData, &messages); err != nil || len(messages.Items) != 1 || messages.Next != nil ||
		messages.Items[0].MessageID != "smoke-answer" || messages.Items[0].Role != "assistant" || messages.Items[0].Summary != "模拟最终回复" {
		return fmt.Errorf("历史应答与模拟消息不一致")
	}
	s.ok("历史查询与 query.result 关联")
	content, err := fw.mockQuery(dt, "message.content.get", map[string]any{"agent_session_id": "sess-03", "message_id": "smoke-answer", "offset": 0},
		map[string]any{"text": "你好🙂", "next_offset": 10})
	if err != nil || !content.OK {
		return fmt.Errorf("正文查询转发失败: %v", err)
	}
	var chunk struct {
		Text string `json:"text"`
		Next *int   `json:"next_offset"`
	}
	if err := json.Unmarshal(content.RawData, &chunk); err != nil || chunk.Text != "你好🙂" || chunk.Next == nil || *chunk.Next != 10 {
		return fmt.Errorf("中文正文或 UTF-8 字节偏移错误")
	}
	last, err := fw.mockQuery(dt, "message.content.get", map[string]any{"agent_session_id": "sess-03", "message_id": "smoke-answer", "offset": *chunk.Next},
		map[string]any{"text": "结束", "next_offset": nil})
	if err != nil || !last.OK {
		return fmt.Errorf("正文末块查询失败: %v", err)
	}
	if err := json.Unmarshal(last.RawData, &chunk); err != nil || chunk.Text != "结束" || chunk.Next != nil {
		return fmt.Errorf("正文末块必须结束分页")
	}
	s.ok("正文分块与 UTF-8 字节偏移")

	// 认证失败：错误 token 被拒绝（握手失败或升级后被 4401 关闭）。
	bad := newWS(s.base + "/v1/devices/connect")
	if err := bad.dialRaw("wrong-token"); err == nil {
		// 服务器先升级再校验：应立刻收到关闭帧。
		_ = bad.conn.SetReadDeadline(time.Now().Add(3 * time.Second))
		_, _, rerr := bad.conn.ReadMessage()
		bad.close()
		if !websocket.IsCloseError(rerr, 4401) {
			return fmt.Errorf("错误凭据应以 4401 拒绝，得到 %v", rerr)
		}
	}
	s.ok("错误凭据被拒绝")

	// 固件伪造 MAC 被拒绝。
	forged, err := fw.request("device.hello", map[string]any{"mac_address": "AA:00:00:00:00:FF"})
	if err != nil || forged.OK || forged.Code != "PERMISSION_DENIED" {
		return fmt.Errorf("伪造 MAC 应返回 PERMISSION_DENIED，得到 %+v: %v", forged, err)
	}
	s.ok("伪造 MAC 被拒绝")
	return nil
}

func (s *smoker) checkHTTP(name, path string, want int) error {
	httpBase := strings.Replace(s.base, "ws://", "http://", 1)
	httpBase = strings.Replace(httpBase, "wss://", "https://", 1)
	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Get(httpBase + path) //nolint:gosec // smoke 工具访问本机测试地址
	if err != nil {
		return fmt.Errorf("%s: %w", name, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != want {
		return fmt.Errorf("%s 状态码 %d，期望 %d", name, resp.StatusCode, want)
	}
	s.ok("HTTP " + name)
	return nil
}

// fwResp 是固件 response 的解析结果。
type fwResp struct {
	RawData json.RawMessage
	OK      bool
	Code    string
	Message string
	Page    contracts.SessionPage
}

// dtReply 是 Desktop 应答的解析结果。
type dtReply struct {
	OK      bool
	Code    string
	Message string
	Bound   *contracts.BoundBinding
}

// wsClient 是测试用 WebSocket 客户端：response 按 id/reply 分发，推送入全局队列。
type wsClient struct {
	pendingMu sync.Mutex
	writeMu   sync.Mutex
	url       string
	conn      *websocket.Conn
	pending   map[string]chan json.RawMessage
	pushes    chan json.RawMessage
}

func newWS(wsURL string) *wsClient {
	return &wsClient{
		url:     wsURL,
		pending: map[string]chan json.RawMessage{},
		pushes:  make(chan json.RawMessage, 64),
	}
}

func (c *wsClient) dial(token string) error {
	header := http.Header{}
	header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token."+token)
	conn, _, err := websocket.DefaultDialer.Dial(c.url, header)
	if err != nil {
		return err
	}
	c.conn = conn
	go c.readLoop()
	return nil
}

// dialRaw 仅握手、不启动后台读协程；供需要自行 ReadMessage 的场景使用。
func (c *wsClient) dialRaw(token string) error {
	header := http.Header{}
	header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token."+token)
	conn, _, err := websocket.DefaultDialer.Dial(c.url, header)
	if err != nil {
		return err
	}
	c.conn = conn
	return nil
}

func (c *wsClient) close() {
	if c.conn != nil {
		_ = c.conn.Close()
	}
}

// readLoop 分发 response（reply_to 或 payload.reply_to_message_id）与推送消息。
func (c *wsClient) readLoop() {
	for {
		_, data, err := c.conn.ReadMessage()
		if err != nil {
			return
		}
		var env struct {
			Type    string `json:"type"`
			ReplyTo string `json:"reply_to"`
			Payload struct {
				ReplyToMessageID string `json:"reply_to_message_id"`
			} `json:"payload"`
		}
		if err := json.Unmarshal(data, &env); err != nil {
			continue
		}
		if env.Type == "response" || env.Payload.ReplyToMessageID != "" {
			key := env.ReplyTo
			if key == "" {
				key = env.Payload.ReplyToMessageID
			}
			c.pendingMu.Lock()
			ch := c.pending[key]
			c.pendingMu.Unlock()
			if ch != nil {
				select {
				case ch <- data:
				default:
				}
			}
			continue
		}
		select {
		case c.pushes <- data:
		default:
		}
	}
}

// request 发送固件请求并等待 response。
func (c *wsClient) request(typ string, payload map[string]any) (*fwResp, error) {
	id := contracts.NewMessageID("smoke_")
	ch := make(chan json.RawMessage, 1)
	c.pendingMu.Lock()
	c.pending[id] = ch
	c.pendingMu.Unlock()
	defer func() { c.pendingMu.Lock(); delete(c.pending, id); c.pendingMu.Unlock() }()
	frame := map[string]any{"id": id, "type": typ, "payload": payload}
	if err := c.writeFrame(frame); err != nil {
		return nil, err
	}
	raw, err := waitReply(ch, 5*time.Second)
	if err != nil {
		return nil, fmt.Errorf("请求 %s: %w", typ, err)
	}
	return parseFirmwareResponse(raw)
}

// reply 发送 Desktop 消息并等待带 reply_to_message_id 的应答。
func (c *wsClient) reply(typ string, payload map[string]any) (*dtReply, error) {
	mid := contracts.NewMessageID("msg_")
	ch := make(chan json.RawMessage, 1)
	c.pendingMu.Lock()
	c.pending[mid] = ch
	c.pendingMu.Unlock()
	defer func() { c.pendingMu.Lock(); delete(c.pending, mid); c.pendingMu.Unlock() }()
	frame := map[string]any{
		"protocol_version": "1.0",
		"message_id":       mid,
		"sequence":         1,
		"type":             typ,
		"sent_at":          time.Now().UTC().Format(time.RFC3339),
		"payload":          payload,
	}
	if err := c.writeFrame(frame); err != nil {
		return nil, err
	}
	raw, err := waitReply(ch, 5*time.Second)
	if err != nil {
		return nil, fmt.Errorf("消息 %s: %w", typ, err)
	}
	var env struct {
		Payload struct {
			OK    bool `json:"ok"`
			Error *struct {
				Code    string `json:"code"`
				Message string `json:"message"`
			} `json:"error"`
			Data *contracts.BoundBinding `json:"data"`
		} `json:"payload"`
	}
	if err := json.Unmarshal(raw, &env); err != nil {
		return nil, err
	}
	out := &dtReply{OK: env.Payload.OK, Bound: env.Payload.Data}
	if env.Payload.Error != nil {
		out.Code = env.Payload.Error.Code
		out.Message = env.Payload.Error.Message
	}
	return out, nil
}

// send 发送无应答期望的 Desktop 消息。
func (c *wsClient) send(typ string, payload map[string]any) error {
	frame := map[string]any{
		"protocol_version": "1.0",
		"message_id":       contracts.NewMessageID("msg_"),
		"sequence":         1,
		"type":             typ,
		"sent_at":          time.Now().UTC().Format(time.RFC3339),
		"payload":          payload,
	}
	return c.writeFrame(frame)
}

// expectType 等待固件推送消息。
func (c *wsClient) expectType(typ string, timeout time.Duration) (json.RawMessage, error) {
	deadline := time.After(timeout)
	for {
		select {
		case raw := <-c.pushes:
			var env struct {
				Type string `json:"type"`
			}
			_ = json.Unmarshal(raw, &env)
			if env.Type == typ {
				return raw, nil
			}
		case <-deadline:
			return nil, fmt.Errorf("未收到 %s", typ)
		}
	}
}

func waitReply(ch chan json.RawMessage, timeout time.Duration) (json.RawMessage, error) {
	select {
	case raw := <-ch:
		return raw, nil
	case <-time.After(timeout):
		return nil, fmt.Errorf("应答超时")
	}
}

func parseFirmwareResponse(raw json.RawMessage) (*fwResp, error) {
	var env struct {
		Payload struct {
			OK    bool `json:"ok"`
			Error *struct {
				Code    string `json:"code"`
				Message string `json:"message"`
			} `json:"error"`
			Data json.RawMessage `json:"data"`
		} `json:"payload"`
	}
	if err := json.Unmarshal(raw, &env); err != nil {
		return nil, err
	}
	out := &fwResp{OK: env.Payload.OK, RawData: env.Payload.Data}
	if env.Payload.Error != nil {
		out.Code = env.Payload.Error.Code
		out.Message = env.Payload.Error.Message
	}
	if len(env.Payload.Data) > 0 {
		if err := json.Unmarshal(env.Payload.Data, &out.Page); err != nil {
			return nil, err
		}
	}
	return out, nil
}

func (c *wsClient) writeFrame(frame any) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	return c.conn.WriteJSON(frame)
}

// mockQuery 在真实 Gateway 网络入口验证两端关联；模拟结果明确不作为真实 Agent 证据。
func (c *wsClient) mockQuery(desktop *wsClient, typ string, input, result any) (*fwResp, error) {
	id := contracts.NewMessageID("smoke_")
	ch := make(chan json.RawMessage, 1)
	c.pendingMu.Lock()
	c.pending[id] = ch
	c.pendingMu.Unlock()
	defer func() { c.pendingMu.Lock(); delete(c.pending, id); c.pendingMu.Unlock() }()
	if err := c.writeFrame(map[string]any{"id": id, "type": typ, "payload": input}); err != nil {
		return nil, err
	}
	raw, err := desktop.expectType(typ, 3*time.Second)
	if err != nil {
		return nil, err
	}
	var query struct {
		MessageID string `json:"message_id"`
		Payload   any    `json:"payload"`
	}
	if err := json.Unmarshal(raw, &query); err != nil {
		return nil, err
	}
	inputJSON, err := json.Marshal(input)
	if err != nil {
		return nil, err
	}
	var expected any
	if err := json.Unmarshal(inputJSON, &expected); err != nil {
		return nil, err
	}
	if query.MessageID == "" || query.MessageID == id || !reflect.DeepEqual(query.Payload, expected) {
		return nil, fmt.Errorf("Gateway 查询 ID 或转发参数不正确")
	}
	if err := desktop.send("query.result", map[string]any{"reply_to_message_id": query.MessageID, "ok": true, "data": result}); err != nil {
		return nil, err
	}
	response, err := waitReply(ch, 3*time.Second)
	if err != nil {
		return nil, err
	}
	return parseFirmwareResponse(response)
}
