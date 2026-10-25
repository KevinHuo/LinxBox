package config

import (
	"encoding/json"
	"os"
	"testing"
)

func TestMCPImplicitConfiguration(t *testing.T) {
	for _, value := range []any{nil, false, true, "true"} {
		path := writeConfig(t, validConfig())
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		var document map[string]any
		if err = json.Unmarshal(raw, &document); err != nil {
			t.Fatal(err)
		}
		delete(document, "mcp")
		if value != nil {
			document["mcp"] = map[string]any{"allow_implicit_request_id": value}
		}
		raw, err = json.Marshal(document)
		if err != nil {
			t.Fatal(err)
		}
		if err = os.WriteFile(path, raw, 0600); err != nil {
			t.Fatal(err)
		}
		cfg, err := Load(path)
		if value == "true" {
			if err == nil {
				t.Fatal("字符串开关必须拒绝")
			}
			continue
		}
		if err != nil || cfg.MCP.AllowImplicitRequestID != (value == true) {
			t.Fatal(value, cfg, err)
		}
	}
}
