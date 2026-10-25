package transport

import (
	"bytes"
	"encoding/json"
	"strings"
	"unicode/utf8"

	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

const maxLogBodyBytes = 4096

// 仅输出协议诊断字段；正文、标题、路径、凭据及未知扩展字段保留脱敏标记。
var visibleBodyFields = map[string]bool{
	"id": true, "type": true, "message_id": true, "reply_to": true, "reply_to_message_id": true,
	"request_id": true, "agent_session_id": true, "device_id": true, "mac_address": true, "desktop_client_id": true,
	"thread_id": true, "project_id": true, "protocol_version": true, "sequence": true, "sent_at": true, "expires_at": true,
	"status": true, "role": true, "agent": true, "offset": true, "next_offset": true, "cursor": true, "next_cursor": true,
	"ok": true, "code": true, "desktop_version": true, "last_acked_sequence": true, "client_time": true,
	"payload": true, "data": true, "items": true, "error": true,
}

func (s *Server) bodyFields(key string, raw []byte) []any {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	var body any
	if decoder.Decode(&body) != nil || !json.Valid(raw) {
		return []any{key, "[非 JSON，原始内容已隐藏]", key + "_bytes", len(raw)}
	}
	redacted, truncated := false, false
	var clean func(any, int) any
	clean = func(value any, depth int) any {
		if depth > 6 {
			truncated = true
			return "[层级已截断]"
		}
		switch value := value.(type) {
		case map[string]any:
			out := map[string]any{}
			for field, child := range value {
				if field == "message" {
					text, ok := child.(string)
					if ok {
						var hidden bool
						out[field], hidden = peerErrorMessage(text)
						redacted = redacted || hidden
					} else {
						out[field] = "[已脱敏]"
						redacted = true
					}
				} else if field == "text" || field == "title" || field == "summary" || field == "target_title" || field == "project_name" || field == "token" || field == "authorization" || field == "root_path" {
					out[field] = nil
					if child != nil {
						out[field] = "[已脱敏]"
						redacted = true
					}
				} else if field == "mac_address" || field == "device_id" {
					text, ok := child.(string)
					if _, err := domain.NormalizeMAC(text); ok && err == nil {
						out[field] = text
					} else {
						out[field] = "[非法 MAC，值已隐藏]"
						redacted = true
					}
				} else if field == "payload" || field == "data" || field == "items" || field == "error" {
					switch child.(type) {
					case map[string]any, []any, nil:
						out[field] = clean(child, depth+1)
					default:
						out[field] = "[结构不合法，值已隐藏]"
						redacted = true
					}
				} else if visibleBodyFields[field] {
					if text, ok := child.(string); ok && !diagnosticLiteral(text) {
						out[field] = "[非诊断标识，值已隐藏]"
						redacted = true
					} else {
						out[field] = clean(child, depth+1)
					}
				} else {
					// 不记录未知字段名，字段名本身也可能携带私人内容。
					redacted = true
					out["redacted_fields"] = true
				}
			}
			return out
		case []any:
			count := len(value)
			if count > 3 {
				count = 3
				truncated = true
			}
			out := make([]any, 0, count)
			for _, child := range value[:count] {
				if _, ok := child.(string); ok {
					out = append(out, "[标量项已脱敏]")
					redacted = true
				} else {
					out = append(out, clean(child, depth+1))
				}
			}
			return out
		case string:
			if !diagnosticLiteral(value) {
				redacted = true
				return "[非诊断标识，值已隐藏]"
			}
			for _, credential := range s.cfg.Credentials.Devices {
				if credential.Token != "" && strings.Contains(value, credential.Token) {
					value = strings.ReplaceAll(value, credential.Token, "[凭据已脱敏]")
					redacted = true
				}
			}
			for _, credential := range s.cfg.Credentials.Desktops {
				if credential.Token != "" && strings.Contains(value, credential.Token) {
					value = strings.ReplaceAll(value, credential.Token, "[凭据已脱敏]")
					redacted = true
				}
			}
			for _, credential := range s.cfg.Credentials.MCP {
				if credential.Token != "" && strings.Contains(value, credential.Token) {
					value = strings.ReplaceAll(value, credential.Token, "[凭据已脱敏]")
					redacted = true
				}
			}
			if len(value) > 256 {
				truncated = true
				return clipUTF8(value, 256) + "…"
			}
			return value
		default:
			return value
		}
	}
	body = clean(body, 0)
	encoded, err := json.Marshal(body)
	if err != nil || len(encoded) > maxLogBodyBytes {
		truncated = true
		body = map[string]any{"preview": clipUTF8(string(encoded), 1024), "truncated": true}
	}
	return []any{key, body, key + "_bytes", len(raw), key + "_redacted", redacted, key + "_truncated", truncated}
}

// 诊断字符串限定为无空白的协议标识；畸形字段不能借 ID/状态字段写入私人正文或路径。
func diagnosticLiteral(value string) bool {
	if strings.Contains(value, "/") || strings.Contains(value, "\\") {
		return false
	}
	for _, char := range value {
		if char < '!' || char > '~' || char == '"' {
			return false
		}
	}
	return true
}

func clipUTF8(value string, limit int) string {
	if len(value) <= limit {
		return value
	}
	value = value[:limit]
	for !utf8.ValidString(value) {
		value = value[:len(value)-1]
	}
	return value
}
