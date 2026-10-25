// Package lifecycle 管理在线连接表、启动恢复、关闭与日志。
package lifecycle

import (
	"fmt"
	"io"
	"log/slog"
	"os"
)

// NewLogger 创建结构化 JSON 日志器。日志只记录关联 ID、操作、状态、时延和
// 脱敏错误；不记录 token、原始 HTTP 头、ASR 全文、私人历史或本机路径。
func NewLogger(level string, output io.Writer) (*slog.Logger, error) {
	var lvl slog.Level
	if err := lvl.UnmarshalText([]byte(level)); err != nil {
		return nil, fmt.Errorf("日志级别 %q 不合法", level)
	}
	if output == nil {
		output = os.Stderr
	}
	handler := slog.NewJSONHandler(output, &slog.HandlerOptions{Level: lvl})
	return slog.New(handler), nil
}
