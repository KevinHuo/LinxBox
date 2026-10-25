package contracts

import (
	"encoding/json"
	"errors"
	"io/fs"
	"strings"
	"testing"
	"testing/fstest"
)

func mustValidator(t *testing.T) *Validator {
	t.Helper()
	v, err := NewValidator()
	if err != nil {
		t.Fatalf("NewValidator 失败: %v", err)
	}
	return v
}

func TestNewValidator(t *testing.T) {
	v := mustValidator(t)
	// 全部入口应已编译。
	for _, name := range []string{
		FirmwareIncoming, FirmwareOutgoing, DesktopIncoming, DesktopOutgoing,
		MCPArgumentsEntry, MCPResultEntry,
	} {
		if _, ok := v.schemas[name]; !ok {
			t.Fatalf("缺少校验入口 %s", name)
		}
	}
	for _, op := range firmwareOps {
		if _, ok := v.schemas["fw.response."+op]; !ok {
			t.Fatalf("缺少固件 response 入口 %s", op)
		}
	}
	for _, typ := range firmwarePushes {
		if _, ok := v.schemas["fw.push."+typ]; !ok {
			t.Fatalf("缺少固件推送入口 %s", typ)
		}
	}
	for _, typ := range desktopTypes {
		if _, ok := v.schemas["dt.type."+typ]; !ok {
			t.Fatalf("缺少 Desktop 类型入口 %s", typ)
		}
	}
}

func TestValidateFirmwareIncoming(t *testing.T) {
	v := mustValidator(t)
	valid := []byte(`{"id":"f1","type":"device.hello","payload":{"mac_address":"AA:BB:CC:DD:EE:01"}}`)
	if err := v.ValidateFirmwareIncoming(valid); err != nil {
		t.Fatalf("合法消息被拒: %v", err)
	}
	// 多字段。
	extra := []byte(`{"id":"f1","type":"device.hello","payload":{"mac_address":"AA:BB:CC:DD:EE:01"},"unexpected":1}`)
	if err := v.ValidateFirmwareIncoming(extra); err == nil {
		t.Fatal("额外字段应被拒")
	}
	// 缺必填。
	missing := []byte(`{"id":"f1","type":"device.hello","payload":{}}`)
	if err := v.ValidateFirmwareIncoming(missing); err == nil {
		t.Fatal("缺必填应被拒")
	}
	// 非法 JSON。
	if err := v.ValidateFirmwareIncoming([]byte(`{`)); err == nil {
		t.Fatal("非法 JSON 应报错")
	}
	// 未开放方向的消息：response 不属于 FirmwareToGateway。
	resp := []byte(`{"id":"r1","type":"response","payload":{"ok":true,"data":{}},"reply_to":"f1"}`)
	if err := v.ValidateFirmwareIncoming(resp); err == nil {
		t.Fatal("response 不应属于固件入站方向")
	}
}

func TestValidateFirmwareResponse(t *testing.T) {
	v := mustValidator(t)
	// 用 agent_session.get.response 校验 ok 与 error 两种形态。
	ok := []byte(`{"id":"r1","type":"response","payload":{"ok":true,"data":{"agent_session_id":"s1","title":"t","project_name":"p","agent":"codex","status":"idle","summary":""}},"reply_to":"f1"}`)
	if err := v.ValidateFirmwareResponse("agent_session.get", ok); err != nil {
		t.Fatalf("合法响应被拒: %v", err)
	}
	errResp := []byte(`{"id":"r1","type":"response","payload":{"ok":false,"error":{"code":"NOT_FOUND","message":"会话不存在"}},"reply_to":"f1"}`)
	if err := v.ValidateFirmwareResponse("agent_session.get", errResp); err != nil {
		t.Fatalf("合法错误响应被拒: %v", err)
	}
	// 错误响应缺少 message。
	bad := []byte(`{"id":"r1","type":"response","payload":{"ok":false,"error":{"code":"NOT_FOUND"}},"reply_to":"f1"}`)
	if err := v.ValidateFirmwareResponse("agent_session.get", bad); err == nil {
		t.Fatal("缺 message 的错误应被拒")
	}
}

