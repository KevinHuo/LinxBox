package service

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

type queryReply struct {
	q    Query
	data json.RawMessage
	err  error
}

func queryFixture(t *testing.T, timeout time.Duration, total, device int) (*QueryRouter, chan queryReply) {
	t.Helper()
	v, err := contracts.NewValidator()
	if err != nil {
		t.Fatal(err)
	}
	ch := make(chan queryReply, 16)
	r := NewQueryRouter(v, timeout, total, device, func(q Query, data json.RawMessage, err error) { ch <- queryReply{q, data, err} })
	t.Cleanup(func() { r.Disconnect(true, "device", 1); r.Disconnect(true, "other", 1) })
	return r, ch
}
func baseQuery(op string) Query {
	return Query{DeviceID: "device", DesktopID: "desktop", FirmwareID: "firmware", Operation: op, DeviceGeneration: 1, DesktopGeneration: 2}
}
func queryResult(q Query, data any, err any) []byte {
	payload := map[string]any{"reply_to_message_id": q.ID, "ok": err == nil}
	if err == nil {
		payload["data"] = data
	} else {
		payload["error"] = err
	}
	raw, _ := json.Marshal(map[string]any{"protocol_version": "1.0", "message_id": "result", "sequence": 1, "type": "query.result", "sent_at": time.Now().UTC().Format(time.RFC3339Nano), "payload": payload})
	return raw
}

func TestQueryLimitsAndCorrelation(t *testing.T) {
	r, ch := queryFixture(t, time.Second, 2, 1)
	if _, err := r.Begin(baseQuery("task.status.get")); err == nil {
		t.Fatal("未知读取操作应拒绝")
	}
	q, err := r.Begin(baseQuery("messages.list"))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := r.Begin(baseQuery("messages.list")); domain.CodeOf(err) != domain.CodeConflict {
		t.Fatal("重复包 ID 应拒绝")
	}
	second := baseQuery("messages.list")
	second.FirmwareID = "different"
	if _, err := r.Begin(second); domain.CodeOf(err) != domain.CodeResourceExhausted {
		t.Fatal("设备并发上限未生效")
	}
	second.DeviceID = "other"
	if _, err := r.Begin(second); err != nil {
		t.Fatal(err)
	}
	third := second
	third.FirmwareID = "third"
	if _, err := r.Begin(third); domain.CodeOf(err) != domain.CodeResourceExhausted {
		t.Fatal("全局上限未生效")
	}
	valid := queryResult(q, map[string]any{"items": []any{}, "next_cursor": nil}, nil)
	for _, identity := range []struct {
		id         string
		generation uint64
	}{{"forged", 2}, {"desktop", 3}} {
		if err := r.Complete(identity.id, identity.generation, valid); err == nil {
			t.Fatal("伪造来源应拒绝")
		}
	}
	if err := r.Complete("desktop", 2, queryResult(q, map[string]any{"text": "wrong", "next_offset": nil}, nil)); err == nil {
		t.Fatal("正文不能作为历史应答")
	}
	if err := r.Complete("desktop", 2, []byte("{")); err == nil {
		t.Fatal("非法 JSON 应拒绝")
	}
	if err := r.Complete("desktop", 2, valid); err != nil {
		t.Fatal(err)
	}
	reply := <-ch
	if reply.err != nil || reply.q.FirmwareID != "firmware" {
		t.Fatal("关联丢失")
	}
	if err := r.Complete("desktop", 2, valid); err == nil {
		t.Fatal("重复应答应拒绝")
	}
}

func TestQueryContentByteValidation(t *testing.T) {
	r, ch := queryFixture(t, time.Second, 8, 8)
	for _, tc := range []struct {
		name, text string
		next       any
		ok         bool
	}{
		{"oversized", strings.Repeat("中", 342), nil, false},
		{"wrong-offset", "中🙂", 8, false},
		{"empty-progress", "", 4, false},
		{"unicode", "中🙂", 10, true},
		{"final", "结束", nil, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			q0 := baseQuery("message.content.get")
			q0.Offset = 3
			q0.FirmwareID = tc.name
			q, err := r.Begin(q0)
			if err != nil {
				t.Fatal(err)
			}
			err = r.Complete("desktop", 2, queryResult(q, map[string]any{"text": tc.text, "next_offset": tc.next}, nil))
			if (err == nil) != tc.ok {
				t.Fatalf("ok=%v err=%v", tc.ok, err)
			}
			if tc.ok {
				if reply := <-ch; reply.err != nil {
					t.Fatal(reply.err)
				}
			} else {
				r.Fail(q.ID, domain.ErrInvalidArgument("测试结束"))
				<-ch
			}
		})
	}
}

func TestQueryTimeoutDisconnectAndErrors(t *testing.T) {
	r, ch := queryFixture(t, 20*time.Millisecond, 8, 8)
	q, err := r.Begin(baseQuery("messages.list"))
	if err != nil {
		t.Fatal(err)
	}
	select {
	case reply := <-ch:
		if domain.CodeOf(reply.err) != domain.CodeDependencyUnavailable {
			t.Fatal("超时错误不正确")
		}
	case <-time.After(time.Second):
		t.Fatal("未超时")
	}
	r.Fail(q.ID, domain.ErrInternal("late"))
	q, _ = r.Begin(baseQuery("messages.list"))
	r.Disconnect(false, "desktop", 9)
	if err := r.Complete("desktop", 2, queryResult(q, nil, map[string]any{"code": "NOT_FOUND", "message": "不存在"})); err != nil {
		t.Fatal(err)
	}
	if reply := <-ch; domain.CodeOf(reply.err) != domain.CodeNotFound {
		t.Fatal("Desktop 错误未透传")
	}
	q, _ = r.Begin(baseQuery("messages.list"))
	r.Disconnect(false, "desktop", 2)
	if reply := <-ch; domain.CodeOf(reply.err) != domain.CodeDependencyUnavailable {
		t.Fatal("断线错误不正确")
	}
	if err := r.Complete("desktop", 2, queryResult(q, map[string]any{"items": []any{}, "next_cursor": nil}, nil)); err == nil {
		t.Fatal("断线后的迟到结果应拒绝")
	}
	q, _ = r.Begin(baseQuery("messages.list"))
	r.Disconnect(true, "device", 1)
	if len(r.pending) != 0 {
		t.Fatal("设备断线未释放")
	}
	select {
	case <-ch:
		t.Fatal("不能给重连设备发送旧应答")
	default:
	}
	defaults := NewQueryRouter(r.validator, 0, 0, 0, func(Query, json.RawMessage, error) {})
	if defaults.timeout != 15*time.Second || defaults.maxDevice != 4 || defaults.maxTotal != 128 {
		t.Fatal("默认上限不正确")
	}
}

func TestQueryRejectsExpiredResultBeforeTimerRuns(t *testing.T) {
	r, ch := queryFixture(t, time.Hour, 8, 8)
	q, err := r.Begin(baseQuery("messages.list"))
	if err != nil {
		t.Fatal(err)
	}
	r.pending[q.ID].query.Deadline = time.Now().Add(-time.Second)
	if err := r.Complete("desktop", 2, queryResult(q, map[string]any{"items": []any{}, "next_cursor": nil}, nil)); err == nil {
		t.Fatal("过期应答应拒绝")
	}
	if reply := <-ch; domain.CodeOf(reply.err) != domain.CodeDependencyUnavailable {
		t.Fatal("应通知查询超时")
	}
}
