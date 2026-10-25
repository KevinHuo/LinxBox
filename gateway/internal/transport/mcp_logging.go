package transport

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

// 这些头只存在于进程内的 RequestExtra 副本，由网关覆盖写入，不参与鉴权或业务路由。
const (
	mcpLogTraceHeader      = "X-Linx-Gateway-Log-Trace"
	mcpLogConnectionHeader = "X-Linx-Gateway-Log-Connection"
	mcpLogRPCHeader        = "X-Linx-Gateway-Log-Rpc"
	mcpLogTransportHeader  = "X-Linx-Gateway-Log-Transport"
)

func newMCPTrace() string { return contracts.NewMessageID("mcp_")[:12] }

func (s *Server) safeMCPID(value string) string {
	if value == "" {
		return ""
	}
	if !domain.ValidateID(value) || !diagnosticLiteral(value) {
		return "[标识已脱敏]"
	}
	for _, credential := range s.cfg.Credentials.Devices {
		if credential.Token != "" && strings.Contains(value, credential.Token) {
			return "[标识已脱敏]"
		}
	}
	for _, credential := range s.cfg.Credentials.Desktops {
		if credential.Token != "" && strings.Contains(value, credential.Token) {
			return "[标识已脱敏]"
		}
	}
	for _, credential := range s.cfg.Credentials.MCP {
		if credential.Token != "" && strings.Contains(value, credential.Token) {
			return "[标识已脱敏]"
		}
	}
	return value
}

type mcpCallLog struct {
	transport                                          string
	trace, rpcID, device, agent, voiceSession, request string
	expectedDevice, expectedAgent                      string
	connection                                         uint64
	missing, duplicate                                 []string
	arguments                                          json.RawMessage
	textBytes, textCodepoints                          int
	textIsString, textNonBlank                         bool
}

func (s *Server) mcpCallContext(req *mcp.CallToolRequest) mcpCallLog {
	var header http.Header
	if req.Extra != nil {
		header = req.Extra.Header
	}
	entry := mcpCallLog{trace: singleHeader(header, mcpLogTraceHeader)}
	entry.transport = "sse"
	if singleHeader(header, mcpLogTransportHeader) == "streamable_http" {
		entry.transport = "streamable_http"
	}
	validTrace := len(entry.trace) == 12 && strings.HasPrefix(entry.trace, "mcp_")
	if validTrace {
		_, err := strconv.ParseUint(entry.trace[4:], 16, 32)
		validTrace = err == nil
	}
	if !validTrace {
		entry.trace = newMCPTrace()
	}
	entry.connection, _ = strconv.ParseUint(singleHeader(header, mcpLogConnectionHeader), 10, 64)
	entry.rpcID = s.safeMCPID(singleHeader(header, mcpLogRPCHeader))
	entry.device = s.safeMCPID(singleHeader(header, "X-Linx-Device-Id"))
	if device, err := domain.NormalizeMAC(singleHeader(header, "X-Linx-Device-Id")); err == nil {
		entry.device = s.safeMCPID(string(device))
	}
	entry.agent = s.safeMCPID(singleHeader(header, "X-Linx-Agent-Id"))
	entry.voiceSession = s.safeMCPID(singleHeader(header, "X-Linx-Session-Id"))
	entry.request = s.safeMCPID(singleHeader(header, "X-Linx-Request-Id"))
	for _, key := range []string{"X-Linx-Device-Id", "X-Linx-Agent-Id", "X-Linx-Session-Id", "X-Linx-Request-Id"} {
		values := header.Values(key)
		if len(values) == 0 || (len(values) == 1 && values[0] == "") {
			entry.missing = append(entry.missing, key)
		}
		if len(values) > 1 {
			entry.duplicate = append(entry.duplicate, key)
		}
	}
	if req.Params != nil {
		entry.arguments = req.Params.Arguments
	}
	var args struct {
		Text *string `json:"text"`
	}
	if json.Unmarshal(entry.arguments, &args) == nil && args.Text != nil {
		entry.textIsString = true
		entry.textBytes = len(*args.Text)
		entry.textCodepoints = utf8.RuneCountInString(*args.Text)
		entry.textNonBlank = strings.TrimSpace(*args.Text) != ""
	}
	return entry
}

func (entry mcpCallLog) fields() []any {
	return []any{"trace_id", entry.trace, "transport", entry.transport, "tool", "propose_task_reply", "mcp_connection_id", entry.connection, "rpc_id", entry.rpcID,
		"device_id", entry.device, "linx_agent_id", entry.agent, "voice_session_id", entry.voiceSession, "request_id", entry.request,
		"credential_device_id", entry.expectedDevice, "credential_linx_agent_id", entry.expectedAgent,
		"missing_headers", entry.missing, "duplicate_headers", entry.duplicate, "text_is_string", entry.textIsString,
		"text_non_blank", entry.textNonBlank, "text_bytes", entry.textBytes, "text_codepoints", entry.textCodepoints, "text_source", "mcp_tool_argument"}
}