func TestValidateFirmwarePush(t *testing.T) {
	v := mustValidator(t)
	bound := []byte(`{"id":"p1","type":"device.bound","payload":{"device_id":"AA:BB:CC:DD:EE:01","desktop_client_id":"d1"}}`)
	if err := v.ValidateFirmwarePush("device.bound", bound); err != nil {
		t.Fatalf("合法推送被拒: %v", err)
	}
	bad := []byte(`{"id":"p1","type":"device.bound","payload":{"device_id":"AA:BB:CC:DD:EE:01"}}`)
	if err := v.ValidateFirmwarePush("device.bound", bad); err == nil {
		t.Fatal("缺 desktop_client_id 应被拒")
	}
	updated := []byte(`{"id":"p1","type":"agent_sessions.updated","payload":{}}`)
	if err := v.ValidateFirmwarePush("agent_sessions.updated", updated); err != nil {
		t.Fatalf("合法更新推送被拒: %v", err)
	}
}

func TestValidateDesktopIncoming(t *testing.T) {
	v := mustValidator(t)
	valid := []byte(`{"protocol_version":"1.0","message_id":"m1","sequence":1,"type":"client.hello","sent_at":"2026-10-25T00:00:00Z","payload":{"desktop_version":"0.1.0","protocol_versions":["1.0"],"last_acked_sequence":0,"capabilities":[]}}`)
	if err := v.ValidateDesktopIncoming(valid); err != nil {
		t.Fatalf("合法消息被拒: %v", err)
	}
	badTime := []byte(`{"protocol_version":"1.0","message_id":"m1","sequence":1,"type":"client.hello","sent_at":"not-a-time","payload":{"desktop_version":"0.1.0","protocol_versions":["1.0"],"last_acked_sequence":0,"capabilities":[]}}`)
	if err := v.ValidateDesktopIncoming(badTime); err == nil {
		t.Fatal("date-time 格式断言应生效")
	}
	// 方向错误：GatewayToDesktop 消息不应通过入站校验。
	dispatch := []byte(`{"protocol_version":"1.0","message_id":"m1","sequence":1,"type":"task.dispatch","sent_at":"2026-10-25T00:00:00Z","payload":{"task_id":"t1","project_id":"p1","agent":"codex","capabilities":["read"],"prompt":"x","acceptance_criteria":[],"policy_version":"v1","continuation":null}}`)
	if err := v.ValidateDesktopIncoming(dispatch); err == nil {
		t.Fatal("task.dispatch 不属于 Desktop 入站方向")
	}
}

func TestValidateDesktopType(t *testing.T) {
	v := mustValidator(t)
	ok := []byte(`{"protocol_version":"1.0","message_id":"m1","sequence":1,"type":"binding.result","sent_at":"2026-10-25T00:00:00Z","payload":{"reply_to_message_id":"b1","ok":true,"data":{"device_id":"AA:BB:CC:DD:EE:01","desktop_client_id":"d1"}}}`)
	if err := v.ValidateDesktopType("binding.result", ok); err != nil {
		t.Fatalf("合法应答被拒: %v", err)
	}
	syncOK := []byte(`{"protocol_version":"1.0","message_id":"m1","sequence":1,"type":"agent_sessions.sync.result","sent_at":"2026-10-25T00:00:00Z","payload":{"reply_to_message_id":"s1","ok":true}}`)
	if err := v.ValidateDesktopType("agent_sessions.sync.result", syncOK); err != nil {
		t.Fatalf("合法 sync 应答被拒: %v", err)
	}
}

func TestValidateMCP(t *testing.T) {
	v := mustValidator(t)
	args := []byte(`{"text":"帮我跑测试"}`)
	if err := v.ValidateMCPArguments(args); err != nil {
		t.Fatalf("合法参数被拒: %v", err)
	}
	badArgs := []byte(`{}`)
	if err := v.ValidateMCPArguments(badArgs); err == nil {
		t.Fatal("缺 text 应被拒")
	}
	emptyArgs := []byte(`{"text":""}`)
	if err := v.ValidateMCPArguments(emptyArgs); err == nil {
		t.Fatal("空 text 应被拒")
	}
	result := []byte(`{"ok":true,"data":{"request_id":"r1","agent_session_id":"s1","target_title":"t","text":"帮我跑测试","status":"awaiting_confirmation","error":null}}`)
	if err := v.ValidateMCPResult(result); err != nil {
		t.Fatalf("合法返回被拒: %v", err)
	}
}

