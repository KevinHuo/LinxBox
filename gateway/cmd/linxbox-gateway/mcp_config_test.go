package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

func mcpUpgradeFixture(t *testing.T) (string, map[string]any) {
	t.Helper()
	dir := t.TempDir()
	database := filepath.Join(dir, "gateway.db")
	db, err := storage.Open(database)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	if _, err := (storage.DeviceRepo{}).Register(t.Context(), db.DB(), "AA:BB:CC:DD:EE:01", "alice", config.DeviceCredentialKey("device-token")); err != nil {
		t.Fatal(err)
	}
	return filepath.Join(dir, "config.json"), map[string]any{
		"listen":    map[string]any{"addr": "127.0.0.1:8787"},
		"database":  map[string]any{"path": database},
		"extension": map[string]any{"preserve": true},
		"credentials": map[string]any{
			"devices":  []any{map[string]any{"token": "device-token", "subject": "alice"}},
			"desktops": []any{map[string]any{"token": "desktop-token", "client_id": "desktop", "subject": "alice"}},
			"mcp": []any{
				map[string]any{"token": "mcp-token", "device_id": "AA:BB:CC:DD:EE:01", "agent_id": "agent", "extension": true},
				map[string]any{"token": "existing-subject-token", "subject": "alice", "agent_id": "agent"},
			},
		},
	}
}

func writeUpgradeFixture(t *testing.T, path string, document map[string]any) []byte {
	t.Helper()
	raw, err := json.Marshal(document)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o640); err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestWriteDynamicMCPConfigPreservesConfig(t *testing.T) {
	input, document := mcpUpgradeFixture(t)
	original := writeUpgradeFixture(t, input, document)
	output := input + ".new"
	if code := run([]string{"--config", input, "--write-dynamic-mcp-config", output}); code != 0 {
		t.Fatal("配置转换失败", code)
	}
	got, err := config.Load(output)
	if err != nil {
		t.Fatal(err)
	}
	if m := got.Credentials.MCP[0]; m.Subject != "alice" || m.DeviceID != "" || m.Token != "mcp-token" || m.AgentID != "agent" {
		t.Fatal("转换未保持身份或凭据")
	}
	raw, err := os.ReadFile(input)
	if err != nil || string(raw) != string(original) {
		t.Fatal("原配置被修改", err)
	}
	info, err := os.Stat(output)
	if err != nil || info.Mode().Perm() != 0o600 {
		t.Fatal("新配置权限错误", err)
	}
	var converted map[string]any
	raw, err = os.ReadFile(output)
	if err != nil || json.Unmarshal(raw, &converted) != nil {
		t.Fatal("输出不是有效配置", err)
	}
	if converted["extension"].(map[string]any)["preserve"] != true || converted["credentials"].(map[string]any)["mcp"].([]any)[0].(map[string]any)["extension"] != true {
		t.Fatal("未知配置字段丢失")
	}
	// 已转换配置不依赖旧设备数据库，且输出存在时不能覆盖。
	converted["database"].(map[string]any)["path"] = filepath.Join(t.TempDir(), "missing.db")
	writeUpgradeFixture(t, output, converted)
	if err := writeDynamicMCPConfig(t.Context(), output, output+".again"); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(output)
	if code := run([]string{"--config", input, "--write-dynamic-mcp-config", output}); code != 1 {
		t.Fatal("不能覆盖已有输出", code)
	}
	after, _ := os.ReadFile(output)
	if string(before) != string(after) {
		t.Fatal("已有输出被覆盖")
	}
}

