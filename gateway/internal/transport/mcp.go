package transport

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/modelcontextprotocol/go-sdk/jsonrpc"
	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
)

// mcpEndpoint 保留灵矽的旧版 HTTP/SSE 入口；JSON-RPC 与工具分发交给官方 SDK。
type mcpEndpoint struct {
	server   *Server
	sdk      *mcp.Server
	mu       sync.Mutex
	sessions map[string]*mcpSSESession
	closed   bool
}

// SetRequests 接入请求服务；无 MCP 凭据时入口仍保持拒绝访问。
func (s *Server) SetRequests(requests *service.RequestService) error {
	schema, err := contracts.MCPInputSchema()
	if err != nil {
		return err
	}
	s.requests = requests
	e := &mcpEndpoint{server: s, sessions: map[string]*mcpSSESession{}}
	e.sdk = mcp.NewServer(&mcp.Implementation{Name: "linxbox-gateway", Version: "1.0.0"}, &mcp.ServerOptions{Logger: s.logger})
	e.sdk.AddTool(&mcp.Tool{Name: "propose_task_reply", Description: "原样接收本轮最终 ASR 文字，等待设备确认；不执行任务。", InputSchema: schema}, e.propose)
	s.mcp = e
	return nil
}

func (s *Server) handleMCP(w http.ResponseWriter, r *http.Request) {
	if s.mcp == nil {
		http.Error(w, "MCP 服务未配置", http.StatusServiceUnavailable)
		return
	}
	s.mcp.ServeHTTP(w, r)
}

func (s *Server) authenticateMCP(r *http.Request) (config.MCPCredential, error) {
	// MCP 使用独立 Bearer 凭据，禁止复用设备/电脑凭据或 WS 子协议。
	values := r.Header.Values("Authorization")
	if len(values) == 1 {
		parts := strings.Fields(values[0])
		if len(parts) == 2 && strings.EqualFold(parts[0], "Bearer") {
			for _, cred := range s.cfg.Credentials.MCP {
				if parts[1] == cred.Token {
					return cred, nil
				}
			}
		}
	}
	return config.MCPCredential{}, domain.ErrUnauthenticated("MCP 凭据无效")
}

func (e *mcpEndpoint) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	cred, err := e.server.authenticateMCP(r)
	if err != nil {
		w.Header().Set("WWW-Authenticate", "Bearer")
		http.Error(w, "MCP 凭据无效", http.StatusUnauthorized)
		return
	}
	// 本入口供服务端调用，拒绝浏览器跨源连接。
	if r.Header.Get("Origin") != "" {
		http.Error(w, "不接受浏览器来源", http.StatusForbidden)
		return
	}
	if r.Method == http.MethodGet && r.URL.Path == "/mcp" {
		e.connect(w, r, cred)
		return
	}
	if r.Method != http.MethodPost || r.URL.Path != "/mcp/message" {
		w.Header().Set("Allow", "GET, POST")
		http.Error(w, "入口或方法不匹配", http.StatusMethodNotAllowed)
		return
	}
	e.mu.Lock()
	session := e.sessions[r.URL.Query().Get("sessionid")]
	e.mu.Unlock()
	if session == nil {
		http.Error(w, "MCP 会话不存在", http.StatusNotFound)
		return
	}
	if session.credential != cred {
		http.Error(w, "MCP 会话不属于此凭据", http.StatusForbidden)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, int64(e.server.cfg.Limits.MaxFrameBytes))
	raw, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "MCP 消息读取失败或超限", http.StatusRequestEntityTooLarge)
		return
	}
	if !json.Valid(raw) {
		http.Error(w, "MCP 消息无效", http.StatusBadRequest)
		return
	}
	msg, err := jsonrpc.DecodeMessage(raw)
	if err != nil {
		http.Error(w, "JSON-RPC 消息无效", http.StatusBadRequest)
		return
	}
	if req, ok := msg.(*jsonrpc.Request); ok {
		// SSE SDK 默认不透传 POST 头；把本次头随消息传入 SDK，禁止缓存 GET 的逐轮上下文。
		req.Extra = &mcp.RequestExtra{Header: r.Header.Clone()}
	}
	select {
	case session.incoming <- msg:
		w.WriteHeader(http.StatusAccepted)
	case <-session.done:
		http.Error(w, "MCP 会话已关闭", http.StatusGone)
	case <-r.Context().Done():
	}
}

