package transport

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"io"
	"log/slog"
	"net/http"
	"strconv"
	"time"

	"github.com/modelcontextprotocol/go-sdk/jsonrpc"

	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

const mcpSessionHeader = "Mcp-Session-Id"

func mcpCredentialIdentity(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

// 只输出会话摘要编号，保留 JSON 数值精度；不记录可用于寻址的真实会话 ID。
func mcpSessionLogID(session string) uint64 {
	if session == "" {
		return 0
	}
	sum := sha256.Sum256([]byte(session))
	return binary.BigEndian.Uint64(sum[:8]) >> 11
}

// 调用者持有 e.mu；新建 Streamable 会话期间持锁，避免并发初始化绕过总量限制。
func (e *mcpEndpoint) sessionCountLocked() int {
	count := len(e.sessions)
	for range e.streamSDK.Sessions() {
		count++
	}
	return count
}

func (e *mcpEndpoint) serveStreamable(w http.ResponseWriter, r *http.Request, trace string) {
	if len(r.Header.Values(mcpSessionHeader)) > 1 {
		e.server.rejectMCPHTTP(w, r, trace, 400, domain.CodeInvalidArgument, "Mcp-Session-Id 不能重复")
		return
	}
	sessionID := singleHeader(r.Header, mcpSessionHeader)
	r = r.Clone(r.Context())
	r.Header.Set(mcpLogTraceHeader, trace)
	r.Header.Set(mcpLogConnectionHeader, strconv.FormatUint(mcpSessionLogID(sessionID), 10))
	r.Header.Set(mcpLogTransportHeader, "streamable_http")
	r.Header.Del(mcpLogRPCHeader)
	rpcMethod := ""
	if r.Method == http.MethodPost {
		r.Body = http.MaxBytesReader(w, r.Body, int64(e.server.cfg.Limits.MaxFrameBytes))
		raw, err := io.ReadAll(r.Body)
		if err != nil {
			e.server.rejectMCPHTTP(w, r, trace, 413, domain.CodeResourceExhausted, "MCP 消息读取失败或超限")
			return
		}
		msg, err := jsonrpc.DecodeMessage(raw)
		if err != nil {
			e.server.rejectMCPHTTP(w, r, trace, 400, domain.CodeInvalidArgument, "JSON-RPC 消息无效", e.server.bodyFields("body", raw)...)
			return
		}
		if req, ok := msg.(*jsonrpc.Request); ok {
			rpcMethod = req.Method
			r.Header.Set(mcpLogRPCHeader, e.server.mcpRPCID(req.ID.Raw()))
		}
		if sessionID == "" && rpcMethod != "initialize" {
			e.server.rejectMCPHTTP(w, r, trace, 400, domain.CodeInvalidArgument, "请先 initialize，再携带返回的 Mcp-Session-Id")
			return
		}
		r.Body = io.NopCloser(bytes.NewReader(raw))
		r.ContentLength = int64(len(raw))
		ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
		defer cancel()
		r = r.WithContext(ctx)
	}

	newSession := r.Method == http.MethodPost && sessionID == ""
	e.mu.Lock()
	if e.closed || (newSession && e.sessionCountLocked() >= e.server.cfg.Limits.MaxOnlineConnections) {
		e.mu.Unlock()
		e.server.rejectMCPHTTP(w, r, trace, 503, domain.CodeResourceExhausted, "MCP 已关闭或连接数达到上限")
		return
	}
	if newSession {
		defer e.mu.Unlock()
	} else {
		e.mu.Unlock()
	}
	response := &mcpHTTPResponse{ResponseWriter: w}
	started := time.Now()
	if r.Method == http.MethodHead || r.Method == http.MethodOptions {
		// 只提供已认证的能力探测，不创建传输会话，也不开放浏览器跨源访问。
		response.Header().Set("Allow", "GET, POST, DELETE, HEAD, OPTIONS")
		status := http.StatusOK
		if r.Method == http.MethodOptions {
			status = http.StatusNoContent
		}
		response.WriteHeader(status)
	} else {
		e.stream.ServeHTTP(response, r)
	}
	if sessionID == "" {
		sessionID = response.Header().Get(mcpSessionHeader)
	}
	e.logStreamableHTTP(r, trace, rpcMethod, sessionID, response.statusCode(), started)
}

func (e *mcpEndpoint) logStreamableHTTP(r *http.Request, trace, rpcMethod, session string, status int, started time.Time) {
	event, message, level := "mcp.http.completed", "Streamable HTTP 请求处理结束；工具业务结果见 mcp.call.completed", slog.LevelInfo
	fields := []any{"trace_id", trace, "transport", "streamable_http", "method", e.server.safeMCPMethod(r.Method),
		"rpc_method", safeMCPRPCMethod(rpcMethod), "rpc_id", singleHeader(r.Header, mcpLogRPCHeader),
		"mcp_connection_id", mcpSessionLogID(session), "http_status", status, "duration_ms", elapsedMillis(started)}
	if status >= 400 {
		event, message, level = "mcp.http.rejected", "Streamable HTTP 请求被拒绝，请检查协议头与传输会话", slog.LevelWarn
		code := domain.CodeInvalidArgument
		if status == 404 {
			code = domain.CodeNotFound
		}
		if status == 403 {
			code = domain.CodePermissionDenied
		}
		diagnostics := errorFields(domain.NewError(code, message), "gateway")
		setErrorHint(diagnostics, mcpHTTPHint(status))
		fields = append(fields, diagnostics...)
	}
	fields = append(fields, "event", event)
	e.server.logger.Log(r.Context(), level, message, fields...)
}

func safeMCPRPCMethod(method string) string {
	switch method {
	case "", "initialize", "notifications/initialized", "tools/list", "tools/call", "ping", "notifications/cancelled":
		return method
	default:
		return "OTHER"
	}
}

// SDK 的 GET 流需要 Flush；只采集状态，不捕获可能含正文或凭据的响应内容。
type mcpHTTPResponse struct {
	http.ResponseWriter
	status int
}

func (w *mcpHTTPResponse) WriteHeader(status int) {
	if w.status == 0 {
		w.status = status
		w.ResponseWriter.WriteHeader(status)
	}
}

func (w *mcpHTTPResponse) Write(p []byte) (int, error) {
	if w.status == 0 {
		w.WriteHeader(http.StatusOK)
	}
	return w.ResponseWriter.Write(p)
}

func (w *mcpHTTPResponse) Flush() {
	if w.status == 0 {
		w.WriteHeader(http.StatusOK)
	}
	_ = http.NewResponseController(w.ResponseWriter).Flush()
}

func (w *mcpHTTPResponse) statusCode() int {
	if w.status == 0 {
		return http.StatusOK
	}
	return w.status
}
