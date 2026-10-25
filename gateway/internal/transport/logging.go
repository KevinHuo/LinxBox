package transport

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"strings"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
)

// traceID 只是日志检索短号；协议仍使用完整消息 ID，不能用短号进行路由。
func traceID(generation uint64, messageID string) string {
	digest := sha256.Sum256([]byte(fmt.Sprintf("%d:%s", generation, messageID)))
	return fmt.Sprintf("c%d-%x", generation, digest[:4])
}

func databaseConfigID(path string) string {
	digest := sha256.Sum256([]byte(path))
	return fmt.Sprintf("%x", digest[:6])
}

func operationName(operation string) string {
	names := map[string]string{
		"device.hello": "登记设备 MAC", "device.bind": "绑定设备", "client.hello": "Desktop 上报连接信息",
		"agent_sessions.sync": "同步会话目录", "agent_sessions.list": "读取会话目录", "agent_session.get": "读取会话详情",
		"messages.list": "读取会话历史", "message.content.get": "读取消息正文", "message.available": "通知新消息",
		"request.prepare": "登记追问", "request.get": "查询追问状态", "request.cancel": "取消追问", "request.confirm": "确认追问",
		"query.result": "接收 Desktop 查询应答",
	}
	if name := names[operation]; name != "" {
		return name
	}
	return "处理消息"
}

// publicErrorMessage 仅放行 Gateway 生成的固定安全文案；未知内容仍按对端错误脱敏。
func publicErrorMessage(message string) (string, bool) {
	switch message {
	case "任务发送结果不确定，请核对原任务；不会自动重发", "Desktop 未接受任务投递",
		"完成事件缺少一致的成功结果，不能确认任务成功", "Desktop 报告任务执行失败，请在电脑端查看详情",
		"Desktop 报告任务中断，结果未确认；不会自动重发", "任务需要在电脑端处理审批，当前入口不自动授权",
		"连接中断或 Gateway 重启，执行结果未确认；不会自动重发":
		return message, false
	default:
		return peerErrorMessage(message)
	}
}

// peerErrorMessage 仅展示已知诊断文案；任意对端错误可能含正文、路径或凭据。
func peerErrorMessage(message string) (string, bool) {
	switch message {
	case "会话不属于当前 Desktop", "会话不存在", "消息不存在", "查询失败", "Desktop 查询超时", "Desktop 已断开连接", "无法向 Desktop 发送查询", "Desktop 未连接", "未收到文字", "请求确认已超时":
		return message, false
	case "缺少本轮请求或灵矽上下文", "灵矽来源与 MCP 凭据不匹配", "MCP 请求自动关联未开启",
		"MCP 账号范围无效", "MCP 设备尚未登记", "设备与 MCP 凭据不属于同一账号",
		"没有可关联的待处理请求，请先 request.prepare", "设备存在多个可关联请求，不能自动选择",
		"请求文字或灵矽来源已固定", "请求已取消或过期", domain.RequestRecoveryMessage:
		return message, false
	default:
		lower := strings.ToLower(message)
		for _, cause := range []struct{ match, summary string }{
			{"thread not found", "Desktop 底层未找到 Codex thread（扩展内容已脱敏）"},
			{"permission denied", "Desktop 查询相关操作权限不足（扩展内容已脱敏）"},
			{"eacces", "Desktop 查询相关操作权限不足（扩展内容已脱敏）"},
			{"timeout", "Desktop 查询相关操作超时（扩展内容已脱敏）"},
			{"超时", "Desktop 查询相关操作超时（扩展内容已脱敏）"},
		} {
			if strings.Contains(lower, cause.match) {
				return cause.summary, true
			}
		}
		return "Desktop 返回未分类错误，原始文案已脱敏", true
	}
}

func diagnosticMessage(err error, source string) (string, bool) {
	message := domain.MessageOf(err)
	if source == "desktop" {
		return peerErrorMessage(message)
	}
	if message == "" {
		return "连接不可用或处理失败", false
	}
	return message, false
}

