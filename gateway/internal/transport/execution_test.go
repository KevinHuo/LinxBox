package transport

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/gorilla/websocket"
)

func preparePipeExecution(t *testing.T) (*Server, *websocket.Conn, *websocket.Conn, contracts.DesktopMessage, contracts.TaskDispatchPayload) {
	t.Helper()
	s, fw, dt := setupPipe(t)
	pipeWrite(t, fw, firmwareWire("request.prepare", "prepare", map[string]any{"request_id": "execute", "agent_session_id": "sess-1"}))
	pipeRead(t, fw, "response")
	if _, err := s.requests.Propose(t.Context(), "AA:BB:CC:DD:EE:01", "execute", "agent", "voice", "继续检查这段代码"); err != nil {
		t.Fatal(err)
	}
	pipeRead(t, fw, "request.proposed")
	pipeWrite(t, fw, firmwareWire("request.confirm", "confirm", map[string]any{"request_id": "execute"}))
	frame := pipeRead(t, dt, "task.dispatch")
	raw, _ := json.Marshal(frame)
	if err := s.validator.ValidateDesktopType("task.dispatch", raw); err != nil {
		t.Fatal(err)
	}
	var message contracts.DesktopMessage
	if err := json.Unmarshal(raw, &message); err != nil {
		t.Fatal(err)
	}
	var payload contracts.TaskDispatchPayload
	if err := json.Unmarshal(message.Payload, &payload); err != nil {
		t.Fatal(err)
	}
	response := pipeRead(t, fw, "response")
	var body contracts.FirmwareResponseBody
	if err := json.Unmarshal(response["payload"], &body); err != nil || !body.OK {
		t.Fatalf("确认响应失败: %s %v", response["payload"], err)
	}
	if payload.Continuation.ThreadID != "thread" || payload.Continuation.AgentSessionID != "sess-1" || payload.Prompt != "继续检查这段代码" {
		t.Fatalf("投递目标改变: %+v", payload)
	}
	return s, fw, dt, message, payload
}

func taskPipeEvent(payload contracts.TaskDispatchPayload, typ string, sequence int64) map[string]any {
	state := map[string]string{"task.started": "running", "task.progress": "running", "task.completed": "succeeded", "task.failed": "failed", "task.interrupted": "interrupted"}[typ]
	return map[string]any{"task_id": payload.TaskID, "event_id": contracts.NewMessageID("evt_"), "event_sequence": sequence,
		"occurred_at": time.Now().UTC().Format(time.RFC3339Nano), "state": state, "stage": "finished", "summary": "本轮状态",
		"result": nil, "error": nil, "thread_id": "thread", "turn_id": "turn"}
}

func expectRequestPush(t *testing.T, c *websocket.Conn, status string) {
	t.Helper()
	frame := pipeRead(t, c, "request.updated")
	var r contracts.Request
	if err := json.Unmarshal(frame["payload"], &r); err != nil || r.Status != status {
		t.Fatalf("期望 %s: %s %v", status, frame["payload"], err)
	}
}

