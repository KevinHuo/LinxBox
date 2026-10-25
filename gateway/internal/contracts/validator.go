package contracts

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"path/filepath"
	"unicode/utf8"

	"github.com/santhosh-tekuri/jsonschema/v6"
)

// Schema 稳定标识（$id）。$id 不是需要请求的 HTTP 地址，仅用于离线解析相对引用。
const (
	CommonSchemaID   = "https://github.com/KevinHuo/LinxBox/contracts/common.schema.json"
	FirmwareSchemaID = "https://github.com/KevinHuo/LinxBox/contracts/firmware-gateway.schema.json"
	DesktopSchemaID  = "https://github.com/KevinHuo/LinxBox/contracts/desktop-gateway.schema.json"
	MCPSchemaID      = "https://github.com/KevinHuo/LinxBox/contracts/mcp.schema.json"
)

// 校验入口名称。
const (
	FirmwareIncoming  = "firmware.incoming"
	FirmwareOutgoing  = "firmware.outgoing"
	DesktopIncoming   = "desktop.incoming"
	DesktopOutgoing   = "desktop.outgoing"
	MCPArgumentsEntry = "mcp.arguments"
	MCPResultEntry    = "mcp.result"
)

// Validator 预加载并编译全部共享 Schema，全程离线，不解析不可信消息中的外部引用。
type Validator struct {
	compiler *jsonschema.Compiler
	schemas  map[string]*jsonschema.Schema
}

// NewValidator 编译共享 Schema 的所有入口。
func NewValidator() (*Validator, error) {
	return newValidator(loadSchemaDocs)
}

// newValidator 允许注入 Schema 文档来源，便于测试编译错误路径。
func newValidator(loadDocs func() (map[string]any, error)) (*Validator, error) {
	compiler := jsonschema.NewCompiler()
	compiler.DefaultDraft(jsonschema.Draft2020)
	compiler.AssertFormat()

	docs, err := loadDocs()
	if err != nil {
		return nil, err
	}
	for id, doc := range docs {
		if err := compiler.AddResource(id, doc); err != nil {
			return nil, fmt.Errorf("注册 Schema %s: %w", id, err)
		}
	}

	v := &Validator{compiler: compiler, schemas: map[string]*jsonschema.Schema{}}
	compile := func(name, loc string) error {
		s, err := compiler.Compile(loc)
		if err != nil {
			return fmt.Errorf("编译 %s 失败: %w", name, err)
		}
		v.schemas[name] = s
		return nil
	}

	if err := compile(FirmwareIncoming, FirmwareSchemaID+"#/$defs/FirmwareToGateway"); err != nil {
		return nil, err
	}
	if err := compile(FirmwareOutgoing, FirmwareSchemaID+"#/$defs/GatewayToFirmware"); err != nil {
		return nil, err
	}
	if err := compile(DesktopIncoming, DesktopSchemaID+"#/$defs/DesktopToGateway"); err != nil {
		return nil, err
	}
	if err := compile(DesktopOutgoing, DesktopSchemaID+"#/$defs/GatewayToDesktop"); err != nil {
		return nil, err
	}
	if err := compile(MCPArgumentsEntry, MCPSchemaID+"#/$defs/Arguments"); err != nil {
		return nil, err
	}
	if err := compile(MCPResultEntry, MCPSchemaID+"#/$defs/Result"); err != nil {
		return nil, err
	}

	// 固件操作 response：按原操作配对后使用对应定义校验 data。
	for _, op := range firmwareOps {
		if err := compile("fw.response."+op, FirmwareSchemaID+"#/$defs/"+op+".response"); err != nil {
			return nil, err
		}
	}
	// 固件主动推送。
	for _, typ := range firmwarePushes {
		if err := compile("fw.push."+typ, FirmwareSchemaID+"#/$defs/"+typ); err != nil {
			return nil, err
		}
	}
	// Desktop 出站消息类型。
	for _, typ := range desktopTypes {
		if err := compile("dt.type."+typ, DesktopSchemaID+"#/$defs/"+typ); err != nil {
			return nil, err
		}
	}
	for _, op := range []string{"messages.list", "message.content.get"} {
		if err := compile("dt.query."+op, DesktopSchemaID+"#/$defs/"+op+".result"); err != nil {
			return nil, err
		}
	}
	return v, nil
}

var firmwareOps = []string{
	"device.hello", "agent_sessions.list", "agent_session.get",
	"messages.list", "message.content.get",
	"request.prepare", "request.confirm", "request.cancel", "request.get",
}

var firmwarePushes = []string{
	"device.bound", "request.proposed", "request.updated",
	"agent_sessions.updated", "message.available",
}

var desktopTypes = []string{
	"binding.result", "agent_sessions.sync.result",
	"project.upsert", "project.revoke",
	"task.dispatch", "task.cancel",
	"messages.list", "message.content.get",
}

// ValidateFirmwareIncoming 校验固件 → Gateway 消息。
func (v *Validator) ValidateFirmwareIncoming(raw []byte) error {
	return v.validate(FirmwareIncoming, raw)
}

