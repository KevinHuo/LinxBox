// Package config 负责加载与校验网关配置；缺少必要凭据时启动失败。
// 配置仅引用凭据来源，不把密钥写入二进制或日志。
package config

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

// Duration 支持 "5s" 形式的 JSON 时长。
type Duration time.Duration

// UnmarshalJSON 接受 "5s" 或秒数。
func (d *Duration) UnmarshalJSON(b []byte) error {
	var v any
	if err := json.Unmarshal(b, &v); err != nil {
		return err
	}
	switch t := v.(type) {
	case string:
		dur, err := time.ParseDuration(t)
		if err != nil {
			return err
		}
		*d = Duration(dur)
	case float64:
		*d = Duration(time.Duration(t * float64(time.Second)))
	default:
		return fmt.Errorf("时长必须是字符串或秒数")
	}
	return nil
}

// Std 返回标准 time.Duration。
func (d Duration) Std() time.Duration { return time.Duration(d) }

// Config 是网关整体配置。
type Config struct {
	Listen      ListenConfig      `json:"listen"`
	Database    DatabaseConfig    `json:"database"`
	Credentials CredentialsConfig `json:"credentials"`
	Pairing     PairingConfig     `json:"pairing"`
	Limits      LimitsConfig      `json:"limits"`
	Timeouts    TimeoutConfig     `json:"timeouts"`
	Log         LogConfig         `json:"log"`
}

// ListenConfig 是监听配置。
type ListenConfig struct {
	Addr    string `json:"addr"`
	TLSCert string `json:"tls_cert_file,omitempty"`
	TLSKey  string `json:"tls_key_file,omitempty"`
}

// DatabaseConfig 是 SQLite 数据库配置。
type DatabaseConfig struct {
	Path string `json:"path"`
}

// CredentialsConfig 是预置凭据（受控部署配置；不保存明文 token 到数据库）。
type CredentialsConfig struct {
	Devices  []DeviceCredential  `json:"devices"`
	Desktops []DesktopCredential `json:"desktops"`
	MCP      []MCPCredential     `json:"mcp,omitempty"`
}

// MCPCredential 将独立的 MCP 凭据限制到一个设备和灵矽智能体。
type MCPCredential struct {
	Token    string `json:"token"`
	DeviceID string `json:"device_id"`
	AgentID  string `json:"agent_id"`
}

// DeviceCredential 认证设备接入账号；真实 MAC 由连接后的 device.hello 登记。
type DeviceCredential struct {
	Token    string `json:"token"`
	DeviceID string `json:"device_id,omitempty"` // 兼容旧配置，不限制本凭据可登记的 MAC。
	Subject  string `json:"subject"`
}

// DesktopCredential 是 Desktop 凭据。
type DesktopCredential struct {
	Token    string `json:"token"`
	ClientID string `json:"client_id"`
	Subject  string `json:"subject"`
}

// PairingConfig 仅保留旧配置解析兼容；绑定权限由动态登记账号决定。
type PairingConfig struct {
	Rules []PairingRule `json:"rules"`
}

// PairingRule 是旧版配对白名单，新版不再以其授权。
type PairingRule struct {
	Subject   string   `json:"subject"`
	DeviceIDs []string `json:"device_ids"`
}

// LimitsConfig 是资源上限。
type LimitsConfig struct {
	MaxFrameBytes        int      `json:"max_frame_bytes"`
	DraftMaxCodepoints   int      `json:"draft_max_codepoints"`
	DraftMaxUTF8Bytes    int      `json:"draft_max_utf8_bytes"`
	SessionPageSize      int      `json:"session_page_size"`
	MaxCatalogItems      int      `json:"max_catalog_items"`
	MaxOnlineConnections int      `json:"max_online_connections"`
	MaxPendingQueries    int      `json:"max_pending_queries"`
	MaxDeviceQueries     int      `json:"max_device_queries"`
	SendQueueSize        int      `json:"send_queue_size"`
	SendTimeout          Duration `json:"send_timeout"`
}

// TimeoutConfig 是业务超时。
type TimeoutConfig struct {
	ShutdownGrace Duration `json:"shutdown_grace"`
	QueryTimeout  Duration `json:"query_timeout"`
}