func (s *Server) logMCPCall(event, stage string, entry mcpCallLog, started time.Time, result contracts.Request, err error) {
	level, message := slog.LevelInfo, "MCP → Gateway：收到文字提案工具调用，开始校验"
	fields := []any{"event", event, "stage", stage, "direction", "MCP客户端→Gateway"}
	fields = append(fields, entry.fields()...)
	switch event {
	case "mcp.call.received":
		fields = append(fields, s.bodyFields("body", entry.arguments)...)
	case "mcp.call.validated":
		message = "MCP 来源和参数 Schema 校验通过，开始核对原请求并保存文字"
	case "mcp.call.completed":
		message = "MCP 提案已接受，返回当前请求快照"
		fields = append(fields, "ok", err == nil, "status", result.Status, "agent_session_id", s.safeMCPID(result.AgentSessionID), "duration_ms", elapsedMillis(started))
		response := contracts.FirmwareResponseBody{OK: err == nil, Data: result}
		if err != nil {
			level = slog.LevelWarn
			message = "MCP 提案处理失败：" + domain.MessageOf(err)
			fields = append(fields, "failed_stage", stage)
			diagnostics := errorFields(err, "gateway")
			setErrorHint(diagnostics, mcpBusinessHint(domain.CodeOf(err)))
			fields = append(fields, diagnostics...)
			fields = append(fields, "error_type", fmt.Sprintf("%T", err))
			response.Data, response.Error = nil, wireError(err)
		}
		fields = append(fields, s.logValueFields("response_body", response)...)
	}
	s.logger.Log(context.Background(), level, message, fields...)
}

func (s *Server) rejectMCPHTTP(w http.ResponseWriter, r *http.Request, trace string, status int, code domain.Code, message string, attrs ...any) {
	if s.logger != nil {
		fields := []any{"event", "mcp.http.rejected", "trace_id", trace, "http_status", status, "method", s.safeMCPMethod(r.Method), "path", r.URL.Path, "remote_addr", r.RemoteAddr}
		diagnostics := errorFields(domain.NewError(code, message), "gateway")
		setErrorHint(diagnostics, mcpHTTPHint(status))
		fields = append(fields, diagnostics...)
		if s.cfg != nil {
			for _, entry := range []struct{ key, header string }{{"claimed_request_id", "X-Linx-Request-Id"}, {"claimed_device_id", "X-Linx-Device-Id"}} {
				if value := singleHeader(r.Header, entry.header); value != "" {
					fields = append(fields, entry.key, s.safeMCPID(value))
				}
			}
		}
		s.logger.Warn("MCP HTTP 请求被拒绝："+message, append(fields, attrs...)...)
	}
	http.Error(w, message, status)
}

func setErrorHint(fields []any, hint string) {
	for i := 0; i+1 < len(fields); i += 2 {
		if fields[i] == "error_hint" {
			fields[i+1] = hint
			return
		}
	}
}

func mcpBusinessHint(code domain.Code) string {
	switch code {
	case domain.CodeUnauthenticated:
		return "使用独立 MCP Bearer 凭据，并在每次 POST 携带认证上下文"
	case domain.CodeInvalidArgument:
		return "核对 missing_headers/duplicate_headers；每轮透传四个来源头，text 必须是非空字符串且满足字节/码点限制"
	case domain.CodeNotFound:
		return "检查本轮 request_id 是否已由固件 request.prepare 登记；SSE sessionid 和 JSON-RPC id 都不是业务请求 ID"
	case domain.CodeConflict:
		return "检查原请求是否取消、过期或文字/来源已固定；同轮重试须保持原文及来源一致"
	case domain.CodePermissionDenied:
		return "核对 MCP 凭据绑定的设备/智能体及原请求、目标会话的账号归属"
	default:
		return "结合 failed_stage 检查 Gateway 存储、上下文期限和响应 Schema"
	}
}

func mcpHTTPHint(status int) string {
	switch status {
	case http.StatusUnauthorized:
		return "每个 MCP HTTP 请求都须携带 credentials.mcp 中的独立 Bearer token，不能复用固件或 Desktop token"
	case http.StatusForbidden:
		return "拒绝浏览器 Origin 或不安全 Host；传输会话必须使用初始化时的同一凭据"
	case http.StatusNotFound, http.StatusGone:
		return "Streamable HTTP 重新 POST /mcp initialize，使用返回的 Mcp-Session-Id；旧 SSE 重新 GET /mcp 并使用 endpoint 地址。传输会话 ID 不是业务 request_id"
	case http.StatusMethodNotAllowed:
		return "Streamable HTTP 使用 POST /mcp，GET/DELETE 携带 Mcp-Session-Id；旧 SSE 使用 GET /mcp 与 POST /mcp/message。核对日志中的实际 method"
	case http.StatusRequestEntityTooLarge:
		return "检查消息是否超过 limits.max_frame_bytes，或 HTTP 请求体是否读取中断"
	case http.StatusBadRequest:
		return "检查 JSON-RPC 2.0 信封、Accept（application/json 与 text/event-stream）、Mcp-Session-Id 和 MCP-Protocol-Version；先完成 initialize"
	case http.StatusUnsupportedMediaType:
		return "Streamable HTTP 的 POST 必须使用 Content-Type: application/json"
	default:
		return "检查 MCP 服务配置、SSE 连接数量及 Gateway 启动状态"
	}
}

func (s *Server) safeMCPMethod(method string) string {
	switch method {
	case "GET", "POST", "DELETE", "HEAD", "OPTIONS", "PUT", "PATCH", "CONNECT", "TRACE":
		return method
	}
	return "OTHER"
}

func (s *Server) mcpRPCID(value any) string {
	if value == nil {
		return ""
	}
	return s.safeMCPID(fmt.Sprint(value))
}
