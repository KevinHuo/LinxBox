package transport

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
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
	if err := requests.Recover(context.Background()); err != nil {
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
		s.rejectMCPHTTP(w, r, newMCPTrace(), http.StatusServiceUnavailable, domain.CodeDependencyUnavailable, "MCP 服务未配置")
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
	trace := newMCPTrace()
	cred, err := e.server.authenticateMCP(r)
	if err != nil {
		w.Header().Set("WWW-Authenticate", "Bearer")
		e.server.rejectMCPHTTP(w, r, trace, http.StatusUnauthorized, domain.CodeUnauthenticated, "MCP 凭据无效")
		return
	}
	// 本入口供服务端调用，拒绝浏览器跨源连接。
	if r.Header.Get("Origin") != "" {
		e.server.rejectMCPHTTP(w, r, trace, http.StatusForbidden, domain.CodePermissionDenied, "不接受浏览器来源")
		return
	}
	if r.Method == http.MethodGet && r.URL.Path == "/mcp" {
		e.connect(w, r, cred)
		return
	}
	if r.Method != http.MethodPost || r.URL.Path != "/mcp/message" {
		w.Header().Set("Allow", "GET, POST")
		e.server.rejectMCPHTTP(w, r, trace, http.StatusMethodNotAllowed, domain.CodeInvalidArgument, "入口或方法不匹配")
		return
	}
	e.mu.Lock()
	session := e.sessions[r.URL.Query().Get("sessionid")]
	e.mu.Unlock()
	if session == nil {
		e.server.rejectMCPHTTP(w, r, trace, http.StatusNotFound, domain.CodeNotFound, "MCP 会话不存在")
		return
	}
	if session.credential != cred {
		e.server.rejectMCPHTTP(w, r, trace, http.StatusForbidden, domain.CodePermissionDenied, "MCP 会话不属于此凭据")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, int64(e.server.cfg.Limits.MaxFrameBytes))
	raw, err := io.ReadAll(r.Body)
	if err != nil {
		e.server.rejectMCPHTTP(w, r, trace, http.StatusRequestEntityTooLarge, domain.CodeResourceExhausted, "MCP 消息读取失败或超限", "mcp_connection_id", session.logID, "max_frame_bytes", e.server.cfg.Limits.MaxFrameBytes)
		return
	}
	if !json.Valid(raw) {
		fields := e.server.bodyFields("body", raw)
		e.server.rejectMCPHTTP(w, r, trace, http.StatusBadRequest, domain.CodeInvalidArgument, "MCP 消息无效", fields...)
		return
	}
	msg, err := jsonrpc.DecodeMessage(raw)
	if err != nil {
		fields := e.server.bodyFields("body", raw)
		e.server.rejectMCPHTTP(w, r, trace, http.StatusBadRequest, domain.CodeInvalidArgument, "JSON-RPC 消息无效", fields...)
		return
	}
	if req, ok := msg.(*jsonrpc.Request); ok {
		// SSE SDK 默认不透传 POST 头；把本次头随消息传入 SDK，禁止缓存 GET 的逐轮上下文。
		header := r.Header.Clone()
		// 仅携带日志元数据，覆盖同名客户端头；不能把它们用于认证或请求归属。
		header.Set(mcpLogTraceHeader, trace)
		header.Set(mcpLogConnectionHeader, strconv.FormatUint(session.logID, 10))
		header.Set(mcpLogRPCHeader, e.server.mcpRPCID(req.ID.Raw()))
		req.Extra = &mcp.RequestExtra{Header: header}
	}
	select {
	case session.incoming <- msg:
		e.server.logger.Debug("MCP HTTP 消息已入队；202 不代表工具业务成功", "event", "mcp.rpc.accepted", "trace_id", trace, "mcp_connection_id", session.logID, "http_status", http.StatusAccepted)
		w.WriteHeader(http.StatusAccepted)
	case <-session.done:
		e.server.rejectMCPHTTP(w, r, trace, http.StatusGone, domain.CodeDependencyUnavailable, "MCP 会话已关闭", "mcp_connection_id", session.logID)
	case <-r.Context().Done():
	}
}