// LogConfig 是日志配置。
type LogConfig struct {
	Level string `json:"level"`
}

// Load 读取并校验配置文件。
func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("读取配置 %s: %w", path, err)
	}
	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("解析配置 %s: %w", path, err)
	}
	cfg.normalize()
	cfg.applyDefaults()
	if err := cfg.Validate(); err != nil {
		return nil, fmt.Errorf("配置校验失败: %w", err)
	}
	return &cfg, nil
}

// normalize 将 MAC 相关字段统一为规范化格式。
func (c *Config) normalize() {
	for i := range c.Credentials.MCP {
		if id, err := domain.NormalizeMAC(c.Credentials.MCP[i].DeviceID); err == nil {
			c.Credentials.MCP[i].DeviceID = string(id)
		}
	}
	for i := range c.Credentials.Devices {
		if id, err := domain.NormalizeMAC(c.Credentials.Devices[i].DeviceID); err == nil {
			c.Credentials.Devices[i].DeviceID = string(id)
		}
	}
	for i := range c.Pairing.Rules {
		for j := range c.Pairing.Rules[i].DeviceIDs {
			if id, err := domain.NormalizeMAC(c.Pairing.Rules[i].DeviceIDs[j]); err == nil {
				c.Pairing.Rules[i].DeviceIDs[j] = string(id)
			}
		}
	}
}

func (c *Config) applyDefaults() {
	d := c.Limits
	if d.MaxFrameBytes <= 0 {
		d.MaxFrameBytes = 64 * 1024
	}
	if d.DraftMaxCodepoints <= 0 {
		d.DraftMaxCodepoints = 16000
	}
	if d.DraftMaxUTF8Bytes <= 0 {
		d.DraftMaxUTF8Bytes = 16 * 1024
	}
	if d.SessionPageSize <= 0 {
		d.SessionPageSize = 5
	}
	if d.MaxCatalogItems <= 0 {
		d.MaxCatalogItems = 200
	}
	if d.MaxOnlineConnections <= 0 {
		d.MaxOnlineConnections = 128
	}
	if d.SendQueueSize <= 0 {
		d.SendQueueSize = 64
	}
	if d.MaxPendingQueries <= 0 {
		d.MaxPendingQueries = 128
	}
	if d.MaxDeviceQueries <= 0 {
		d.MaxDeviceQueries = 4
	}
	if d.SendTimeout == 0 {
		d.SendTimeout = Duration(5 * time.Second)
	}
	c.Limits = d

	t := c.Timeouts
	if t.ShutdownGrace == 0 {
		t.ShutdownGrace = Duration(10 * time.Second)
	}
	if t.QueryTimeout == 0 {
		t.QueryTimeout = Duration(15 * time.Second)
	}
	c.Timeouts = t

	if c.Log.Level == "" {
		c.Log.Level = "info"
	}
}