func TestValidateFragment(t *testing.T) {
	v := mustValidator(t)
	valid := []byte(`{"device_id":"AA:BB:CC:DD:EE:01","desktop_client_id":null}`)
	if err := v.ValidateFragment("common.schema.json", "Binding", valid); err != nil {
		t.Fatalf("合法片段被拒: %v", err)
	}
	if err := v.ValidateFragment("common.schema.json", "DeviceId", []byte(`"AA:BB:CC:DD:EE:01"`)); err != nil {
		t.Fatalf("合法 DeviceId 被拒: %v", err)
	}
	if err := v.ValidateFragment("common.schema.json", "DeviceId", []byte(`"aa:bb:cc:dd:ee:01"`)); err == nil {
		t.Fatal("小写 DeviceId 应被拒")
	}
	if err := v.ValidateFragment("no-such.schema.json", "X", valid); err == nil {
		t.Fatal("未知 Schema 文件应报错")
	}
	// 未知校验入口。
	if err := v.validate("no.such.entry", valid); err == nil {
		t.Fatal("未知入口应报错")
	}
}

func TestValidateOutgoingDirections(t *testing.T) {
	v := mustValidator(t)
	// 固件出站：response 合法/非法。
	resp := []byte(`{"id":"r1","type":"response","payload":{"ok":true,"data":{"agent_session_id":"s1","title":"t","project_name":"p","agent":"codex","status":"idle","summary":""}},"reply_to":"f1"}`)
	if err := v.ValidateFirmwareOutgoing(resp); err != nil {
		t.Fatalf("合法出站被拒: %v", err)
	}
	badResp := []byte(`{"id":"r1","type":"response","payload":{"ok":true},"reply_to":"f1"}`)
	if err := v.ValidateFirmwareOutgoing(badResp); err == nil {
		t.Fatal("缺 data 的 response 应被拒")
	}
	// 固件出站推送：device.bound 合法。
	bound := []byte(`{"id":"p1","type":"device.bound","payload":{"device_id":"AA:BB:CC:DD:EE:01","desktop_client_id":"d1"}}`)
	if err := v.ValidateFirmwareOutgoing(bound); err != nil {
		t.Fatalf("合法推送被拒: %v", err)
	}
	// 入站消息不属于出站方向。
	incoming := []byte(`{"id":"f1","type":"device.hello","payload":{"mac_address":"AA:BB:CC:DD:EE:01"}}`)
	if err := v.ValidateFirmwareOutgoing(incoming); err == nil {
		t.Fatal("入站消息不应属于出站方向")
	}
	// Desktop 出站：binding.result 合法/非法。
	bindOK := []byte(`{"protocol_version":"1.0","message_id":"m1","sequence":1,"type":"binding.result","sent_at":"2026-10-25T00:00:00Z","payload":{"reply_to_message_id":"b1","ok":true,"data":{"device_id":"AA:BB:CC:DD:EE:01","desktop_client_id":"d1"}}}`)
	if err := v.ValidateDesktopOutgoing(bindOK); err != nil {
		t.Fatalf("合法 Desktop 出站被拒: %v", err)
	}
	badBind := []byte(`{"protocol_version":"1.0","message_id":"m1","sequence":1,"type":"binding.result","sent_at":"2026-10-25T00:00:00Z","payload":{"reply_to_message_id":"b1","ok":true}}`)
	if err := v.ValidateDesktopOutgoing(badBind); err == nil {
		t.Fatal("缺 data 的 binding.result 应被拒")
	}
	// Desktop 入站消息不属于出站方向。
	hello := []byte(`{"protocol_version":"1.0","message_id":"m1","sequence":1,"type":"client.hello","sent_at":"2026-10-25T00:00:00Z","payload":{"desktop_version":"0.1.0","protocol_versions":["1.0"],"last_acked_sequence":0,"capabilities":[]}}`)
	if err := v.ValidateDesktopOutgoing(hello); err == nil {
		t.Fatal("client.hello 不应属于出站方向")
	}
}

func TestLoadSchemaDocsFSErrors(t *testing.T) {
	valid := `{"$schema":"https://json-schema.org/draft/2020-12/schema","$id":"https://x/t.schema.json"}`
	tests := []struct {
		name string
		fi   map[string]string
		want string
	}{
		{"缺少目录", map[string]string{}, "assets"},
		{"非法 JSON", map[string]string{"assets/a.schema.json": "{"}, "不是合法 JSON"},
		{"非对象", map[string]string{"assets/a.schema.json": `[1,2]`}, "不是 JSON 对象"},
		{"缺 $id", map[string]string{"assets/a.schema.json": `{"a":1}`}, "缺少 $id"},
		{"空目录", map[string]string{"assets/readme.txt": "x"}, "没有 Schema"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			m := map[string]*fstest.MapFile{}
			for k, v := range tt.fi {
				m[k] = &fstest.MapFile{Data: []byte(v)}
			}
			_, err := loadSchemaDocsFS(fstest.MapFS(m))
			if err == nil {
				t.Fatal("应报错")
			}
			if !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("错误 %q 应包含 %q", err, tt.want)
			}
		})
	}
	// 正常路径。
	m := fstest.MapFS{
		"assets/common.schema.json": &fstest.MapFile{Data: []byte(valid)},
	}
	docs, err := loadSchemaDocsFS(m)
	if err != nil {
		t.Fatal(err)
	}
	if len(docs) != 1 {
		t.Fatalf("文档数错误: %d", len(docs))
	}
}

