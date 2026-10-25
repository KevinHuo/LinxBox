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
