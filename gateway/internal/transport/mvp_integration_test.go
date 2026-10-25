package transport

import (
	"encoding/json"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
)

// 同一份实现必须兼容缺头 MCP 提案、确认执行和缺结果恢复，防止整合时遗漏。
func TestMVPStreamableImplicitConfirmDispatchAndCompletion(t *testing.T) {
	for _, recovery := range []bool{false, true} {
		name := "direct_completion"
		if recovery {
			name = "incomplete_result_recovery"
		}
		t.Run(name, func(t *testing.T) { runMVPStreamableExecution(t, recovery) })
	}
}

func runMVPStreamableExecution(t *testing.T, recovery bool) {
	t.Helper()
	env, capture := streamableEnv(t, 16, true)
	fw := dialWS(t, env.baseWS+"/v1/devices/connect", "dev-token")
	if r := fw.fwReq(t, "device.hello", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !r.OK {
		t.Fatal(r)
	}
	dt := dialWS(t, env.baseWS+"/v1/clients/connect", "dt-token")
	if r := dt.dtReq(t, "device.bind", map[string]any{"mac_address": "AA:BB:CC:DD:EE:01"}); !r.OK {
		t.Fatal(r)
	}
	if r := dt.dtReq(t, "agent_sessions.sync", map[string]any{"items": []any{sessionPayload(1)}}); !r.OK {
		t.Fatal(r)
	}
	if r := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "mvp-request", "agent_session_id": "sess-1"}); !r.OK {
		t.Fatal(r)
	}
	client, headers := newStreamableClient(t, env)
	for key, value := range map[string]string{"X-Linx-Device-Id": "AA:BB:CC:DD:EE:01", "X-Linx-Agent-Id": "agent", "X-Linx-Session-Id": "voice"} {
		headers.set(key, value)
	}
	const text = "  模拟语音的完整追问🙂\n"
	if r := callMCP(t, env, client, map[string]any{"text": text}, ""); r.RequestID != "mvp-request" || r.Status != "awaiting_confirmation" {
		t.Fatal(r)
	}
	fw.expectPush(t, "request.proposed")
	check := func(r *fwResp, status string) {
		t.Helper()
		var request contracts.Request
		if !r.OK || json.Unmarshal(r.RawData, &request) != nil || request.Status != status || request.RequestID != "mvp-request" {
			t.Fatal(status, r, string(r.RawData))
		}
	}
	check(fw.fwReq(t, "request.confirm", map[string]any{"request_id": "mvp-request"}), "sending")
	var dispatch contracts.DesktopMessage
	if err := json.Unmarshal(dt.expectPush(t, "task.dispatch"), &dispatch); err != nil {
		t.Fatal(err)
	}
	var payload contracts.TaskDispatchPayload
	if err := json.Unmarshal(dispatch.Payload, &payload); err != nil {
		t.Fatal(err)
	}
	if payload.Prompt != text || payload.Continuation.ThreadID != "thread" || payload.Continuation.AgentSessionID != "sess-1" || payload.ProjectID != "prj" {
		t.Fatal(payload)
	}
	pushState := func(want string) {
		t.Helper()
		var frame struct {
			Payload contracts.Request `json:"payload"`
		}
		if err := json.Unmarshal(fw.expectPush(t, "request.updated"), &frame); err != nil || frame.Payload.Status != want {
			t.Fatal(want, frame, err)
		}
	}
	pushState("sending")
	send := func(typ string, payload any) {
		t.Helper()
		dt.writeMu.Lock()
		defer dt.writeMu.Unlock()
		if err := dt.conn.WriteJSON(desktopWire(typ, contracts.NewMessageID("desktop_"), payload)); err != nil {
			t.Fatal(err)
		}
	}
	send("task.ack", contracts.TaskAckPayload{TaskID: payload.TaskID, MessageID: dispatch.MessageID, Sequence: dispatch.Sequence, Disposition: "accepted"})
	send("task.started", taskPipeEvent(payload, "task.started", 1))
	pushState("running")
	completed := taskPipeEvent(payload, "task.completed", 2)
	if recovery {
		send("task.completed", completed)
		pushState("unknown")
		check(fw.fwReq(t, "request.get", map[string]any{"request_id": "mvp-request"}), "unknown")
		check(fw.fwReq(t, "request.confirm", map[string]any{"request_id": "mvp-request"}), "unknown")
		blocked := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "next-request", "agent_session_id": "sess-1"})
		if blocked.OK || blocked.Code != "CONFLICT" {
			t.Fatal("缺少结果时应保持原请求占用", blocked)
		}
		missing := fw.fwReq(t, "request.get", map[string]any{"request_id": "next-request"})
		if missing.OK || missing.Code != "NOT_FOUND" {
			t.Fatal("prepare 失败不能创建新请求", missing)
		}
		completed = taskPipeEvent(payload, "task.completed", 3)
	}
	completed["result"] = map[string]any{"contract_version": "task-result-v1", "status": "succeeded", "summary": "模拟执行完成", "changes": []any{}, "verification": []any{}, "risks": []any{}, "needs_user_action": nil, "voice_summary": "模拟执行完成"}
	send("task.completed", completed)
	pushState("completed")
	check(fw.fwReq(t, "request.get", map[string]any{"request_id": "mvp-request"}), "completed")
	check(fw.fwReq(t, "request.confirm", map[string]any{"request_id": "mvp-request"}), "completed")
	count := 0
	for _, record := range waitLog(t, capture, "task.enqueued") {
		if record["event"] == "task.enqueued" {
			count++
		}
	}
	if count != 1 {
		t.Fatal("完整链路重复投递", count)
	}
	if recovery {
		if r := fw.fwReq(t, "request.prepare", map[string]any{"request_id": "next-request", "agent_session_id": "sess-1"}); !r.OK {
			t.Fatal("有效补报后应释放原请求占用", r)
		}
	}
}
