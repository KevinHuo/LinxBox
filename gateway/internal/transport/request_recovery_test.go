package transport

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func TestRequestRecoveryLogsActualFailureAndInstance(t *testing.T) {
	env, capture := streamableEnv(t, 16, true)
	fw := prepareStreamableRequest(t, env)
	for range 2 {
		r := fw.fwReq(t, "request.get", map[string]any{"request_id": "missing-recovery"})
		if r.OK || r.Code != "NOT_FOUND" || r.Message != domain.RequestRecoveryMessage {
			t.Fatal(r)
		}
	}
	r := fw.fwReq(t, "request.get", map[string]any{"request_id": "req-1"})
	var snapshot contracts.Request
	if !r.OK || json.Unmarshal(r.RawData, &snapshot) != nil || snapshot.Status != "awaiting_text" {
		t.Fatal(r, string(r.RawData))
	}
	instance, failures := "", 0
	for _, record := range waitLog(t, capture, "request.state") {
		if record["event"] != "request.state" || record["type"] != "request.get" {
			continue
		}
		id, _ := record["gateway_instance_id"].(string)
		if id == "" || (instance != "" && instance != id) || record["database_config_id"] != databaseConfigID(env.cfg.Database.Path) {
			t.Fatal("缺少稳定实例/配置标识", record)
		}
		instance = id
		body := record["response_body"].(map[string]any)
		if record["ok"] == false {
			failures++
			if body["ok"] != false || body["data"] != nil || body["error"].(map[string]any)["message"] != domain.RequestRecoveryMessage {
				t.Fatal("日志必须对应真实错误应答", record)
			}
			if record["status"] != nil || record["agent_session_id"] != nil {
				t.Fatal("查不到记录不能伪造空快照字段", record)
			}
			if record["request_lookup"] != "missing" || record["retry_same_request"] != false || record["recovery_action"] != "verify_original_task" || !strings.Contains(record["error_hint"].(string), "request.prepare") {
				t.Fatal("恢复建议不明确", record)
			}
		} else if body["ok"] != true || body["data"].(map[string]any)["status"] != "awaiting_text" {
			t.Fatal("成功日志也要匹配应答", record)
		}
	}
	if failures != 2 {
		t.Fatal(failures)
	}
	if strings.Contains(capture.snapshot(), env.cfg.Database.Path) {
		t.Fatal("日志暴露数据库路径")
	}
	if databaseConfigID("different-path") == databaseConfigID(env.cfg.Database.Path) {
		t.Fatal("配置指纹没有区分路径")
	}
}