// Validate 检查配置合法性。
func (c *Config) Validate() error {
	if _, _, err := net.SplitHostPort(c.Listen.Addr); err != nil {
		return fmt.Errorf("listen.addr 必须为 host:port")
	}
	if c.Listen.TLSCert != "" || c.Listen.TLSKey != "" {
		if c.Listen.TLSCert == "" || c.Listen.TLSKey == "" {
			return fmt.Errorf("tls_cert_file 与 tls_key_file 必须同时提供")
		}
	}
	if c.Database.Path == "" {
		return fmt.Errorf("database.path 不能为空")
	}
	if len(c.Credentials.Devices) == 0 {
		return fmt.Errorf("credentials.devices 至少需要一个设备凭据")
	}
	if len(c.Credentials.Desktops) == 0 {
		return fmt.Errorf("credentials.desktops 至少需要一个 Desktop 凭据")
	}
	seenTokens := map[string]string{}
	checkToken := func(kind, token, owner string) error {
		if token == "" {
			return fmt.Errorf("%s 凭据 token 不能为空", kind)
		}
		if prev, ok := seenTokens[token]; ok {
			return fmt.Errorf("token 重复：%s 与 %s 使用同一 token", prev, owner)
		}
		seenTokens[token] = owner
		return nil
	}
	for _, d := range c.Credentials.Devices {
		if err := checkToken("设备", d.Token, "device:"+d.DeviceID); err != nil {
			return err
		}
		if d.DeviceID != "" && !domain.IsDeviceID(d.DeviceID) {
			return fmt.Errorf("设备凭据 device_id %q 不是合法 MAC", d.DeviceID)
		}
		if !domain.ValidateSubject(d.Subject) {
			return fmt.Errorf("设备凭据 subject 不能为空")
		}
	}
	for _, d := range c.Credentials.Desktops {
		if err := checkToken("Desktop", d.Token, "desktop:"+d.ClientID); err != nil {
			return err
		}
		if !domain.ValidateID(d.ClientID) {
			return fmt.Errorf("Desktop 凭据 client_id 必须为 1～128 个字符")
		}
		if !domain.ValidateSubject(d.Subject) {
			return fmt.Errorf("Desktop 凭据 subject 不能为空")
		}
	}
	for _, r := range c.Pairing.Rules {
		if !domain.ValidateSubject(r.Subject) {
			return fmt.Errorf("配对规则 subject 不能为空")
		}
		if len(r.DeviceIDs) == 0 {
			return fmt.Errorf("配对规则 %s 的 device_ids 不能为空", r.Subject)
		}
		for _, d := range r.DeviceIDs {
			if !domain.IsDeviceID(d) {
				return fmt.Errorf("配对规则 %s 的 device_id %q 不是合法 MAC", r.Subject, d)
			}
		}
	}
	for _, m := range c.Credentials.MCP {
		if err := checkToken("MCP", m.Token, "mcp:"+m.DeviceID); err != nil {
			return err
		}
		if _, err := domain.NormalizeMAC(m.DeviceID); err != nil {
			return fmt.Errorf("MCP device_id 必须为合法 MAC")
		}
		if !domain.ValidateID(m.AgentID) {
			return fmt.Errorf("MCP agent_id 必须为 1～128 个字符")
		}
	}
	if c.Limits.MaxFrameBytes < 1024 {
		return fmt.Errorf("max_frame_bytes 不能小于 1024")
	}
	if c.Limits.MaxFrameBytes > 64*1024 {
		return fmt.Errorf("max_frame_bytes 不能超过契约 64 KiB 上限")
	}
	if c.Limits.SendTimeout < 0 || c.Timeouts.QueryTimeout < 0 || c.Timeouts.ShutdownGrace < 0 {
		return fmt.Errorf("超时时间不能为负数")
	}
	if c.Limits.SessionPageSize <= 0 || c.Limits.SessionPageSize > 5 {
		return fmt.Errorf("session_page_size 必须在 1～5")
	}
	if c.Limits.MaxCatalogItems < c.Limits.SessionPageSize {
		return fmt.Errorf("max_catalog_items 不能小于 session_page_size")
	}
	return nil
}

// DeviceCredentialKey 关联当前接入凭据与持久设备登记；数据库不保存明文 token。
func DeviceCredentialKey(token string) string {
	key := sha256.Sum256([]byte(token))
	return hex.EncodeToString(key[:])
}

func (c *Config) DeviceSubjectForCredential(key string) (domain.Subject, bool) {
	for _, credential := range c.Credentials.Devices {
		if DeviceCredentialKey(credential.Token) == key {
			return domain.Subject(credential.Subject), true
		}
	}
	return "", false
}

// DeviceByToken 按 token 精确匹配设备凭据。
func (c *Config) DeviceByToken(token string) (DeviceCredential, bool) {
	for _, d := range c.Credentials.Devices {
		if d.Token == token {
			return d, true
		}
	}
	return DeviceCredential{}, false
}

// DesktopByToken 按 token 精确匹配 Desktop 凭据。
func (c *Config) DesktopByToken(token string) (DesktopCredential, bool) {
	for _, d := range c.Credentials.Desktops {
		if d.Token == token {
			return d, true
		}
	}
	return DesktopCredential{}, false
}

// DesktopSubject 返回当前 Desktop 凭据所属主体，用于复核持久化绑定。
func (c *Config) DesktopSubject(desktopID domain.DesktopClientID) (domain.Subject, bool) {
	for _, d := range c.Credentials.Desktops {
		if d.ClientID == string(desktopID) {
			return domain.Subject(d.Subject), true
		}
	}
	return "", false
}
