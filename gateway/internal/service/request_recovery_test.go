package service

import (
	"testing"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func TestRequestRecoveryMissingDoesNotCreateOrRedispatch(t *testing.T) {
	e, d := newExecutionEnv(t)
	confirmExecution(t, e)
	for range 2 {
		_, err := e.s.Get(t.Context(), requestDevice, "missing")
		requestCode(t, err, domain.CodeNotFound)
		if domain.MessageOf(err) != domain.RequestRecoveryMessage {
			t.Fatal(err)
		}
	}
	var count int
	if err := e.db.DB().QueryRow(`SELECT COUNT(*) FROM requests`).Scan(&count); err != nil || count != 1 || d.sends != 1 {
		t.Fatal("查询缺失请求不能创建或重投", count, d.sends, err)
	}
	_, err := e.s.Get(t.Context(), "AA:BB:CC:DD:EE:02", "req")
	requestCode(t, err, domain.CodePermissionDenied)
	requestStatus(t, e, "sending")
}

func TestRequestRecoveryExpiryRetainsFailedSnapshot(t *testing.T) {
	e := newRequestEnv(t)
	e.prepare(t, "req")
	e.now = e.now.Add(121 * time.Second)
	r, err := e.s.Get(t.Context(), requestDevice, "req")
	if err != nil || r.Status != "failed" || r.Error == nil || r.Error.Message != "未收到文字" {
		t.Fatal("超时不是记录缺失", r, err)
	}
}