// ValidateFirmwareOutgoing 校验 Gateway → 固件消息。
func (v *Validator) ValidateFirmwareOutgoing(raw []byte) error {
	return v.validate(FirmwareOutgoing, raw)
}

// ValidateDesktopIncoming 校验 Desktop → Gateway 消息。
func (v *Validator) ValidateDesktopIncoming(raw []byte) error {
	return v.validate(DesktopIncoming, raw)
}

// ValidateDesktopOutgoing 校验 Gateway → Desktop 消息。
func (v *Validator) ValidateDesktopOutgoing(raw []byte) error {
	return v.validate(DesktopOutgoing, raw)
}

// ValidateMCPArguments 校验 propose_task_reply 工具参数。
func (v *Validator) ValidateMCPArguments(raw []byte) error {
	return v.validate(MCPArgumentsEntry, raw)
}

// ValidateMCPResult 校验 propose_task_reply 业务返回。
func (v *Validator) ValidateMCPResult(raw []byte) error {
	return v.validate(MCPResultEntry, raw)
}

// ValidateFirmwareResponse 按原操作对应的 response 定义校验固件应答。
func (v *Validator) ValidateFirmwareResponse(op string, raw []byte) error {
	return v.validate("fw.response."+op, raw)
}

// ValidateFirmwarePush 按推送类型定义校验固件推送。
func (v *Validator) ValidateFirmwarePush(typ string, raw []byte) error {
	return v.validate("fw.push."+typ, raw)
}

// ValidateDesktopType 按消息类型定义校验 Desktop 出站消息。
func (v *Validator) ValidateDesktopType(typ string, raw []byte) error {
	return v.validate("dt.type."+typ, raw)
}

// ValidateDesktopQueryResult 必须按原查询操作校验，不能只使用 query.result 联合入口。
func (v *Validator) ValidateDesktopQueryResult(op string, raw []byte) error {
	return v.validate("dt.query."+op, raw)
}

// ValidateFragment 校验原始 JSON 是否符合指定 Schema 文件的某个 $defs 定义，
// schemaName 为文件名（如 common.schema.json），def 为 $defs 下的名称。
// 用于复用根目录 fixtures 的正反例。
func (v *Validator) ValidateFragment(schemaName, def string, raw []byte) error {
	var id string
	switch schemaName {
	case "common.schema.json":
		id = CommonSchemaID
	case "firmware-gateway.schema.json":
		id = FirmwareSchemaID
	case "desktop-gateway.schema.json":
		id = DesktopSchemaID
	case "mcp.schema.json":
		id = MCPSchemaID
	default:
		return fmt.Errorf("未知 Schema 文件 %q", schemaName)
	}
	key := "frag." + schemaName + "." + def
	s, ok := v.schemas[key]
	if !ok {
		var err error
		s, err = v.compiler.Compile(id + "#/$defs/" + def)
		if err != nil {
			return err
		}
		v.schemas[key] = s
	}
	return v.validateSchema(s, raw)
}

func (v *Validator) validate(name string, raw []byte) error {
	s, ok := v.schemas[name]
	if !ok {
		return fmt.Errorf("未知校验入口 %q", name)
	}
	return v.validateSchema(s, raw)
}

func (v *Validator) validateSchema(s *jsonschema.Schema, raw []byte) error {
	if !utf8.Valid(raw) {
		return fmt.Errorf("消息不是有效 UTF-8")
	}
	var doc any
	if err := json.Unmarshal(raw, &doc); err != nil {
		return fmt.Errorf("JSON 解码失败: %w", err)
	}
	if err := s.Validate(doc); err != nil {
		return fmt.Errorf("Schema 校验失败: %w", err)
	}
	return nil
}

// loadSchemaDocs 从嵌入资源读取全部 Schema，返回 $id → 文档。
func loadSchemaDocs() (map[string]any, error) {
	return loadSchemaDocsFS(assetsFS)
}

// loadSchemaDocsFS 从任意文件系统读取 Schema 文档；独立出来以便测试错误路径。
func loadSchemaDocsFS(fsys fs.FS) (map[string]any, error) {
	entries, err := fs.ReadDir(fsys, "assets")
	if err != nil {
		return nil, err
	}
	docs := map[string]any{}
	for _, e := range entries {
		if filepath.Ext(e.Name()) != ".json" {
			continue
		}
		data, err := fs.ReadFile(fsys, "assets/"+e.Name())
		if err != nil {
			return nil, err
		}
		var doc any
		if err := json.Unmarshal(data, &doc); err != nil {
			return nil, fmt.Errorf("%s 不是合法 JSON: %w", e.Name(), err)
		}
		m, ok := doc.(map[string]any)
		if !ok {
			return nil, fmt.Errorf("%s 不是 JSON 对象", e.Name())
		}
		id, _ := m["$id"].(string)
		if id == "" {
			return nil, fmt.Errorf("%s 缺少 $id", e.Name())
		}
		docs[id] = doc
	}
	if len(docs) == 0 {
		return nil, fmt.Errorf("嵌入资源中没有 Schema")
	}
	return docs, nil
}
