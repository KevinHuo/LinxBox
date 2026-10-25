package contracts

import (
	"encoding/json"
	"testing"
	"testing/fstest"
)

func TestMCPInputSchemaExport(t *testing.T) {
	schema, err := MCPInputSchema()
	if err != nil {
		t.Fatal(err)
	}
	properties := schema["properties"].(map[string]any)
	if len(properties) != 1 || properties["text"].(map[string]any)["maxLength"] != float64(16000) || schema["additionalProperties"] != false {
		t.Fatalf("导出字段偏离共享契约: %+v", schema)
	}
	for _, tc := range []struct{ tool, common string }{
		{"", ""},
		{"{", ""},
		{`{}`, ""},
		{`{"$defs":{"Arguments":{"type":"object"}}}`, ""},
		{`{"$defs":{"Arguments":{"type":"object"}}}`, "{"},
		{`{"$defs":{"Arguments":{"type":"object"}}}`, `{"$defs":{"DraftText":{"type":"string"}}}`},
	} {
		source := fstest.MapFS{}
		if tc.tool != "" {
			source["assets/mcp.schema.json"] = &fstest.MapFile{Data: []byte(tc.tool)}
		}
		if tc.common != "" {
			source["assets/common.schema.json"] = &fstest.MapFile{Data: []byte(tc.common)}
		}
		if _, err := mcpInputSchema(source); err == nil {
			t.Fatal("非法共享契约被接受")
		}
	}
	// 独立序列化，无外部引用，接入客户端可以直接加载。
	raw, err := json.Marshal(schema)
	if err != nil || len(raw) == 0 {
		t.Fatal(err)
	}
}
