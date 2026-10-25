package service

import (
	"encoding/base64"
	"errors"
	"slices"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func record(id string) contracts.SessionRecord {
	return contracts.SessionRecord{
		AgentSessionID: id,
		Title:          "会话 " + id,
		ProjectName:    "proj",
		Agent:          "codex",
		Status:         "idle",
		Summary:        "",
		ThreadID:       "thread-" + id,
		ProjectID:      "prj",
	}
}

func TestSyncAndList(t *testing.T) {
	env := newServiceEnv(t)
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:01"); err != nil {
		t.Fatal(err)
	}
	// 同步 6 条。
	records := make([]contracts.SessionRecord, 0, 6)
	for i := 1; i <= 6; i++ {
		records = append(records, record("sess-0"+string(rune('0'+i))))
	}
	v, err := env.sessions.Sync(env.ctx, "desktop-1", records)
	if err != nil {
		t.Fatal(err)
	}
	if v != 1 {
		t.Fatalf("版本应为 1: %d", v)
	}
	if env.notify.updateCount() != 1 {
		t.Fatal("同步应推送目录更新")
	}
	// 分页 5+1。
	page1, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", "")
	if err != nil {
		t.Fatal(err)
	}
	if len(page1.Items) != 5 || page1.NextCursor == nil {
		t.Fatalf("第一页错误: %d 项, next=%v", len(page1.Items), page1.NextCursor)
	}
	page2, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", *page1.NextCursor)
	if err != nil {
		t.Fatal(err)
	}
	if len(page2.Items) != 1 || page2.NextCursor != nil {
		t.Fatalf("第二页错误: %d 项, next=%v", len(page2.Items), page2.NextCursor)
	}
	// 重新同步（目录更新）后旧 cursor 作废。
	if _, err := env.sessions.Sync(env.ctx, "desktop-1", records[:2]); err != nil {
		t.Fatal(err)
	}
	if _, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", *page1.NextCursor); err == nil {
		t.Fatal("旧 cursor 应作废")
	}
	// 非法 cursor。
	if _, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", "not-a-cursor"); err == nil {
		t.Fatal("非法 cursor 应拒绝")
	}
	if _, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", "aGVsbG8="); err == nil {
		t.Fatal("负偏移 cursor 应拒绝")
	}
}

func TestSyncPreservesOrderAndInvalidatesReorderedCursor(t *testing.T) {
	env := newServiceEnv(t)
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "AA:BB:CC:DD:EE:01"); err != nil {
		t.Fatal(err)
	}
	want := []string{"z", "a", "m", "b", "y", "c"}
	records := make([]contracts.SessionRecord, 0, len(want))
	for _, id := range want {
		records = append(records, record(id))
	}
	if _, err := env.sessions.Sync(env.ctx, "desktop-1", records); err != nil {
		t.Fatal(err)
	}
	first, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", "")
	if err != nil || first.NextCursor == nil {
		t.Fatalf("第一页应有续页：%+v %v", first, err)
	}
	second, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", *first.NextCursor)
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, item := range append(first.Items, second.Items...) {
		got = append(got, item.AgentSessionID)
	}
	if !slices.Equal(got, want) || second.NextCursor != nil {
		t.Fatalf("分页没有保持上传顺序：%v", got)
	}
	// 仅重新排列相同记录，也必须更新目录版本并使旧 cursor 失效。
	slices.Reverse(records)
	if version, err := env.sessions.Sync(env.ctx, "desktop-1", records); err != nil || version != 2 {
		t.Fatalf("重排版本错误：%d %v", version, err)
	}
	if _, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", *first.NextCursor); !errors.Is(err, domain.ErrConflict("x")) {
		t.Fatalf("重排后旧 cursor 应失效：%v", err)
	}
	// 被拒绝的同步不改变刚保存的顺序。
	if _, err := env.sessions.Sync(env.ctx, "desktop-1", []contracts.SessionRecord{record("a"), record("a")}); err == nil {
		t.Fatal("重复 ID 应拒绝")
	}
	reordered, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", "")
	if err != nil || len(reordered.Items) != 5 {
		t.Fatalf("重排后的第一页错误：%+v %v", reordered, err)
	}
	for i, item := range reordered.Items {
		if item.AgentSessionID != records[i].AgentSessionID {
			t.Fatalf("重排顺序错误：第 %d 项为 %s", i, item.AgentSessionID)
		}
	}
}

