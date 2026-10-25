package config

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestIntegrationConfigLimits(t *testing.T) {
	source, err := os.ReadFile("../../config.example.json")
	if err != nil {
		t.Fatal(err)
	}
	var cfg map[string]any
	json.Unmarshal(source, &cfg)
	limits := cfg["limits"].(map[string]any)
	limits["max_frame_bytes"] = 65537
	data, _ := json.Marshal(cfg)
	path := filepath.Join(t.TempDir(), "cfg.json")
	os.WriteFile(path, data, 0600)
	if _, err := Load(path); err == nil {
		t.Fatal("不能通过配置放宽契约帧上限")
	}
	limits["max_frame_bytes"] = 65536
	cfg["timeouts"].(map[string]any)["query_timeout"] = "-1s"
	data, _ = json.Marshal(cfg)
	os.WriteFile(path, data, 0600)
	if _, err := Load(path); err == nil {
		t.Fatal("负超时应拒绝")
	}
}
