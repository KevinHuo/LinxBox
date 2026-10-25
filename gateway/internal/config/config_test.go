package config

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func validConfig() Config {
	return Config{
		Listen:   ListenConfig{Addr: "127.0.0.1:8787"},
		Database: DatabaseConfig{Path: "/tmp/gateway-test.db"},
		Credentials: CredentialsConfig{
			Devices:  []DeviceCredential{{Token: "dev-token", DeviceID: "AA:BB:CC:DD:EE:01", Subject: "alice"}},
			Desktops: []DesktopCredential{{Token: "dt-token", ClientID: "desktop-1", Subject: "alice"}},
		},
		Pairing: PairingConfig{Rules: []PairingRule{{Subject: "alice", DeviceIDs: []string{"aa:bb:cc:dd:ee:01"}}}},
	}
}

// writeConfig 将配置写为临时文件。
func writeConfig(t *testing.T, cfg Config) string {
	t.Helper()
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

func TestLoadValid(t *testing.T) {
	cfg := validConfig()
	cfg.Database.Path = filepath.Join(t.TempDir(), "gw.db")
	got, err := Load(writeConfig(t, cfg))
	if err != nil {
		t.Fatalf("Load 失败: %v", err)
	}
	// 默认值补齐。
	if got.Limits.MaxFrameBytes != 64*1024 {
		t.Fatalf("MaxFrameBytes 默认值错误: %d", got.Limits.MaxFrameBytes)
	}
	if got.Limits.SessionPageSize != 5 {
		t.Fatalf("SessionPageSize 默认值错误: %d", got.Limits.SessionPageSize)
	}
	if got.Limits.SendTimeout.Std() != 5*time.Second {
		t.Fatalf("SendTimeout 默认值错误: %v", got.Limits.SendTimeout.Std())
	}
	if got.Timeouts.ShutdownGrace.Std() != 10*time.Second {
		t.Fatalf("ShutdownGrace 默认值错误: %v", got.Timeouts.ShutdownGrace.Std())
	}
	if got.Log.Level != "info" {
		t.Fatalf("日志级别默认值错误: %s", got.Log.Level)
	}
	// MAC 规范化。
	if got.Credentials.Devices[0].DeviceID != "AA:BB:CC:DD:EE:01" {
		t.Fatalf("设备 MAC 未规范化: %s", got.Credentials.Devices[0].DeviceID)
	}
	if got.Pairing.Rules[0].DeviceIDs[0] != "AA:BB:CC:DD:EE:01" {
		t.Fatalf("配对规则 MAC 未规范化: %s", got.Pairing.Rules[0].DeviceIDs[0])
	}
	if _, ok := got.DeviceByToken("dev-token"); !ok {
		t.Fatal("DeviceByToken 应命中")
	}
	if _, ok := got.DeviceByToken("wrong"); ok {
		t.Fatal("DeviceByToken 不应命中错误 token")
	}
	if _, ok := got.DesktopByToken("dt-token"); !ok {
		t.Fatal("DesktopByToken 应命中")
	}
	if subject, ok := got.DeviceSubjectForCredential(DeviceCredentialKey("dev-token")); !ok || subject != "alice" {
		t.Fatal("设备接入账号缺失")
	}
}

func TestLoadErrors(t *testing.T) {
	tests := []struct {
		name   string
		mutate func(*Config)
		want   string
	}{
		{"缺少监听地址", func(c *Config) { c.Listen.Addr = "" }, "listen.addr"},
		{"监听地址非法", func(c *Config) { c.Listen.Addr = "no-port" }, "listen.addr"},
		{"缺少数据库路径", func(c *Config) { c.Database.Path = "" }, "database.path"},
		{"缺少设备凭据", func(c *Config) { c.Credentials.Devices = nil }, "devices"},
		{"缺少 Desktop 凭据", func(c *Config) { c.Credentials.Desktops = nil }, "desktops"},
		{"设备 token 为空", func(c *Config) { c.Credentials.Devices[0].Token = "" }, "token"},
		{"设备 MAC 非法", func(c *Config) { c.Credentials.Devices[0].DeviceID = "bad" }, "device_id"},
		{"设备主体为空", func(c *Config) { c.Credentials.Devices[0].Subject = "" }, "subject"},
		{"Desktop client_id 非法", func(c *Config) { c.Credentials.Desktops[0].ClientID = "" }, "client_id"},
		{"Desktop 主体为空", func(c *Config) { c.Credentials.Desktops[0].Subject = "" }, "subject"},
		{"token 重复", func(c *Config) { c.Credentials.Desktops[0].Token = "dev-token" }, "token 重复"},
		{"配对规则主体为空", func(c *Config) { c.Pairing.Rules[0].Subject = "" }, "subject"},
		{"配对规则设备为空", func(c *Config) { c.Pairing.Rules[0].DeviceIDs = nil }, "device_ids"},
		{"配对规则 MAC 非法", func(c *Config) { c.Pairing.Rules[0].DeviceIDs = []string{"bad"} }, "device_id"},
		{"TLS 只配一个", func(c *Config) { c.Listen.TLSCert = "/x/cert.pem" }, "tls_cert_file"},
		{"分页超限", func(c *Config) { c.Limits.SessionPageSize = 6 }, "session_page_size"},
		{"最大帧过小", func(c *Config) { c.Limits.MaxFrameBytes = 100 }, "max_frame_bytes"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			cfg := validConfig()
			cfg.Database.Path = filepath.Join(t.TempDir(), "gw.db")
			tt.mutate(&cfg)
			_, err := Load(writeConfig(t, cfg))
			if err == nil {
				t.Fatal("期望校验失败")
			}
			if !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("错误 %q 应包含 %q", err, tt.want)
			}
		})
	}
}

