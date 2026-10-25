package config

import (
	"strings"
	"testing"
)

func TestMCPCredentials(t *testing.T) {
	for _, tc := range []struct {
		name       string
		credential MCPCredential
		valid      bool
	}{
		{"规范化", MCPCredential{Token: "mcp-token", DeviceID: "aabbccddee01", AgentID: "agent"}, true},
		{"空凭据", MCPCredential{DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"}, false},
		{"复用设备凭据", MCPCredential{Token: "dev-token", DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"}, false},
		{"设备可在之后登记", MCPCredential{Token: "mcp-token", DeviceID: "AA:BB:CC:DD:EE:02", AgentID: "agent"}, true},
		{"非法设备", MCPCredential{Token: "mcp-token", DeviceID: "invalid", AgentID: "agent"}, false},
		{"缺少智能体", MCPCredential{Token: "mcp-token", DeviceID: "AA:BB:CC:DD:EE:01"}, false},
		{"账号动态设备", MCPCredential{Token: "mcp-token", Subject: "alice", AgentID: "agent"}, true},
		{"缺少范围", MCPCredential{Token: "mcp-token", AgentID: "agent"}, false},
		{"双重范围", MCPCredential{Token: "mcp-token", Subject: "alice", DeviceID: "AA:BB:CC:DD:EE:01", AgentID: "agent"}, false},
		{"账号过长", MCPCredential{Token: "mcp-token", Subject: strings.Repeat("a", 129), AgentID: "agent"}, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := validConfig()
			cfg.Credentials.MCP = []MCPCredential{tc.credential}
			got, err := Load(writeConfig(t, cfg))
			if (err == nil) != tc.valid {
				t.Fatalf("凭据校验结果不符: %v", err)
			}
			if tc.name == "规范化" && got.Credentials.MCP[0].DeviceID != "AA:BB:CC:DD:EE:01" {
				t.Fatal("MCP 设备未规范化")
			}
		})
	}
}