func TestExecutionInteropConfirmFactsAndNoRedispatch(t *testing.T) {
	s, fw, dt, dispatch, payload := preparePipeExecution(t)
	pipeWrite(t, dt, desktopWire("task.started", "started", taskPipeEvent(payload, "task.started", 1)))
	expectRequestPush(t, fw, "running")
	// ACK 晚到不能把 running 回退为 sending。
	pipeWrite(t, dt, desktopWire("task.ack", "ack", contracts.TaskAckPayload{TaskID: payload.TaskID, MessageID: dispatch.MessageID, Sequence: dispatch.Sequence, Disposition: "accepted"}))
	pipeWrite(t, fw, firmwareWire("request.confirm", "confirm-again", map[string]any{"request_id": "execute"}))
	response := pipeRead(t, fw, "response")
	if err := s.validator.ValidateFirmwareResponse("request.confirm", mustJSON(t, response)); err != nil {
		t.Fatal(err)
	}
	pipeWrite(t, fw, firmwareWire("messages.list", "query-barrier", map[string]any{"agent_session_id": "sess-1"}))
	// 下一个 Desktop 帧必须是查询，不允许重复确认产生第二个 dispatch。
	dt.SetReadDeadline(time.Now().Add(time.Second))
	var next contracts.DesktopMessage
	if err := dt.ReadJSON(&next); err != nil || next.Type != "messages.list" {
		t.Fatalf("重复确认产生额外投递: %+v %v", next, err)
	}
	pipeWrite(t, dt, desktopWire("query.result", "query-answer", map[string]any{"reply_to_message_id": next.MessageID, "ok": true, "data": map[string]any{"items": []any{}, "next_cursor": nil}}))
	pipeRead(t, fw, "response")
	pipeWrite(t, dt, desktopWire("task.progress", "progress", taskPipeEvent(payload, "task.progress", 2)))
	completed := taskPipeEvent(payload, "task.completed", 3)
	completed["result"] = map[string]any{"contract_version": "task-result-v1", "status": "succeeded", "summary": "本轮完成", "changes": []any{}, "verification": []any{}, "risks": []any{}, "needs_user_action": nil, "voice_summary": "本轮完成"}
	pipeWrite(t, dt, desktopWire("task.completed", "completed", completed))
	expectRequestPush(t, fw, "completed")
	pipeWrite(t, fw, firmwareWire("request.get", "get", map[string]any{"request_id": "execute"}))
	response = pipeRead(t, fw, "response")
	var body struct {
		Data contracts.Request `json:"data"`
	}
	if err := json.Unmarshal(response["payload"], &body); err != nil || body.Data.Status != "completed" {
		t.Fatalf("最终快照不一致: %+v %v", body, err)
	}
}

func mustJSON(t *testing.T, value any) []byte {
	t.Helper()
	raw, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func TestExecutionInteropDisconnectAndAuthenticatedRecovery(t *testing.T) {
	s, fw, dt, _, payload := preparePipeExecution(t)
	dt.Close()
	expectRequestPush(t, fw, "unknown")
	newer := pipeDial(t, s, "/v1/clients/connect", "dt-token")
	completed := taskPipeEvent(payload, "task.completed", 1)
	completed["result"] = map[string]any{"contract_version": "task-result-v1", "status": "succeeded", "summary": "本轮完成", "changes": []any{}, "verification": []any{}, "risks": []any{}, "needs_user_action": nil, "voice_summary": "本轮完成"}
	pipeWrite(t, newer, desktopWire("task.completed", "recovered", completed))
	expectRequestPush(t, fw, "completed")
}

func TestExecutionPrepareDispatchBoundaries(t *testing.T) {
	s, _, _ := setupPipe(t)
	payload := contracts.TaskDispatchPayload{TaskID: "task", ProjectID: "prj", Agent: "codex", Capabilities: []string{}, Prompt: "追问", AcceptanceCriteria: []string{}, PolicyVersion: "request-confirm/v1", Continuation: contracts.Continuation{AgentSessionID: "sess-1", ThreadID: "thread"}}
	if _, err := s.PrepareDispatch("offline", payload); domain.CodeOf(err) != domain.CodeDependencyUnavailable {
		t.Fatal(err)
	}
	bad := payload
	bad.Continuation.ThreadID = ""
	if _, err := s.PrepareDispatch("desktop-1", bad); domain.CodeOf(err) != domain.CodeDependencyUnavailable {
		t.Fatal(err)
	}
	s.cfg.Limits.MaxFrameBytes = 10
	if _, err := s.PrepareDispatch("desktop-1", payload); domain.CodeOf(err) != domain.CodeResourceExhausted {
		t.Fatal(err)
	}
	s.cfg.Limits.MaxFrameBytes = 65536
	delivery, err := s.PrepareDispatch("desktop-1", payload)
	if err != nil {
		t.Fatal(err)
	}
	old := s.conns.Desktop("desktop-1")
	s.conns.UnregisterDesktop("desktop-1", old)
	if err := delivery.Send(); err == nil {
		t.Fatal("旧连接不得投递")
	}
}