func TestSyncValidation(t *testing.T) {
	env := newServiceEnv(t)
	// 重复 agent_session_id 整批拒绝。
	dup := []contracts.SessionRecord{record("s1"), record("s1")}
	if _, err := env.sessions.Sync(env.ctx, "desktop-1", dup); err == nil {
		t.Fatal("重复会话应整批拒绝")
	}
	// 超上限整批拒绝。
	env.cfg.Limits.MaxCatalogItems = 2
	over := []contracts.SessionRecord{record("s1"), record("s2"), record("s3")}
	if _, err := env.sessions.Sync(env.ctx, "desktop-1", over); err == nil {
		t.Fatal("超上限应整批拒绝")
	}
}

func TestListAndGetUnauthorized(t *testing.T) {
	env := newServiceEnv(t)
	// 未绑定设备读取目录。
	if _, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", ""); err == nil {
		t.Fatal("未绑定设备不应读取目录")
	}
	if _, err := env.sessions.Get(env.ctx, "AA:BB:CC:DD:EE:01", "s1"); err == nil {
		t.Fatal("未绑定设备不应读取会话")
	}
	// 绑定后未同步：目录为空。
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:01"); err != nil {
		t.Fatal(err)
	}
	page, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", "")
	if err != nil {
		t.Fatal(err)
	}
	if len(page.Items) != 0 || page.NextCursor != nil {
		t.Fatalf("空目录错误: %+v", page)
	}
	// Get 缺失会话。
	if _, err := env.sessions.Get(env.ctx, "AA:BB:CC:DD:EE:01", "missing"); err == nil {
		t.Fatal("缺失会话应返回 NOT_FOUND")
	} else if !errors.Is(err, domain.ErrNotFound("x")) {
		t.Fatalf("错误码错误: %v", err)
	}
}

func TestSyncNotifiesBoundDevices(t *testing.T) {
	env := newServiceEnv(t)
	// 两个设备绑定同一 Desktop，同步应推送两个设备。
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:01"); err != nil {
		t.Fatal(err)
	}
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:02"); err != nil {
		t.Fatal(err)
	}
	env.notify.boundCount() // 消耗绑定推送计数
	if _, err := env.sessions.Sync(env.ctx, "desktop-1", []contracts.SessionRecord{record("s1")}); err != nil {
		t.Fatal(err)
	}
	if env.notify.updateCount() != 2 {
		t.Fatalf("应推送两个设备: %d", env.notify.updateCount())
	}
}

func TestGetSessionProjection(t *testing.T) {
	env := newServiceEnv(t)
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:01"); err != nil {
		t.Fatal(err)
	}
	if _, err := env.sessions.Sync(env.ctx, "desktop-1", []contracts.SessionRecord{record("s1")}); err != nil {
		t.Fatal(err)
	}
	got, err := env.sessions.Get(env.ctx, "AA:BB:CC:DD:EE:01", "s1")
	if err != nil {
		t.Fatal(err)
	}
	if got.AgentSessionID != "s1" || got.Title != "会话 s1" {
		t.Fatalf("会话投影错误: %+v", got)
	}
}

func TestListNegativeCursor(t *testing.T) {
	env := newServiceEnv(t)
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:01"); err != nil {
		t.Fatal(err)
	}
	if _, err := env.sessions.Sync(env.ctx, "desktop-1", []contracts.SessionRecord{record("s1")}); err != nil {
		t.Fatal(err)
	}
	for _, c := range []string{
		base64.RawURLEncoding.EncodeToString([]byte(`{"v":-1,"o":0}`)),
		base64.RawURLEncoding.EncodeToString([]byte(`{"v":1,"o":-2}`)),
	} {
		if _, err := env.sessions.List(env.ctx, "AA:BB:CC:DD:EE:01", c); err == nil {
			t.Fatal("负值 cursor 应报错")
		}
	}
}
