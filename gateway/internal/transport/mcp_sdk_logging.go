package transport

import (
	"context"
	"fmt"
	"log/slog"
)

// SDK 会输出真实 session_id，部分错误还会直接拼入消息；仅转发固定诊断事件和安全字段。
type mcpSDKLogHandler struct{ slog.Handler }

func (h mcpSDKLogHandler) Handle(ctx context.Context, record slog.Record) error {
	message, known := map[string]string{
		"server connecting":                    "MCP SDK 正在建立传输会话",
		"server session connected":             "MCP SDK 传输会话已建立",
		"server session disconnected":          "MCP SDK 传输会话已结束",
		"session initialized":                  "MCP SDK 初始化完成",
		"initialized before initialize":        "MCP 初始化顺序错误：先 initialize 再发送 initialized",
		"duplicate initialized notification":   "MCP 收到重复 initialized 通知",
		"method invalid during initialization": "MCP 初始化尚未完成，当前方法不可调用",
	}[record.Message]
	if !known {
		message = "MCP SDK 传输事件（详情已脱敏）"
	}
	level := record.Level
	if level < slog.LevelWarn {
		level = slog.LevelDebug
	}
	clean := slog.NewRecord(record.Time, level, message, record.PC)
	clean.AddAttrs(slog.String("event", "mcp.sdk"), slog.Bool("error_detail_redacted", !known))
	record.Attrs(func(attr slog.Attr) bool {
		clean.AddAttrs(safeMCPsdkAttrs([]slog.Attr{attr})...)
		return true
	})
	if !h.Handler.Enabled(ctx, level) {
		return nil
	}
	return h.Handler.Handle(ctx, clean)
}

func (h mcpSDKLogHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	return mcpSDKLogHandler{Handler: h.Handler.WithAttrs(safeMCPsdkAttrs(attrs))}
}

func (h mcpSDKLogHandler) WithGroup(name string) slog.Handler {
	return mcpSDKLogHandler{Handler: h.Handler.WithGroup(name)}
}

func safeMCPsdkAttrs(attrs []slog.Attr) []slog.Attr {
	var safe []slog.Attr
	for _, attr := range attrs {
		switch attr.Key {
		case "session_id":
			if attr.Value.Kind() == slog.KindString {
				safe = append(safe, slog.Uint64("mcp_connection_id", mcpSessionLogID(attr.Value.String())))
			}
		case "method":
			if attr.Value.Kind() == slog.KindString {
				safe = append(safe, slog.String("rpc_method", safeMCPRPCMethod(attr.Value.String())))
			}
		case "error":
			safe = append(safe, slog.String("error_type", fmt.Sprintf("%T", attr.Value.Any())))
		}
	}
	return safe
}
