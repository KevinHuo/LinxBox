// Command linxbox-gateway 是常驻 Gateway 服务入口。
package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/app"
	"github.com/KevinHuo/LinxBox/gateway/internal/config"
)

func main() {
	os.Exit(run(os.Args[1:]))
}

// run 解析参数并启动服务；使用独立 FlagSet 避免污染测试进程的全局 flag。
func run(args []string) int {
	fs := flag.NewFlagSet("linxbox-gateway", flag.ContinueOnError)
	configPath := fs.String("config", "", "配置文件路径（必填）")
	dynamicConfig := fs.String("write-dynamic-mcp-config", "", "将 MCP 固定设备范围转换为已登记账号，写入新的配置文件后退出（不覆盖原文件）")
	healthURL := fs.String("healthcheck", "", "检查 HTTP 就绪地址后退出")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *healthURL != "" {
		client := &http.Client{Timeout: 3 * time.Second}
		response, err := client.Get(*healthURL)
		if err != nil {
			return 1
		}
		defer response.Body.Close()
		if response.StatusCode != http.StatusOK {
			return 1
		}
		return 0
	}
	if *configPath == "" {
		fs.Usage()
		return 2
	}
	if *dynamicConfig != "" {
		if err := writeDynamicMCPConfig(context.Background(), *configPath, *dynamicConfig); err != nil {
			fmt.Fprintf(os.Stderr, "MCP 配置转换失败: %v\n", err)
			return 1
		}
		fmt.Fprintln(os.Stdout, "已输出账号范围的 MCP 配置；原配置未修改，凭据未输出。应用新配置并重启后生效。")
		return 0
	}
	cfg, err := config.Load(*configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "配置加载失败: %v\n", err)
		return 1
	}
	a, err := app.New(cfg)
	if err != nil {
		fmt.Fprintf(os.Stderr, "初始化失败: %v\n", err)
		return 1
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	logger := a.Logger()
	logger.Info("linxbox-gateway 启动", "addr", cfg.Listen.Addr)
	if err := a.Run(ctx); err != nil {
		logger.Error("服务异常退出", "err", err)
		_ = a.Close()
		return 1
	}
	if err := a.Close(); err != nil {
		logger.Error("关闭数据库失败", "err", err)
		return 1
	}
	logger.Info("linxbox-gateway 已退出")
	return 0
}
