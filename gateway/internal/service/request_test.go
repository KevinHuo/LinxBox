package service

import (
	"context"
	"database/sql"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

type requestNotice struct {
	device, event string
	request       contracts.Request
}
type requestNotifier struct{ events []requestNotice }

func (n *requestNotifier) NotifyRequest(device, event string, request contracts.Request) {
	n.events = append(n.events, requestNotice{device, event, request})
}

type requestEnv struct {
	s   *RequestService
	db  *storage.DB
	n   *requestNotifier
	now time.Time
}

const requestDevice domain.DeviceID = "AA:BB:CC:DD:EE:01"

func newRequestEnv(t *testing.T) *requestEnv {
	t.Helper()
	base := newServiceEnv(t)
	db, err := storage.Open(filepath.Join(t.TempDir(), "requests.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	repos := storage.NewRepositories(db.DB())
	ctx := t.Context()
	for i, device := range []string{string(requestDevice), "AA:BB:CC:DD:EE:02"} {
		if _, err := repos.Devices.Register(ctx, db.DB(), device, "alice", config.DeviceCredentialKey(base.cfg.Credentials.Devices[i].Token)); err != nil {
			t.Fatal(err)
		}
		if err := repos.Bindings.Insert(ctx, db.DB(), storage.BindingRow{DeviceID: device, DesktopClientID: "desktop-1", Subject: "alice", CreatedAt: time.Now()}); err != nil {
			t.Fatal(err)
		}
	}
	if err := repos.WithTx(ctx, func(tx *sql.Tx) error {
		return repos.Sessions.ReplaceAll(ctx, tx, "desktop-1", []storage.SessionRow{
			{DesktopClientID: "desktop-1", AgentSessionID: "session", ThreadID: "thread", ProjectID: "project", Title: "固定标题", Status: "idle"},
			{DesktopClientID: "desktop-1", AgentSessionID: "other", ThreadID: "other-thread", ProjectID: "project", Title: "另一会话", Status: "idle"},
		}, 1, time.Now())
	}); err != nil {
		t.Fatal(err)
	}
	n := &requestNotifier{}
	e := &requestEnv{db: db, n: n, now: time.Now()}
	e.s = NewRequestService(repos, base.cfg, n)
	e.s.now = func() time.Time { return e.now }
	return e
}

func requestCode(t *testing.T, err error, want domain.Code) {
	t.Helper()
	if err == nil || domain.CodeOf(err) != want {
		t.Fatalf("期望 %s，得到 %v", want, err)
	}
}

func (e *requestEnv) prepare(t *testing.T, id string) contracts.Request {
	t.Helper()
	r, err := e.s.Prepare(t.Context(), requestDevice, id, "session")
	if err != nil {
		t.Fatal(err)
	}
	return r
}
func (e *requestEnv) propose(t *testing.T, id, text string) (contracts.Request, error) {
	t.Helper()
	return e.s.Propose(t.Context(), requestDevice, id, "agent", "voice-session", text)
}

func TestRequestOriginalTextAndIdempotency(t *testing.T) {
	e := newRequestEnv(t)
	r := e.prepare(t, "req")
	if r.Status != "awaiting_text" || r.Text != nil || r.Error != nil {
		t.Fatalf("初始状态错误: %+v", r)
	}
	before, err := e.s.requests.Get(t.Context(), e.db.DB(), "req")
	if err != nil {
		t.Fatal(err)
	}
	e.now = e.now.Add(10 * time.Second)
	e.prepare(t, "req")
	after, err := e.s.requests.Get(t.Context(), e.db.DB(), "req")
	if err != nil {
		t.Fatal(err)
	}
	if before.ExpiresAt != after.ExpiresAt {
		t.Fatal("重复 prepare 延长了期限")
	}
	_, err = e.s.Prepare(t.Context(), requestDevice, "req", "other")
	requestCode(t, err, domain.CodeConflict)
	_, err = e.s.Prepare(t.Context(), requestDevice, "second", "other")
	requestCode(t, err, domain.CodeConflict)
	_, err = e.s.Prepare(t.Context(), "AA:BB:CC:DD:EE:02", "second", "session")
	requestCode(t, err, domain.CodeConflict)
	text := "  你好，今天天气怎么样？🙂\n"
	r, err = e.propose(t, "req", text)
	if err != nil || r.Text == nil || *r.Text != text || r.Status != "awaiting_confirmation" {
		t.Fatalf("原文未保存: %+v %v", r, err)
	}
	if len(e.n.events) != 1 || e.n.events[0].event != "request.proposed" {
		t.Fatal("未推送文字")
	}
	e.now = e.now.Add(time.Second)
	if _, err := e.propose(t, "req", text); err != nil {
		t.Fatal(err)
	}
	if len(e.n.events) != 1 {
		t.Fatal("重复 MCP 重复推送")
	}
	_, err = e.propose(t, "req", "改写")
	requestCode(t, err, domain.CodeConflict)
	_, err = e.s.Propose(t.Context(), requestDevice, "req", "agent", "another-session", text)
	requestCode(t, err, domain.CodeConflict)
	// 固定目标不会随目录标题变化而改变。
	if _, err := e.db.DB().Exec(`UPDATE session_catalog SET title='新标题'`); err != nil {
		t.Fatal(err)
	}
	r = e.prepare(t, "req")
	if r.TargetTitle != "固定标题" {
		t.Fatal("prepare 重试更换了标题")
	}
	// 重建服务模拟进程恢复，仍返回同一文字，不发第二次通知。
	restarted := NewRequestService(storage.NewRepositories(e.db.DB()), e.s.cfg, e.n)
	r, err = restarted.Get(t.Context(), requestDevice, "req")
	if err != nil || r.Text == nil || *r.Text != text {
		t.Fatalf("恢复失败: %+v %v", r, err)
	}
	if _, err := restarted.Propose(t.Context(), requestDevice, "req", "agent", "voice-session", text); err != nil {
		t.Fatal(err)
	}
	if len(e.n.events) != 1 {
		t.Fatal("恢复后重复推送")
	}
}

func TestRequestCancellationAndLateASR(t *testing.T) {
	e := newRequestEnv(t)
	e.prepare(t, "old")
	for range 2 {
		r, err := e.s.Cancel(t.Context(), requestDevice, "old")
		if err != nil || r.Status != "cancelled" {
			t.Fatalf("取消失败: %+v %v", r, err)
		}
	}
	e.prepare(t, "new")
	_, err := e.propose(t, "old", "迟到的语音")
	requestCode(t, err, domain.CodeConflict)
	r, err := e.s.Get(t.Context(), requestDevice, "new")
	if err != nil || r.Text != nil || r.Status != "awaiting_text" {
		t.Fatal("旧录音污染新请求")
	}
	if _, err := e.propose(t, "new", "你好"); err != nil {
		t.Fatal(err)
	}
	if _, err := e.s.Cancel(t.Context(), requestDevice, "new"); err != nil {
		t.Fatal(err)
	}
	_, err = e.propose(t, "new", "你好")
	requestCode(t, err, domain.CodeConflict)
}

func TestRequestExpirySurvivesRestart(t *testing.T) {
	for _, proposed := range []bool{false, true} {
		t.Run(map[bool]string{false: "awaiting_text", true: "awaiting_confirmation"}[proposed], func(t *testing.T) {
			e := newRequestEnv(t)
			e.prepare(t, "old")
			ttl := 120 * time.Second
			if proposed {
				if _, err := e.propose(t, "old", "最终文字"); err != nil {
					t.Fatal(err)
				}
				ttl = 300 * time.Second
			}
			e.now = e.now.Add(ttl)
			s := NewRequestService(storage.NewRepositories(e.db.DB()), e.s.cfg, e.n)
			s.now = func() time.Time { return e.now }
			if err := s.Expire(t.Context()); err != nil {
				t.Fatal(err)
			}
			r, err := s.Get(t.Context(), requestDevice, "old")
			if err != nil || r.Status != "failed" || r.Error == nil {
				t.Fatalf("未持久化过期: %+v %v", r, err)
			}
			_, err = s.Propose(t.Context(), requestDevice, "old", "agent", "voice-session", "最终文字")
			requestCode(t, err, domain.CodeConflict)
			_, err = s.Cancel(t.Context(), requestDevice, "old")
			requestCode(t, err, domain.CodeConflict)
			e.prepare(t, "new")
		})
	}
}

func TestRequestAccessAndValidation(t *testing.T) {
	e := newRequestEnv(t)
	_, err := e.s.Prepare(t.Context(), requestDevice, "", "session")
	requestCode(t, err, domain.CodeInvalidArgument)
	_, err = e.s.Prepare(t.Context(), "unbound", "req", "session")
	requestCode(t, err, domain.CodePermissionDenied)
	_, err = e.s.Prepare(t.Context(), requestDevice, "req", "missing")
	requestCode(t, err, domain.CodeNotFound)
	e.prepare(t, "req")
	_, err = e.s.Get(t.Context(), "AA:BB:CC:DD:EE:02", "req")
	requestCode(t, err, domain.CodePermissionDenied)
	_, err = e.s.Get(t.Context(), "unbound", "req")
	requestCode(t, err, domain.CodePermissionDenied)
	_, err = e.s.Get(t.Context(), requestDevice, "missing")
	requestCode(t, err, domain.CodeNotFound)
	for _, text := range []string{"", " \n\t", "\u3000", string([]byte{255}), strings.Repeat("a", 16001), strings.Repeat("中", 5462)} {
		_, err := e.propose(t, "req", text)
		requestCode(t, err, domain.CodeInvalidArgument)
	}
	for _, tuple := range [][3]string{{"", "agent", "voice"}, {"req", "", "voice"}, {"req", "agent", ""}} {
		_, err := e.s.Propose(t.Context(), requestDevice, tuple[0], tuple[1], tuple[2], "你好")
		requestCode(t, err, domain.CodeInvalidArgument)
	}
	e.s.cfg.Limits.DraftMaxCodepoints = 2
	_, err = e.propose(t, "req", "abc")
	requestCode(t, err, domain.CodeInvalidArgument)
	e.s.cfg.Limits.DraftMaxUTF8Bytes = 4
	_, err = e.propose(t, "req", "中文")
	requestCode(t, err, domain.CodeInvalidArgument)
}

func TestRequestTargetAuthorizationChange(t *testing.T) {
	for _, query := range []string{`DELETE FROM session_catalog`, `UPDATE session_catalog SET thread_id='new'`, `UPDATE session_catalog SET project_id='new'`} {
		t.Run(query, func(t *testing.T) {
			e := newRequestEnv(t)
			e.prepare(t, "req")
			if _, err := e.db.DB().Exec(query); err != nil {
				t.Fatal(err)
			}
			_, err := e.propose(t, "req", "你好")
			if err == nil {
				t.Fatal("目标失效后仍接收文字")
			}
		})
	}
}

func TestRequestConcurrentProposeAndCancel(t *testing.T) {
	e := newRequestEnv(t)
	e.prepare(t, "req")
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if i%2 == 0 {
				e.s.Propose(context.Background(), requestDevice, "req", "agent", "voice-session", "你好")
			} else {
				e.s.Cancel(context.Background(), requestDevice, "req")
			}
		}(i)
	}
	wg.Wait()
	r, err := e.s.Get(t.Context(), requestDevice, "req")
	if err != nil || r.Status != "cancelled" {
		t.Fatalf("取消竞态错误: %+v %v", r, err)
	}
	if len(e.n.events) == 0 || e.n.events[len(e.n.events)-1].request.Status != "cancelled" {
		t.Fatal("取消后出现旧状态通知")
	}
}

func TestRequestTerminalDuplicateAndStorageFailure(t *testing.T) {
	e := newRequestEnv(t)
	e.prepare(t, "req")
	if _, err := e.propose(t, "req", "你好"); err != nil {
		t.Fatal(err)
	}
	for _, state := range []string{"sending", "running", "completed", "failed", "unknown"} {
		if _, err := e.db.DB().Exec(`UPDATE requests SET status=?`, state); err != nil {
			t.Fatal(err)
		}
		if state == "failed" {
			if _, err := e.db.DB().Exec(`UPDATE requests SET error_code='DEPENDENCY_UNAVAILABLE',error_message='任务依赖不可用'`); err != nil {
				t.Fatal(err)
			}
		}
		r, err := e.propose(t, "req", "你好")
		if err != nil || r.Status != state {
			t.Fatalf("重复请求未返回原状态 %s: %+v %v", state, r, err)
		}
		_, err = e.s.Cancel(t.Context(), requestDevice, "req")
		requestCode(t, err, domain.CodeConflict)
	}
	if _, err := e.db.DB().Exec(`UPDATE requests SET text=NULL,status='failed'`); err != nil {
		t.Fatal(err)
	}
	_, err := e.propose(t, "req", "你好")
	requestCode(t, err, domain.CodeConflict)
	e.db.Close()
	if _, err := e.s.Prepare(t.Context(), requestDevice, "new", "session"); err == nil {
		t.Fatal("数据库故障被忽略")
	}
	if _, err := e.propose(t, "req", "你好"); err == nil {
		t.Fatal("数据库故障被忽略")
	}
	if err := e.s.Expire(t.Context()); err == nil {
		t.Fatal("数据库故障被忽略")
	}
}
