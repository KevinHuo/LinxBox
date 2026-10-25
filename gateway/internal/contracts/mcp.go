package contracts

import (
	"encoding/json"
	"fmt"
	"io/fs"
)

// MCPInputSchema 从构建时嵌入的共享契约导出独立 inputSchema。
// 展开唯一的 DraftText 引用，使灵矽客户端无需联网解析仓库相对路径。
func MCPInputSchema() (map[string]any, error) {
	return mcpInputSchema(assetsFS)
}

func schemaDefinition(source fs.FS, name, definition string) (map[string]any, error) {
	raw, err := fs.ReadFile(source, "assets/"+name)
	if err != nil {
		return nil, err
	}
	var schema struct {
		Defs map[string]map[string]any `json:"$defs"`
	}
	if err := json.Unmarshal(raw, &schema); err != nil {
		return nil, err
	}
	value := schema.Defs[definition]
	if value == nil {
		return nil, fmt.Errorf("共享契约缺少 %s/%s", name, definition)
	}
	return value, nil
}

func mcpInputSchema(source fs.FS) (map[string]any, error) {
	args, err := schemaDefinition(source, "mcp.schema.json", "Arguments")
	if err != nil {
		return nil, err
	}
	text, err := schemaDefinition(source, "common.schema.json", "DraftText")
	if err != nil {
		return nil, err
	}
	properties, ok := args["properties"].(map[string]any)
	if !ok || args["type"] != "object" {
		return nil, fmt.Errorf("共享 MCP 参数必须是带 properties 的 object")
	}
	properties["text"] = text
	return args, nil
}