func TestLoadMissingFile(t *testing.T) {
	if _, err := Load(filepath.Join(t.TempDir(), "nope.json")); err == nil {
		t.Fatal("缺失文件应报错")
	}
}

func TestLoadBadJSON(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bad.json")
	if err := os.WriteFile(path, []byte("{not json"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err == nil {
		t.Fatal("非法 JSON 应报错")
	}
}

func TestDurationUnmarshal(t *testing.T) {
	var d Duration
	if err := json.Unmarshal([]byte(`"3s"`), &d); err != nil {
		t.Fatal(err)
	}
	if d.Std() != 3*time.Second {
		t.Fatalf("时长解析错误: %v", d.Std())
	}
	if err := json.Unmarshal([]byte(`5`), &d); err != nil {
		t.Fatal(err)
	}
	if d.Std() != 5*time.Second {
		t.Fatalf("秒数解析错误: %v", d.Std())
	}
	if err := json.Unmarshal([]byte(`"abc"`), &d); err == nil {
		t.Fatal("非法时长应报错")
	}
	if err := json.Unmarshal([]byte(`true`), &d); err == nil {
		t.Fatal("非法类型应报错")
	}
}

func TestDurationUnmarshalInvalidJSON(t *testing.T) {
	var d Duration
	if err := json.Unmarshal([]byte(`{`), &d); err == nil {
		t.Fatal("非法 JSON 应报错")
	}
}

func TestValidateMaxCatalogItemsTooSmall(t *testing.T) {
	c := validConfig()
	c.Pairing.Rules = []PairingRule{{Subject: "alice", DeviceIDs: []string{"AA:BB:CC:DD:EE:01"}}}
	c.Limits = LimitsConfig{MaxFrameBytes: 64 * 1024, SessionPageSize: 5, MaxCatalogItems: 4}
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "max_catalog_items") {
		t.Fatalf("应拒绝 max_catalog_items 过小: %v", err)
	}
}

func TestLookupMisses(t *testing.T) {
	c := validConfig()
	if _, ok := c.DesktopByToken("missing"); ok {
		t.Fatal("未知 token 不应命中")
	}
	if _, ok := c.DeviceSubjectForCredential(DeviceCredentialKey("missing")); ok {
		t.Fatal("未知设备不应命中")
	}
}

func TestLoadWithoutMACOrPairing(t *testing.T) {
	c := validConfig()
	c.Credentials.Devices[0].DeviceID = ""
	c.Pairing.Rules = nil
	got, err := Load(writeConfig(t, c))
	if err != nil || got.Credentials.Devices[0].DeviceID != "" || len(got.Pairing.Rules) != 0 {
		t.Fatal("新配置无需预登记 MAC 或配对白名单", got, err)
	}
}

func TestLogFormats(t *testing.T) {
	for _, format := range []string{"", "text", "json", "xml"} {
		cfg := validConfig()
		cfg.Log.Format = format
		got, err := Load(writeConfig(t, cfg))
		if format == "xml" {
			if err == nil {
				t.Fatal("未知格式应拒绝")
			}
			continue
		}
		if err != nil {
			t.Fatal(err)
		}
		want := format
		if want == "" {
			want = "text"
		}
		if got.Log.Format != want {
			t.Fatal("日志格式未应用", got.Log.Format)
		}
	}
}
