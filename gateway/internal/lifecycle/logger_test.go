package lifecycle

import (
	"bytes"
	"log/slog"
	"testing"
)

func TestNewLoggerLevels(t *testing.T) {
	var buf bytes.Buffer
	logger, err := NewLogger("info", &buf)
	if err != nil {
		t.Fatal(err)
	}
	logger.Debug("debug-msg")
	logger.Info("info-msg")
	out := buf.String()
	if bytes.Contains(buf.Bytes(), []byte("debug-msg")) {
		t.Fatal("info 级别不应输出 debug")
	}
	if !bytes.Contains(buf.Bytes(), []byte("info-msg")) {
		t.Fatal("info 级别应输出 info")
	}
	if !bytes.Contains(buf.Bytes(), []byte(`"msg"`)) {
		t.Fatalf("JSON 日志结构错误: %s", out)
	}
	// 无效级别。
	if _, err := NewLogger("verbose", nil); err == nil {
		t.Fatal("无效级别应报错")
	}
	// nil 输出回退到 stderr 不报错。
	if _, err := NewLogger("warn", nil); err != nil {
		t.Fatal(err)
	}
	// 结构化字段。
	_ = slog.New(slog.NewTextHandler(&buf, nil))
}

func TestReadableLoggerFormats(t *testing.T) {
	for _, format := range []string{"", "text", "json"} {
		var output bytes.Buffer
		logger, err := NewLoggerWithFormat("info", format, &output)
		if err != nil {
			t.Fatal(err)
		}
		logger.Info("Desktop → Gateway：读取会话历史失败：会话不存在", "error_message", "会话不存在", "body", map[string]any{"agent_session_id": "sess-1", "data": map[string]any{}}, "response_body", map[string]any{"error": map[string]any{"message": "会话不存在"}})
		if !bytes.Contains(output.Bytes(), []byte("会话不存在")) || !bytes.Contains(output.Bytes(), []byte("sess-1")) {
			t.Fatal("可读文案和 body 不能丢失", output.String())
		}
		if format == "json" && !bytes.Contains(output.Bytes(), []byte(`"msg"`)) {
			t.Fatal("JSON 格式必须保留", output.String())
		}
		if format != "json" && !bytes.Contains(output.Bytes(), []byte("level=INFO")) {
			t.Fatal("默认使用文本格式", output.String())
		}
		if format != "json" && (!bytes.Contains(output.Bytes(), []byte("body.agent_session_id=sess-1")) || !bytes.Contains(output.Bytes(), []byte("response_body.error.message=会话不存在"))) {
			t.Fatal("文本 body 应展开易读字段", output.String())
		}
	}
	if _, err := NewLoggerWithFormat("info", "invalid", nil); err == nil {
		t.Fatal("未知日志格式应拒绝")
	}
}
