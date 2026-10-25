package config

import "testing"

func TestMCPCredentials(t *testing.T) {
	for _, tc := range []struct {
		name       string
		credential MCPCredential
		valid      bool
	}{
		{"规范化", MCPCredential{Token: "mcp-token", DeviceID: "aabbccddee01", AgentID: "agent"}, true},
		{"空凭据", MCPCredential{DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"}, false},
		{"复用设备凭据", MCPCredential{Token: "dev-token", DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"}, false},
		{"未知设备", MCPCredential{Token: "mcp-token", DeviceID: "AA:BB:CC:DD:EE:02", AgentID: "agent"}, false},
		{"非法设备", MCPCredential{Token: "mcp-token", DeviceID: "invalid", AgentID: "agent"}, false},
		{"缺少智能体", MCPCredential{Token: "mcp-token", DeviceID: "AA:BB:CC:DD:EE:01"}, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := validConfig()
			cfg.Credentials.MCP = []MCPCredential{tc.credential}
			got, err := Load(writeConfig(t, cfg))
			if (err == nil) != tc.valid {
				t.Fatalf("凭据校验结果不符: %v", err)
			}
			if tc.valid && got.Credentials.MCP[0].DeviceID != "AA:BB:CC:DD:EE:01" {
				t.Fatal("MCP 设备未规范化")
			}
		})
	}
}
