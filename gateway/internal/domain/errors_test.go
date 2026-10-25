package domain

import (
	"errors"
	"fmt"
	"testing"
)

func TestErrorConstruction(t *testing.T) {
	e := ErrNotFound("会话不存在")
	if e.Code != CodeNotFound {
		t.Fatalf("错误码错误: %v", e.Code)
	}
	if e.Error() != "NOT_FOUND: 会话不存在" {
		t.Fatalf("Error() 输出错误: %s", e.Error())
	}
	if !errors.Is(e, NewError(CodeNotFound, "")) {
		t.Fatal("同码错误应可匹配")
	}
	if errors.Is(e, NewError(CodeConflict, "")) {
		t.Fatal("不同码错误不应匹配")
	}
}

func TestWrapError(t *testing.T) {
	cause := fmt.Errorf("底层失败")
	e := WrapError(CodeInternal, "内部错误", cause)
	if !errors.Is(e, cause) {
		t.Fatal("应可解包底层错误")
	}
	if e.Error() == "" {
		t.Fatal("Error() 不应为空")
	}
}

func TestCodeOf(t *testing.T) {
	if CodeOf(ErrConflict("x")) != CodeConflict {
		t.Fatal("业务错误应返回自身码")
	}
	if CodeOf(fmt.Errorf("未知")) != CodeInternal {
		t.Fatal("未知错误应视为 INTERNAL")
	}
	var nilErr *Error
	if CodeOf(nilErr) != CodeInternal {
		t.Fatal("nil 错误应视为 INTERNAL")
	}
}

func TestMessageOf(t *testing.T) {
	if MessageOf(ErrPermissionDenied("无权")) != "无权" {
		t.Fatal("应返回业务消息")
	}
	if MessageOf(fmt.Errorf("raw")) != "内部错误" {
		t.Fatal("未知错误应返回默认文案")
	}
}

func TestAllErrorCodes(t *testing.T) {
	constructors := []struct {
		code Code
		fn   func(string) *Error
	}{
		{CodeInvalidArgument, ErrInvalidArgument},
		{CodeUnauthenticated, ErrUnauthenticated},
		{CodePermissionDenied, ErrPermissionDenied},
		{CodeNotFound, ErrNotFound},
		{CodeConflict, ErrConflict},
		{CodeDependencyUnavailable, ErrDependencyUnavailable},
		{CodeResourceExhausted, ErrResourceExhausted},
		{CodeInternal, ErrInternal},
	}
	for _, c := range constructors {
		e := c.fn("m")
		if e.Code != c.code {
			t.Fatalf("%s 构造错误", c.code)
		}
		if e.Message != "m" {
			t.Fatalf("%s 消息错误", c.code)
		}
	}
}

func TestCodeOfNil(t *testing.T) {
	if got := CodeOf(nil); got != CodeInternal {
		t.Fatalf("nil 应视为 INTERNAL: %v", got)
	}
}

func TestMessageOfNil(t *testing.T) {
	if got := MessageOf(nil); got != "内部错误" {
		t.Fatalf("nil 应使用默认文案: %q", got)
	}
}