// openErrFS 只允许目录读取，.json 文件读取失败，用于覆盖 ReadFile 错误分支。
type openErrFS struct{ inner fstest.MapFS }

func (f openErrFS) Open(name string) (fs.File, error) {
	if strings.HasSuffix(name, ".json") {
		return nil, errors.New("读取失败")
	}
	return f.inner.Open(name)
}

func (f openErrFS) ReadDir(name string) ([]fs.DirEntry, error) { return f.inner.ReadDir(name) }

func TestLoadSchemaDocsFSReadFileError(t *testing.T) {
	m := fstest.MapFS{
		"assets/common.schema.json": &fstest.MapFile{Data: []byte(`{"$id":"https://x/c"}`)},
	}
	if _, err := loadSchemaDocsFS(openErrFS{inner: m}); err == nil {
		t.Fatal("读取失败应报错")
	}
}

func TestValidateFragmentCompileError(t *testing.T) {
	v := mustValidator(t)
	// 不存在的 def：Compile 失败分支。
	if err := v.ValidateFragment("common.schema.json", "No.Such.Def", []byte(`"x"`)); err == nil {
		t.Fatal("未知 def 应编译失败")
	}
	// 未知 Schema 文件。
	if err := v.ValidateFragment("unknown.schema.json", "DeviceId", []byte(`"x"`)); err == nil {
		t.Fatal("未知 Schema 文件应报错")
	}
}

func TestNewValidatorErrorPaths(t *testing.T) {
	// 加载失败。
	if _, err := newValidator(func() (map[string]any, error) { return nil, errors.New("加载失败") }); err == nil {
		t.Fatal("文档加载失败应报错")
	}
	// AddResource 拒绝非法文档（$ref 类型错误）。
	if _, err := newValidator(func() (map[string]any, error) {
		return map[string]any{CommonSchemaID: map[string]any{"$id": CommonSchemaID, "$ref": 123}}, nil
	}); err == nil {
		t.Fatal("非法文档应注册失败")
	}
	// AddResource 拒绝缺少 $id 的文档。
	if _, err := newValidator(func() (map[string]any, error) {
		return map[string]any{CommonSchemaID: map[string]any{"type": "object"}}, nil
	}); err == nil {
		t.Fatal("缺 $id 的文档应注册失败")
	}
	// 各编译入口逐个缺失对应 $defs，覆盖每个调用点的错误分支。
	realDocs, err := loadSchemaDocs()
	if err != nil {
		t.Fatal(err)
	}
	clone := func(m map[string]any, schemaID string) map[string]any {
		out := map[string]any{}
		for k, v := range m {
			out[k] = v
		}
		doc, _ := m[schemaID].(map[string]any)
		d2 := map[string]any{}
		for k, v := range doc {
			if defs, ok := v.(map[string]any); ok {
				// 深拷贝 $defs，避免破坏共享文档影响后续用例。
				nd := map[string]any{}
				for dk, dv := range defs {
					nd[dk] = dv
				}
				d2[k] = nd
				continue
			}
			d2[k] = v
		}
		out[schemaID] = d2
		return out
	}
	dropDef := func(m map[string]any, schemaID, def string) map[string]any {
		c := clone(m, schemaID)
		doc := c[schemaID].(map[string]any)
		defs := doc["$defs"].(map[string]any)
		delete(defs, def)
		// 同文档内部 $ref 的传递闭包：给出一个 def 名，收集它（经 $defs 引用）可达的所有 def。
		reachable := func(root string) map[string]bool {
			seen := map[string]bool{root: true}
			queue := []string{root}
			for len(queue) > 0 {
				cur := queue[0]
				queue = queue[1:]
				var walk func(any)
				walk = func(v any) {
					switch o := v.(type) {
					case map[string]any:
						if ref, ok := o["$ref"].(string); ok {
							if name, ok := strings.CutPrefix(ref, "#/$defs/"); ok {
								// 缺失定义也计入可达：只有这样才能让根分支
								// 被识别为依赖被删定义并触发改写。
								if !seen[name] {
									seen[name] = true
									if defs[name] != nil {
										queue = append(queue, name)
									}
								}
								return
							}
						}
						for _, vv := range o {
							walk(vv)
						}
					case []any:
						for _, vv := range o {
							walk(vv)
						}
					}
				}
				walk(defs[cur])
			}
			return seen
		}
		// 根 oneOf/anyOf/allOf 引用整个方向定义；jsonschema 编译资源时按根结构
		// 整体解析，任一可达引用缺失都会在最前面的调用点失败。因此：
		// 1) 依赖被删定义的根分支改写为无引用的叶子 schema，不再牵连早期编译；
		// 2) 其余分支原样保留，让错误精确定位到对应调用点。
		for _, k := range []string{"oneOf", "anyOf", "allOf"} {
			arr, ok := doc[k].([]any)
			if !ok {
				continue
			}
			kept := make([]any, 0, len(arr))
			for _, item := range arr {
				ref, ok := item.(map[string]any)
				if !ok {
					kept = append(kept, item)
					continue
				}
				loc, ok := ref["$ref"].(string)
				if !ok {
					kept = append(kept, item)
					continue
				}
				name, ok := strings.CutPrefix(loc, "#/$defs/")
				if ok && reachable(name)[def] {
					if defs[name] != nil {
						defs[name] = map[string]any{"type": "object"}
					}
					continue
				}
				kept = append(kept, item)
			}
			doc[k] = kept
		}
		return c
	}
	tests := []struct {
		name     string
		schemaID string
		def      string
	}{
		{"FirmwareToGateway", FirmwareSchemaID, "FirmwareToGateway"},
		{"GatewayToFirmware", FirmwareSchemaID, "GatewayToFirmware"},
		{"DesktopToGateway", DesktopSchemaID, "DesktopToGateway"},
		{"GatewayToDesktop", DesktopSchemaID, "GatewayToDesktop"},
		{"MCP Arguments", MCPSchemaID, "Arguments"},
		{"MCP Result", MCPSchemaID, "Result"},
		{"固件操作 response", FirmwareSchemaID, "device.hello.response"},
		{"固件推送", FirmwareSchemaID, "device.bound"},
		{"Desktop 出站", DesktopSchemaID, "binding.result"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			docs := dropDef(realDocs, tt.schemaID, tt.def)
			if _, err := newValidator(func() (map[string]any, error) { return docs, nil }); err == nil {
				t.Fatal("缺失定义应编译失败")
			}
		})
	}
}

