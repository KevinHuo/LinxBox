// Package app 组装依赖，可被集成测试创建。
package app

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
	"github.com/KevinHuo/LinxBox/gateway/internal/transport"
)

// App 是组装完成的应用。
type App struct {
	cfg    *config.Config
	logger *slog.Logger
	db     *storage.DB
	server *transport.Server
}

// newValidatorFn 与 openDB 允许测试注入失败路径。
var (
	newValidatorFn = contracts.NewValidator
	openDB         = storage.Open
)

// New 依据配置组装应用：Schema 校验器、数据库迁移、连接表、服务与传输层。
func New(cfg *config.Config) (*App, error) {
	logger, err := lifecycle.NewLoggerWithFormat(cfg.Log.Level, cfg.Log.Format, nil)
	if err != nil {
		return nil, err
	}
	validator, err := newValidatorFn()
	if err != nil {
		return nil, fmt.Errorf("初始化契约校验器: %w", err)
	}
	db, err := openDB(cfg.Database.Path)
	if err != nil {
		return nil, fmt.Errorf("初始化数据库: %w", err)
	}
	repos := storage.NewRepositories(db.DB())
	for _, credential := range cfg.Credentials.Devices {
		if credential.DeviceID != "" {
			if err := repos.Devices.AdoptLegacyCredential(context.Background(), repos.DB(), credential.DeviceID, credential.Subject, config.DeviceCredentialKey(credential.Token)); err != nil {
				db.Close()
				return nil, fmt.Errorf("迁移旧设备凭据关联: %w", err)
			}
		}
	}
	conns := lifecycle.NewConnections(logger, cfg.Limits)

	server := transport.NewServer(cfg, logger, validator, transport.NewAuthenticator(cfg), conns)
	bindingSvc := service.NewBindingService(repos, cfg, server, logger)
	sessionSvc := service.NewSessionService(repos, cfg, server, logger)
	server.SetServices(bindingSvc, sessionSvc)
	if err := server.SetRequests(service.NewRequestService(repos, cfg, server)); err != nil {
		db.Close()
		return nil, fmt.Errorf("初始化 MCP: %w", err)
	}

	return &App{cfg: cfg, logger: logger, db: db, server: server}, nil
}

// Run 启动服务并阻塞到 ctx 取消。
func (a *App) Run(ctx context.Context) error {
	return a.server.Run(ctx)
}

// Close 关闭数据库。
func (a *App) Close() error {
	a.server.CloseMCP()
	return a.db.Close()
}

// Logger 返回应用日志器（供测试与工具使用）。
func (a *App) Logger() *slog.Logger { return a.logger }
