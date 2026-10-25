package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"net/url"
	"os"
	"path/filepath"
	"slices"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

// writeDynamicMCPConfig 只读旧设备登记，显式将 token 授权范围改为对应账号。
// 输出文件必须不存在；原配置、数据库及未知配置字段保持不变。
func writeDynamicMCPConfig(ctx context.Context, input, output string) error {
	raw, err := os.ReadFile(input)
	if err != nil {
		return err
	}
	cfg, err := config.Parse(raw)
	if err != nil {
		return err
	}
	if len(cfg.Credentials.MCP) == 0 {
		return fmt.Errorf("配置中没有独立 MCP 凭据")
	}
	var document map[string]json.RawMessage
	if err := json.Unmarshal(raw, &document); err != nil {
		return fmt.Errorf("配置 JSON 无效")
	}
	var credentials map[string]json.RawMessage
	if err := json.Unmarshal(document["credentials"], &credentials); err != nil {
		return fmt.Errorf("缺少 credentials 对象")
	}
	var entries []map[string]json.RawMessage
	if err := json.Unmarshal(credentials["mcp"], &entries); err != nil || len(entries) != len(cfg.Credentials.MCP) {
		return fmt.Errorf("MCP 配置与已校验快照不一致")
	}
	updated := *cfg
	updated.Credentials.MCP = append([]config.MCPCredential(nil), cfg.Credentials.MCP...)
	var db *sql.DB
	defer func() {
		if db != nil {
			_ = db.Close()
		}
	}()
	for i, credential := range updated.Credentials.MCP {
		if credential.Subject == "" {
			if db == nil {
				path, err := filepath.Abs(cfg.Database.Path)
				if err != nil {
					return err
				}
				dsn := (&url.URL{Scheme: "file", Path: path, RawQuery: "mode=ro"}).String()
				db, err = sql.Open("sqlite", dsn)
				if err != nil {
					return err
				}
			}
			device, err := (storage.DeviceRepo{}).Get(ctx, db, credential.DeviceID)
			if err != nil {
				return fmt.Errorf("第 %d 项 MCP 对应设备没有可读取的登记，无法确定账号", i+1)
			}
			subject, valid := cfg.DeviceSubjectForCredential(device.CredentialKey)
			if !valid || string(subject) != device.Subject {
				return fmt.Errorf("第 %d 项 MCP 对应设备的账号授权已失效", i+1)
			}
			credential.Subject, credential.DeviceID = string(subject), ""
			updated.Credentials.MCP[i] = credential
		}
		entries[i]["subject"], _ = json.Marshal(credential.Subject)
		delete(entries[i], "device_id")
	}
	if err := updated.Validate(); err != nil {
		return err
	}
	credentials["mcp"], _ = json.Marshal(entries)
	document["credentials"], _ = json.Marshal(credentials)
	raw, err = json.MarshalIndent(document, "", "  ")
	if err != nil {
		return err
	}
	converted, err := config.Parse(raw)
	if err != nil || !slices.Equal(converted.Credentials.MCP, updated.Credentials.MCP) {
		return fmt.Errorf("转换后配置的 MCP 范围不一致，请检查重复或大小写混用的配置键")
	}
	file, err := os.OpenFile(output, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	complete := false
	defer func() {
		_ = file.Close()
		if !complete {
			_ = os.Remove(output)
		}
	}()
	if _, err := file.Write(append(raw, '\n')); err != nil {
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	complete = true
	return nil
}
