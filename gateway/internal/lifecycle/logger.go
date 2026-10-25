// Package lifecycle 管理在线连接表、启动恢复、关闭与日志。
package lifecycle

import (
	"fmt"
	"io"
	"log/slog"
	"os"
	"sort"
)

// NewLogger 创建结构化 JSON 日志器。日志只记录关联 ID、操作、状态、时延和
// 脱敏错误；不记录 token、原始 HTTP 头、ASR 全文、私人历史或本机路径。
func NewLogger(level string, output io.Writer) (*slog.Logger, error) {
	return NewLoggerWithFormat(level, "json", output)
}

// NewLoggerWithFormat 默认使用便于 journalctl 阅读的文本；JSON 保留用于结构化采集。
func NewLoggerWithFormat(level, format string, output io.Writer) (*slog.Logger, error) {
	var lvl slog.Level
	if err := lvl.UnmarshalText([]byte(level)); err != nil {
		return nil, fmt.Errorf("日志级别 %q 不合法", level)
	}
	if output == nil {
		output = os.Stderr
	}
	options := &slog.HandlerOptions{Level: lvl}
	var handler slog.Handler
	switch format {
	case "", "text":
		options.ReplaceAttr = func(groups []string, attr slog.Attr) slog.Attr {
			if attr.Key == "body" || attr.Key == "response_body" {
				return readableBody(attr)
			}
			return attr
		}
		handler = slog.NewTextHandler(output, options)
	case "json":
		handler = slog.NewJSONHandler(output, options)
	default:
		return nil, fmt.Errorf("日志格式 %q 不合法，只能为 text 或 json", format)
	}
	return slog.New(handler), nil
}

// 文本格式把 body 对象展开为 body.agent_session_id 等键，避免 JSON 二次转义。
// JSON 日志保持真正的嵌套对象，数组和空对象也保留明确表示。
func readableBody(attr slog.Attr) slog.Attr {
	value, ok := attr.Value.Any().(map[string]any)
	if !ok {
		return attr
	}
	if len(value) == 0 {
		return slog.String(attr.Key, "{}")
	}
	keys := make([]string, 0, len(value))
	for key := range value {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	fields := make([]slog.Attr, 0, len(keys))
	for _, key := range keys {
		fields = append(fields, readableBody(slog.Any(key, value[key])))
	}
	return slog.Attr{Key: attr.Key, Value: slog.GroupValue(fields...)}
}