func TestWriteDynamicMCPConfigErrors(t *testing.T) {
	for _, scenario := range []string{"输入缺失", "JSON非法", "无MCP", "设备未登记", "数据库缺失", "凭据撤销", "账号不符", "账号过长", "对象键不兼容", "数组键不兼容", "输出目录缺失"} {
		t.Run(scenario, func(t *testing.T) {
			input, document := mcpUpgradeFixture(t)
			credentials := document["credentials"].(map[string]any)
			mcp := credentials["mcp"].([]any)[0].(map[string]any)
			output := input + ".new"
			switch scenario {
			case "无MCP":
				delete(credentials, "mcp")
			case "设备未登记":
				mcp["device_id"] = "AA:BB:CC:DD:EE:09"
			case "数据库缺失":
				document["database"].(map[string]any)["path"] = filepath.Join(t.TempDir(), "missing.db")
			case "凭据撤销":
				credentials["devices"].([]any)[0].(map[string]any)["token"] = "new-token"
			case "账号不符":
				credentials["devices"].([]any)[0].(map[string]any)["subject"] = "bob"
			case "账号过长":
				subject := strings.Repeat("a", 129)
				credentials["devices"].([]any)[0].(map[string]any)["subject"] = subject
				db, err := storage.Open(document["database"].(map[string]any)["path"].(string))
				if err != nil {
					t.Fatal(err)
				}
				_, err = db.DB().Exec(`UPDATE device_registrations SET subject=?`, subject)
				_ = db.Close()
				if err != nil {
					t.Fatal(err)
				}
			case "对象键不兼容":
				document["Credentials"] = credentials
				delete(document, "credentials")
			case "数组键不兼容":
				credentials["MCP"] = credentials["mcp"]
				delete(credentials, "mcp")
			case "输出目录缺失":
				output = filepath.Join(t.TempDir(), "missing", "config.json")
			}
			original := writeUpgradeFixture(t, input, document)
			if scenario == "输入缺失" {
				input += ".missing"
			} else if scenario == "JSON非法" {
				original = []byte("{")
				if err := os.WriteFile(input, original, 0o640); err != nil {
					t.Fatal(err)
				}
			}
			if err := writeDynamicMCPConfig(t.Context(), input, output); err == nil {
				t.Fatal("错误配置应拒绝")
			}
			if _, err := os.Stat(output); !os.IsNotExist(err) {
				t.Fatal("失败不应留下输出文件", err)
			}
			if scenario != "输入缺失" {
				raw, err := os.ReadFile(input)
				if err != nil || string(raw) != string(original) {
					t.Fatal("失败改变了原配置", err)
				}
			}
		})
	}
}

func TestWriteDynamicMCPConfigKeepsAccountsSeparate(t *testing.T) {
	input, document := mcpUpgradeFixture(t)
	credentials := document["credentials"].(map[string]any)
	credentials["devices"] = append(credentials["devices"].([]any), map[string]any{"token": "bob-device-token", "subject": "bob"})
	credentials["mcp"] = append(credentials["mcp"].([]any), map[string]any{"token": "bob-mcp-token", "device_id": "AA:BB:CC:DD:EE:02", "agent_id": "bob-agent"})
	db, err := storage.Open(document["database"].(map[string]any)["path"].(string))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := (storage.DeviceRepo{}).Register(t.Context(), db.DB(), "AA:BB:CC:DD:EE:02", "bob", config.DeviceCredentialKey("bob-device-token")); err != nil {
		t.Fatal(err)
	}
	writeUpgradeFixture(t, input, document)
	output := input + ".new"
	if err := writeDynamicMCPConfig(t.Context(), input, output); err != nil {
		t.Fatal(err)
	}
	cfg, err := config.Load(output)
	if err != nil || cfg.Credentials.MCP[0].Subject != "alice" || cfg.Credentials.MCP[2].Subject != "bob" {
		t.Fatal("转换不能混合账号", err)
	}
}

func TestWriteDynamicMCPConfigRejectsConflictingKeys(t *testing.T) {
	input, document := mcpUpgradeFixture(t)
	raw := writeUpgradeFixture(t, input, document)
	// encoding/json 接受大小写别名；重排对象键后不得改变最终有效凭据。
	raw = append(raw[:len(raw)-1], []byte(`,"Credentials":{"mcp":[{"token":"alias-one","subject":"bob","agent_id":"agent"},{"token":"alias-two","subject":"bob","agent_id":"agent"}]}}`)...)
	if err := os.WriteFile(input, raw, 0o640); err != nil {
		t.Fatal(err)
	}
	if err := writeDynamicMCPConfig(t.Context(), input, input+".new"); err == nil {
		t.Fatal("转换不能改变实际生效的凭据或账号")
	}
	if _, err := os.Stat(input + ".new"); !os.IsNotExist(err) {
		t.Fatal("冲突配置不应产生输出", err)
	}
}