func errorFields(err error, source string) []any {
	message, redacted := diagnosticMessage(err, source)
	hint := "结合 trace_id 检查对应步骤和服务端状态"
	switch domain.CodeOf(err) {
	case domain.CodeNotFound:
		hint = "核对会话/消息 ID；若错误来自 Desktop，检查本地会话映射与历史读取"
	case domain.CodePermissionDenied:
		hint = "核对双方账号、设备绑定及项目/会话授权"
	case domain.CodeUnauthenticated:
		hint = "核对当前入口所需的角色凭据及认证方式"
	case domain.CodeDependencyUnavailable:
		hint = "检查对端在线状态及查询是否超时"
	case domain.CodeInvalidArgument:
		hint = "检查 body 字段、消息外层、MAC、时间和 UTF-8 字节偏移"
	case domain.CodeConflict:
		hint = "检查绑定或请求当前状态；目录游标失效时重新读取首页"
	case domain.CodeResourceExhausted:
		hint = "检查连接、队列、在途查询数量及消息大小限制"
	}
	fields := []any{"error_code", domain.CodeOf(err), "error_message", message, "error_source", source, "error_hint", hint, "error_detail_redacted", redacted}
	var conflict *service.RequestConflict
	if errors.As(err, &conflict) {
		fields = append(fields, "target_status", conflict.TargetStatus, "blocking_request_count", len(conflict.Blockers))
		for i, blocker := range conflict.Blockers {
			suffix := ""
			if i > 0 {
				suffix = fmt.Sprintf("_%d", i+1)
			}
			fields = append(fields, "blocking_request_id"+suffix, blocker.RequestID, "blocking_status"+suffix, blocker.Status, "blocking_scope"+suffix, blocker.Scope)
		}
	}
	return fields
}

// logOperation 将业务结果与队列细节分层，避免一次失败重复输出多条 WARN。
func (s *Server) logOperation(event string, conn *lifecycle.Connection, operation, requestID string, err error, attrs ...any) {
	level := slog.LevelInfo
	message := operationName(operation) + "成功"
	fields := []any{"event", event, "trace_id", traceID(conn.Generation(), requestID), "connection_id", conn.Generation(), "type", operation, "message_id", requestID, "ok", err == nil}
	if err != nil {
		level = slog.LevelWarn
		reason, _ := diagnosticMessage(err, "gateway")
		message = operationName(operation) + "失败：" + reason
	}
	if event == "response.enqueued" {
		level = slog.LevelDebug
		message = "Gateway → 对端：响应已入队（尚未确认对端收到）"
		if err != nil {
			fields = append(fields, "error_code", domain.CodeOf(err))
		}
	} else if err != nil {
		diagnostics := errorFields(err, "gateway")
		if operation == "request.get" && domain.CodeOf(err) == domain.CodeNotFound {
			setErrorHint(diagnostics, "核对本轮 request.prepare 成功记录、网关实例与数据库配置/备份恢复；停止轮询缺失 ID，曾确认的任务先在 Desktop 核对，不能据此自动重发")
			fields = append(fields, "request_lookup", "missing", "data_source", "gateway_database", "recovery_action", "verify_original_task", "retry_same_request", false)
		}
		fields = append(fields, diagnostics...)
		if strings.HasPrefix(event, "response.") {
			reason, _ := diagnosticMessage(err, "gateway")
			message = "Gateway → 对端：回传响应失败：" + reason
		}
	}
	if event == "request.state" && err == nil {
		for i := 0; i+1 < len(attrs); i += 2 {
			if attrs[i] == "status" {
				states := map[string]string{"awaiting_text": "等待识别文字", "awaiting_confirmation": "等待用户确认", "cancelled": "已取消", "failed": "已失败"}
				if state := states[fmt.Sprint(attrs[i+1])]; state != "" {
					message += "：" + state
				}
			}
		}
	}
	s.logger.Log(context.Background(), level, message, append(fields, attrs...)...)
}

func (s *Server) logIncoming(conn *lifecycle.Connection, kind, operation, messageID, identity string, payload json.RawMessage) {
	level := slog.LevelInfo
	if operation == "query.result" || operation == "client.heartbeat" {
		level = slog.LevelDebug
	}
	fields := []any{"event", "request.received", "trace_id", traceID(conn.Generation(), messageID), "direction", kind + "→Gateway",
		"connection_id", conn.Generation(), "type", operation, "message_id", messageID}
	key := "device_id"
	if kind == "Desktop" {
		key = "client_id"
	}
	fields = append(fields, key, identity)
	fields = append(fields, s.bodyFields("body", payload)...)
	s.logger.Log(context.Background(), level, kind+" → Gateway："+operationName(operation), fields...)
}