func TestValidateFragmentCompileCache(t *testing.T) {
	v := mustValidator(t)
	// 同一 def 二次校验走缓存。
	if err := v.ValidateFragment("common.schema.json", "DeviceId", []byte(`"AA:BB:CC:DD:EE:01"`)); err != nil {
		t.Fatal(err)
	}
	if _, ok := v.schemas["frag.common.schema.json.DeviceId"]; !ok {
		t.Fatal("片段入口应被缓存")
	}
}

func TestValidatorErrorMessageIncludesContext(t *testing.T) {
	v := mustValidator(t)
	err := v.ValidateFirmwareIncoming([]byte(`{"id":"f1","type":"device.hello","payload":{"mac_address":42}}`))
	if err == nil {
		t.Fatal("应报错")
	}
	if !strings.Contains(err.Error(), "Schema 校验失败") {
		t.Fatalf("错误应包含校验上下文: %v", err)
	}
}

func TestRoundTripTypes(t *testing.T) {
	b := BoundBinding{DeviceID: "AA:BB:CC:DD:EE:01", DesktopClientID: "d1"}
	data, err := json.Marshal(b)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), `"desktop_client_id":"d1"`) {
		t.Fatalf("编码结果错误: %s", data)
	}
	// Binding 未绑定时 desktop_client_id 必须为 null 而非缺省。
	unbound := Binding{DeviceID: "AA:BB:CC:DD:EE:01"}
	data, err = json.Marshal(unbound)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), `"desktop_client_id":null`) {
		t.Fatalf("未绑定时应输出 null: %s", data)
	}
	// SessionPage 无下一页时 next_cursor 为 null。
	page := SessionPage{Items: []AgentSession{}}
	data, err = json.Marshal(page)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), `"next_cursor":null`) {
		t.Fatalf("无下一页应输出 null: %s", data)
	}
	// Empty 输出空对象。
	data, err = json.Marshal(Empty{})
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "{}" {
		t.Fatalf("Empty 应编码为 {}: %s", data)
	}
}