func (e *mcpEndpoint) connect(w http.ResponseWriter, r *http.Request, cred config.MCPCredential) {
	e.mu.Lock()
	if e.closed || len(e.sessions) >= e.server.cfg.Limits.MaxOnlineConnections {
		e.mu.Unlock()
		http.Error(w, "MCP 连接数达到上限", http.StatusServiceUnavailable)
		return
	}
	id := rand.Text()
	session := &mcpSSESession{
		SSEServerTransport: &mcp.SSEServerTransport{Endpoint: "/mcp/message?sessionid=" + id, Response: w},
		credential:         cred, incoming: make(chan jsonrpc.Message, 16), done: make(chan struct{}),
	}
	e.sessions[id] = session
	e.mu.Unlock()
	defer func() {
		e.mu.Lock()
		delete(e.sessions, id)
		e.mu.Unlock()
	}()
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache, no-transform")
	w.Header().Set("X-Accel-Buffering", "no")
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Minute)
	defer cancel()
	ss, err := e.sdk.Connect(ctx, session, nil)
	if err != nil {
		return
	}
	defer ss.Close()
	select {
	case <-ctx.Done():
	case <-session.done:
	}
}

// mcpSSESession 复用 SDK 的 SSE 编码；只补充逐 POST 的请求上下文。
type mcpSSESession struct {
	*mcp.SSEServerTransport
	mcp.Connection
	credential config.MCPCredential
	incoming   chan jsonrpc.Message
	done       chan struct{}
	once       sync.Once
	connMu     sync.Mutex
}

func (s *mcpSSESession) Connect(ctx context.Context) (mcp.Connection, error) {
	conn, err := s.SSEServerTransport.Connect(ctx)
	if err != nil {
		return nil, err
	}
	s.connMu.Lock()
	defer s.connMu.Unlock()
	s.Connection = conn
	select {
	case <-s.done:
		conn.Close()
		return nil, io.EOF
	default:
	}
	return s, nil
}

func (s *mcpSSESession) Read(ctx context.Context) (jsonrpc.Message, error) {
	select {
	case msg := <-s.incoming:
		return msg, nil
	case <-s.done:
		return nil, io.EOF
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func (s *mcpSSESession) Close() error {
	var err error
	s.once.Do(func() {
		close(s.done)
		s.connMu.Lock()
		defer s.connMu.Unlock()
		if s.Connection != nil {
			err = s.Connection.Close()
		}
	})
	return err
}

func (e *mcpEndpoint) close() {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.closed = true
	for _, session := range e.sessions {
		session.Close()
	}
}

func (e *mcpEndpoint) propose(ctx context.Context, req *mcp.CallToolRequest) (*mcp.CallToolResult, error) {
	var result contracts.Request
	var err error
	if req.Extra == nil {
		err = domain.ErrUnauthenticated("缺少 MCP 调用上下文")
	} else {
		r := &http.Request{Header: req.Extra.Header}
		var cred config.MCPCredential
		cred, err = e.server.authenticateMCP(r)
		if err == nil {
			var device domain.DeviceID
			device, err = domain.NormalizeMAC(singleHeader(r.Header, "X-Linx-Device-Id"))
			if err == nil && (string(device) != cred.DeviceID || singleHeader(r.Header, "X-Linx-Agent-Id") != cred.AgentID) {
				err = domain.ErrPermissionDenied("灵矽来源与 MCP 凭据不匹配")
			}
			if err == nil {
				err = e.server.validator.ValidateMCPArguments(req.Params.Arguments)
				if err != nil {
					err = domain.ErrInvalidArgument("工具只接受非空最终识别文字 text")
				}
			}
			if err == nil {
				var args struct {
					Text string `json:"text"`
				}
				if json.Unmarshal(req.Params.Arguments, &args) != nil {
					err = domain.ErrInvalidArgument("工具参数无法解析")
				} else {
					callCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
					defer cancel()
					result, err = e.server.requests.Propose(callCtx, device,
						singleHeader(r.Header, "X-Linx-Request-Id"), cred.AgentID,
						singleHeader(r.Header, "X-Linx-Session-Id"), args.Text)
				}
			}
		}
	}
	body := contracts.FirmwareResponseBody{OK: err == nil}
	if err != nil {
		body.Error = wireError(err)
	} else {
		body.Data = result
	}
	raw, _ := json.Marshal(body)
	if e.server.validator.ValidateMCPResult(raw) != nil {
		err = domain.ErrInternal("MCP 返回不符合共享契约")
		raw, _ = json.Marshal(contracts.FirmwareResponseBody{OK: false, Error: wireError(err)})
	}
	return &mcp.CallToolResult{Content: []mcp.Content{&mcp.TextContent{Text: string(raw)}}, IsError: err != nil}, nil
}

func singleHeader(header http.Header, key string) string {
	values := header.Values(key)
	if len(values) != 1 {
		return ""
	}
	return values[0]
}