func elapsedMillis(started time.Time) float64 {
	return float64(time.Since(started).Microseconds()) / 1000
}

func (s *Server) logInvalid(conn *lifecycle.Connection, kind string, raw []byte) {
	message := "消息字段不符合共享 Schema，未处理；检查 body 的必填字段、消息类型、MAC 和时间格式"
	if !json.Valid(raw) {
		message = "消息不是合法 JSON，未处理；检查引号、逗号及编码"
	}
	fields := []any{"event", "request.invalid", "connection_id", conn.Generation(), "direction", kind + "→Gateway",
		"error_code", domain.CodeInvalidArgument, "error_message", message}
	fields = append(fields, s.bodyFields("body", raw)...)
	s.logger.Warn(message, fields...)
}

func payloadReferences(raw json.RawMessage) []any {
	var payload struct {
		Session string `json:"agent_session_id"`
		Message string `json:"message_id"`
		Request string `json:"request_id"`
	}
	if json.Unmarshal(raw, &payload) != nil {
		return nil
	}
	var fields []any
	for _, entry := range []struct{ key, value string }{{"agent_session_id", payload.Session}, {"content_message_id", payload.Message}, {"request_id", payload.Request}} {
		if entry.value != "" {
			fields = append(fields, entry.key, entry.value)
		}
	}
	return fields
}

func (s *Server) logValueFields(key string, value any) []any {
	raw, err := json.Marshal(value)
	if err != nil {
		return []any{key, "[无法编码]"}
	}
	return s.bodyFields(key, raw)
}

func (s *Server) logQueryCompletion(q service.Query, data json.RawMessage, err error) {
	source, direction := "gateway", "Gateway"
	if q.EndReason == "desktop_result" {
		source, direction = "desktop", "Desktop→Gateway"
	}
	fields := []any{"event", "query.completed", "trace_id", traceID(q.DeviceGeneration, q.FirmwareID), "direction", direction,
		"query_id", q.ID, "message_id", q.FirmwareID, "desktop_message_id", q.DesktopMessageID, "type", q.Operation,
		"device_id", q.DeviceID, "client_id", q.DesktopID, "connection_id", q.DeviceGeneration, "desktop_connection_id", q.DesktopGeneration,
		"duration_ms", elapsedMillis(q.StartedAt), "end_reason", q.EndReason, "ok", err == nil}
	fields = append(fields, payloadReferences(q.Payload)...)
	fields = append(fields, s.bodyFields("body", q.Payload)...)
	response := contracts.FirmwareResponseBody{OK: err == nil, Data: data}
	level, message := slog.LevelInfo, "Desktop → Gateway："+operationName(q.Operation)+"成功"
	if err != nil {
		response.Data, response.Error = nil, wireError(err)
		fields = append(fields, errorFields(err, source)...)
		reason, _ := diagnosticMessage(err, source)
		level, message = slog.LevelWarn, direction+"："+operationName(q.Operation)+"失败："+reason
	}
	fields = append(fields, s.logValueFields("response_body", response)...)
	s.logger.Log(context.Background(), level, message, fields...)
}

func (s *Server) finishFirmware(conn *lifecycle.Connection, p Principal, msg contracts.FirmwareMessage, started time.Time, data any, err error) {
	fields := []any{"direction", "Gateway→固件", "device_id", p.DeviceID, "duration_ms", elapsedMillis(started)}
	fields = append(fields, payloadReferences(msg.Payload)...)
	response := contracts.FirmwareResponseBody{OK: err == nil, Data: data}
	if err != nil {
		response.Data, response.Error = nil, wireError(err)
	}
	fields = append(fields, s.logValueFields("response_body", response)...)
	if msg.Type == "agent_sessions.list" || msg.Type == "agent_session.get" {
		fields = append(fields, "data_source", "gateway_catalog")
	}
	s.logOperation("request.completed", conn, msg.Type, msg.ID, err, fields...)
	s.respondFirmware(conn, msg.Type, msg.ID, data, err)
}

func websocketCloseCode(err error) int {
	var closeError *websocket.CloseError
	if errors.As(err, &closeError) {
		return closeError.Code
	}
	return 0
}
