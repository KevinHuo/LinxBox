package transport

import (
	"context"
	"errors"
	"log/slog"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestMCPSDKLogsHideSessionAndUntrustedDetails(t *testing.T) {
	capture := &logCapture{}
	logger := slog.New(mcpSDKLogHandler{Handler: slog.NewJSONHandler(capture, &slog.HandlerOptions{Level: slog.LevelDebug})})
	logger.With("session_id", "private-session", "Authorization", "private-token").WithGroup("sdk").Warn("private-message", "error", errors.New("private-body"), "method", "private-method", "uri", "private-path")
	logger.Info("server session connected", "session_id", "private-session")
	logs := capture.snapshot()
	for _, value := range []string{"private-session", "private-token", "private-body", "private-method", "private-path", "private-message"} {
		if strings.Contains(logs, value) {
			t.Fatal("SDK 日志泄露", value)
		}
	}
	if !strings.Contains(logs, "mcp_connection_id") || !strings.Contains(logs, "error_type") {
		t.Fatal("缺少安全诊断字段", logs)
	}
	quiet := &logCapture{}
	slog.New(mcpSDKLogHandler{Handler: slog.NewJSONHandler(quiet, nil)}).Info("server session connected")
	if quiet.snapshot() != "" {
		t.Fatal("机械事件应留在 debug")
	}
	// 内部传输处理器也必须独立拒绝无效凭据，不能仅依靠外层路由。
	env, _ := streamableEnv(t, 1)
	r := httptest.NewRequest("POST", "/mcp", strings.NewReader(streamInitialize))
	r.Header.Set("Authorization", "Bearer invalid-token")
	w := httptest.NewRecorder()
	env.server.mcp.stream.ServeHTTP(w, r.WithContext(context.Background()))
	if w.Code != 401 {
		t.Fatal(w.Code)
	}
}
