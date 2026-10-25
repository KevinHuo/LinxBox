package app

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
)

func writeValidConfig(t *testing.T) string {
	t.Helper()
	cfg := map[string]any{
		"listen":   map[string]any{"addr": "127.0.0.1:0"},
		"database": map[string]any{"path": filepath.Join(t.TempDir(), "gw.db")},
		"credentials": map[string]any{
			"devices": []map[string]any{
				{"token": "dev-token", "device_id": "AA:BB:CC:DD:EE:01", "subject": "alice"},
			},
			"desktops": []map[string]any{
				{"token": "dt-token", "client_id": "desktop-1", "subject": "alice"},
			},
		},
		"pairing": map[string]any{
			"rules": []map[string]any{{"subject": "alice", "device_ids": []string{"AA:BB:CC:DD:EE:01"}}},
		},
	}
	data, err := json.Marshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestNewAndRunShutdown(t *testing.T) {
	cfgPath := writeValidConfig(t)
	// 直接构造 App 需要 config.Config；通过 config.Load 走真实入口。
	cfg, err := config.Load(cfgPath)
	if err != nil {
		t.Fatal(err)
	}
	a, err := New(cfg)
	if err != nil {
		t.Fatalf("New 失败: %v", err)
	}
	defer a.Close()
	if a.Logger() == nil {
		t.Fatal("Logger 不应为空")
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- a.Run(ctx)
	}()
	time.Sleep(200 * time.Millisecond)
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run 应正常退出: %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Run 未在取消后退出")
	}
}

func TestNewBadConfig(t *testing.T) {
	if _, err := config.Load(filepath.Join(t.TempDir(), "missing.json")); err == nil {
		t.Fatal("缺失配置应报错")
	}
}

func validCfg(t *testing.T) *config.Config {
	t.Helper()
	return &config.Config{
		Listen:   config.ListenConfig{Addr: "127.0.0.1:0"},
		Log:      config.LogConfig{Level: "info"},
		Database: config.DatabaseConfig{Path: filepath.Join(t.TempDir(), "gw.db")},
		Credentials: config.CredentialsConfig{
			Devices:  []config.DeviceCredential{{Token: "dev-token", DeviceID: "AA:BB:CC:DD:EE:01", Subject: "alice"}},
			Desktops: []config.DesktopCredential{{Token: "dt-token", ClientID: "desktop-1", Subject: "alice"}},
		},
		Pairing: config.PairingConfig{Rules: []config.PairingRule{{Subject: "alice", DeviceIDs: []string{"AA:BB:CC:DD:EE:01"}}}},
	}
}

func TestNewBadLogLevel(t *testing.T) {
	cfg := validCfg(t)
	cfg.Log = config.LogConfig{Level: "bogus"}
	if _, err := New(cfg); err == nil {
		t.Fatal("非法日志级别应报错")
	}
}

func TestNewValidatorFailure(t *testing.T) {
	old := newValidatorFn
	newValidatorFn = func() (*contracts.Validator, error) { return nil, errors.New("boom") }
	defer func() { newValidatorFn = old }()
	if _, err := New(validCfg(t)); err == nil {
		t.Fatal("校验器初始化失败应报错")
	}
}

func TestNewStorageFailure(t *testing.T) {
	cfg := validCfg(t)
	cfg.Database.Path = filepath.Join(t.TempDir(), "no-such-dir", "gw.db")
	if _, err := New(cfg); err == nil {
		t.Fatal("数据库打开失败应报错")
	}
}
