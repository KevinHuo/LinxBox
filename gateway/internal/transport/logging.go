package transport

import (
	"context"
	"errors"
	"log/slog"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
)

// logOperation 只记录关联字段和结果码；对端错误文案可能含正文，不能直接写入日志。
func (s *Server) logOperation(event string, conn *lifecycle.Connection, operation, requestID string, err error, attrs ...any) {
	level := slog.LevelInfo
	fields := []any{"event", event, "connection_id", conn.Generation(), "type", operation, "request_id", requestID, "ok", err == nil}
	if err != nil {
		level = slog.LevelWarn
		fields = append(fields, "error_code", domain.CodeOf(err))
	}
	s.logger.Log(context.Background(), level, "请求处理结果", append(fields, attrs...)...)
}

func websocketCloseCode(err error) int {
	var closeError *websocket.CloseError
	if errors.As(err, &closeError) {
		return closeError.Code
	}
	return 0
}
