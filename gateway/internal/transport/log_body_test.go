package transport

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func fieldMap(fields []any) map[string]any {
	out := map[string]any{}
	for i := 0; i+1 < len(fields); i += 2 {
		out[fields[i].(string)] = fields[i+1]
	}
	return out
}

func TestBodyLogsPreserveParametersAndRedactContent(t *testing.T) {
	s := &Server{cfg: &config.Config{Credentials: config.CredentialsConfig{
		Devices: []config.DeviceCredential{{Token: "firmware-secret"}}, Desktops: []config.DesktopCredential{{Token: "desktop-secret"}}, MCP: []config.MCPCredential{{Token: "mcp-secret"}},
	}}}
	const private = "私人标题正文及路径 /Users/private/project"
	input := map[string]any{"agent_session_id": "sess-1", "message_id": "msg-1", "offset": 12, "request_id": "req-1", "mac_address": "34:00:00:00:00:01",
		"text": private, "title": private, "summary": private, "token": "firmware-secret", "authorization": "desktop-secret", "unknown-private-key": private,
		"data":   map[string]any{"text": nil, "items": []any{map[string]any{"message_id": "m1", "summary": private}}},
		"error":  map[string]any{"code": "NOT_FOUND", "message": "会话不存在"},
		"cursor": "firmware-secret-desktop-secret-mcp-secret"}
	raw, err := json.Marshal(input)
	if err != nil {
		t.Fatal(err)
	}
	fields := fieldMap(s.bodyFields("body", raw))
	body := fields["body"].(map[string]any)
	if body["agent_session_id"] != "sess-1" || body["offset"] != json.Number("12") || body["mac_address"] != "34:00:00:00:00:01" || body["text"] != "[已脱敏]" {
		t.Fatal("诊断参数或脱敏字段错误", body)
	}
	if fields["body_redacted"] != true || body["error"].(map[string]any)["message"] != "会话不存在" {
		t.Fatal("已知错误文案应可读", fields)
	}
	encoded, _ := json.Marshal(fields)
	for _, secret := range []string{private, "firmware-secret", "desktop-secret", "mcp-secret", "unknown-private-key"} {
		if strings.Contains(string(encoded), secret) {
			t.Fatal("body 泄露敏感内容", secret)
		}
	}
	for _, invalid := range []string{`{`, `{} trailing-private`, `{"mac_address":"` + private + `","payload":"` + private + `","message":42}`} {
		fields := fieldMap(s.bodyFields("body", []byte(invalid)))
		encoded, _ := json.Marshal(fields)
		if strings.Contains(string(encoded), private) {
			t.Fatal("畸形消息也不能泄露", string(encoded))
		}
	}
	for _, invalid := range []string{`"` + private + `"`, `{"data":["` + private + `","single-private-word"]}`, `{"offset":"` + private + `"}`, `{"cursor":"/private/file"}`} {
		encoded, _ := json.Marshal(fieldMap(s.bodyFields("body", []byte(invalid))))
		if strings.Contains(string(encoded), private) || strings.Contains(string(encoded), "single-private-word") || strings.Contains(string(encoded), "/private/file") {
			t.Fatal("畸形标量不能绕过脱敏", string(encoded))
		}
	}
}

func TestBodyLogLimits(t *testing.T) {
	s := &Server{cfg: &config.Config{}}
	for _, input := range []any{
		map[string]any{"items": []any{1, 2, 3, 4}},
		map[string]any{"cursor": strings.Repeat("a", 300)},
		map[string]any{"data": map[string]any{"data": map[string]any{"data": map[string]any{"data": map[string]any{"data": map[string]any{"data": map[string]any{"data": map[string]any{"id": "deep"}}}}}}}},
	} {
		raw, _ := json.Marshal(input)
		fields := fieldMap(s.bodyFields("body", raw))
		if fields["body_truncated"] != true {
			t.Fatal("数组、长字符串和深层结构必须有界", fields)
		}
	}
	input := map[string]any{"sequence": json.Number(strings.Repeat("1", 5000))}
	raw, _ := json.Marshal(input)
	fields := fieldMap(s.bodyFields("body", raw))
	encoded, _ := json.Marshal(fields["body"])
	if len(encoded) > maxLogBodyBytes || !utf8.Valid(encoded) || fields["body_truncated"] != true {
		t.Fatal("body 预览必须保留合法 UTF-8 并限制字节", len(encoded))
	}
	if clipUTF8("中a", 2) != "" || clipUTF8("ok", 20) != "ok" {
		t.Fatal("UTF-8 截断错误")
	}
	if fieldMap(s.logValueFields("body", make(chan int)))["body"] != "[无法编码]" {
		t.Fatal("不可编码数据必须明确标记")
	}
}

func TestReadableErrorDiagnostics(t *testing.T) {
	for _, code := range []domain.Code{domain.CodeNotFound, domain.CodePermissionDenied, domain.CodeUnauthenticated, domain.CodeDependencyUnavailable, domain.CodeInvalidArgument, domain.CodeConflict, domain.CodeResourceExhausted, domain.CodeInternal} {
		fields := fieldMap(errorFields(domain.NewError(code, "明确原因"), "gateway"))
		if fields["error_message"] != "明确原因" || fields["error_hint"] == "" {
			t.Fatal("每种错误都需要文案和提示", fields)
		}
	}
	for _, message := range []string{"会话不存在", "会话不属于当前 Desktop", "消息不存在", "查询失败", "thread not found: /private/file", "permission denied: /private/file", "EACCES /private/file", "timeout /private/file", "读取超时 /private/file", "private words"} {
		fields := fieldMap(errorFields(domain.ErrNotFound(message), "desktop"))
		if strings.Contains(fmt.Sprint(fields), "/private/file") || strings.Contains(fmt.Sprint(fields), "private words") {
			t.Fatal("不可信异常细节应脱敏", fields)
		}
	}
	if message, _ := diagnosticMessage(domain.ErrInternal(""), "gateway"); message == "" {
		t.Fatal("空文案需要兜底")
	}
	if payloadReferences([]byte(`{`)) != nil || len(payloadReferences([]byte(`{"agent_session_id":"s","message_id":"m","request_id":"r"}`))) != 6 {
		t.Fatal("关联字段提取错误")
	}
	if operationName("unknown") != "处理消息" || traceID(1, "a") == traceID(2, "a") {
		t.Fatal("未知操作或跨连接追踪错误")
	}
}