func (e *mcpEndpoint) connect(w http.ResponseWriter, r *http.Request, cred config.MCPCredential) {
	started := time.Now()
	trace := newMCPTrace()
	e.mu.Lock()
	if e.closed || len(e.sessions) >= e.server.cfg.Limits.MaxOnlineConnections {
		e.mu.Unlock()
		e.server.rejectMCPHTTP(w, r, trace, http.StatusServiceUnavailable, domain.CodeResourceExhausted, "MCP 连接数达到上限")
		return
	}
	id := rand.Text()
	session := &mcpSSESession{
		SSEServerTransport: &mcp.SSEServerTransport{Endpoint: "/mcp/message?sessionid=" + id, Response: w},
		credential:         cred, incoming: make(chan jsonrpc.Message, 16), done: make(chan struct{}),
		logID: e.server.conns.NextGeneration(),
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
		e.server.logger.Warn("MCP SSE 建连失败，工具调用尚未开始", "event", "mcp.connection.failed", "trace_id", trace, "mcp_connection_id", session.logID, "device_id", e.server.safeMCPID(cred.DeviceID), "error_message", "SSE 传输未能建立，请检查流式响应支持和客户端连接")
		return
	}
	e.server.logger.Info("MCP SSE 连接已建立，等待初始化及工具调用", "event", "mcp.connection.opened", "trace_id", trace, "mcp_connection_id", session.logID, "device_id", e.server.safeMCPID(cred.DeviceID), "linx_agent_id", e.server.safeMCPID(cred.AgentID))
	defer ss.Close()
	select {
	case <-ctx.Done():
	case <-session.done:
	}
	reason := "client_disconnected"
	if ctx.Err() == context.DeadlineExceeded {
		reason = "lifetime_expired"
	}
	e.mu.Lock()
	if e.closed {
		reason = "gateway_shutdown"
	}
	e.mu.Unlock()
	e.server.logger.Info("MCP SSE 连接已结束", "event", "mcp.connection.closed", "trace_id", trace, "mcp_connection_id", session.logID, "device_id", e.server.safeMCPID(cred.DeviceID), "end_reason", reason, "duration_ms", elapsedMillis(started))
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
	logID      uint64
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
	started := time.Now()
	entry := e.server.mcpCallContext(req)
	stage := "context"
	e.server.logMCPCall("mcp.call.received", "received", entry, started, contracts.Request{}, nil)
	var result contracts.Request
	var err error
	if req.Extra == nil {
		err = domain.ErrUnauthenticated("缺少 MCP 调用上下文")
	} else {
		stage = "authentication"
		r := &http.Request{Header: req.Extra.Header}
		var cred config.MCPCredential
		cred, err = e.server.authenticateMCP(r)
		if err == nil {
			entry.expectedDevice = e.server.safeMCPID(cred.DeviceID)
			entry.expectedAgent = e.server.safeMCPID(cred.AgentID)
			stage = "context"
			var device domain.DeviceID
			device, err = domain.NormalizeMAC(singleHeader(r.Header, "X-Linx-Device-Id"))
			if err == nil && (string(device) != cred.DeviceID || singleHeader(r.Header, "X-Linx-Agent-Id") != cred.AgentID) {
				err = domain.ErrPermissionDenied("灵矽来源与 MCP 凭据不匹配")
			}
			if err == nil {
				stage = "arguments"
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
					stage = "persist"
					if !domain.ValidateID(singleHeader(r.Header, "X-Linx-Request-Id")) || !domain.ValidateID(singleHeader(r.Header, "X-Linx-Session-Id")) {
						stage = "context"
					} else {
						e.server.logMCPCall("mcp.call.validated", "source_and_schema", entry, started, contracts.Request{}, nil)
					}
					callCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
					defer cancel()
					result, err = e.server.requests.Propose(callCtx, device,
						singleHeader(r.Header, "X-Linx-Request-Id"), cred.AgentID,
						singleHeader(r.Header, "X-Linx-Session-Id"), args.Text)
					if err != nil && stage != "context" {
						switch domain.CodeOf(err) {
						case domain.CodeNotFound, domain.CodeConflict, domain.CodePermissionDenied:
							stage = "request_state"
						case domain.CodeInvalidArgument:
							stage = "arguments"
						}
					}
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
		stage = "response"
		err = domain.ErrInternal("MCP 返回不符合共享契约")
		raw, _ = json.Marshal(contracts.FirmwareResponseBody{OK: false, Error: wireError(err)})
	}
	if err == nil {
		stage = "completed"
	}
	e.server.logMCPCall("mcp.call.completed", stage, entry, started, result, err)
	return &mcp.CallToolResult{Content: []mcp.Content{&mcp.TextContent{Text: string(raw)}}, IsError: err != nil}, nil
}

func singleHeader(header http.Header, key string) string {
	values := header.Values(key)
	if len(values) != 1 {
		return ""
	}
	return values[0]
}
