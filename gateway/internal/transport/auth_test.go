package transport

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

func TestExtractToken(t *testing.T) {
	req := func(proto, auth string) *http.Request {
		r := httptest.NewRequest(http.MethodGet, "/v1/devices/connect", nil)
		if proto != "" {
			r.Header.Set("Sec-WebSocket-Protocol", proto)
		}
		if auth != "" {
			r.Header.Set("Authorization", auth)
		}
		return r
	}
	tests := []struct {
		name   string
		proto  string
		auth   string
		want   string
		wantOK bool
	}{
		{"完整子协议", "linxbox.v1, linxbox-token.abc123", "", "abc123", true},
		{"子协议带空白", " linxbox.v1 , linxbox-token.abc123 ", "", "abc123", true},
		{"缺版本标记", "linxbox-token.abc123", "", "", false},
		{"缺 token", "linxbox.v1", "", "", false},
		{"空 token", "linxbox.v1, linxbox-token.", "", "", false},
		// token 含逗号会被子协议拆分机制切分，这里按实现行为断言为拆分后的首段。
		{"token 含逗号", "linxbox.v1, linxbox-token.a,b", "", "a", true},
		{"Bearer 兜底", "", "Bearer abc123", "abc123", true},
		{"Bearer 大小写", "", "bearer abc123", "abc123", true},
		{"Bearer 空", "", "Bearer ", "", false},
		{"Bearer 非 Bearer", "", "Basic abc", "", false},
		{"两者都无", "", "", "", false},
		{"子协议优先", "linxbox.v1, linxbox-token.proto", "Bearer auth", "proto", true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, ok := extractToken(req(tt.proto, tt.auth))
			if ok != tt.wantOK || got != tt.want {
				t.Fatalf("得到 %q/%v，期望 %q/%v", got, ok, tt.want, tt.wantOK)
			}
		})
	}
}

func TestParseSubprotocols(t *testing.T) {
	if got := parseSubprotocols("a, b ,c"); len(got) != 3 || got[0] != "a" || got[1] != "b" || got[2] != "c" {
		t.Fatalf("解析错误: %v", got)
	}
	if got := parseSubprotocols(""); got != nil {
		t.Fatalf("空头应返回 nil: %v", got)
	}
}

func TestAuthenticator(t *testing.T) {
	cfg := testServerConfig(t)
	auth := NewAuthenticator(cfg)

	// 设备：合法 token。
	r := httptest.NewRequest(http.MethodGet, "/v1/devices/connect", nil)
	r.Header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.dev-token")
	p, err := auth.Authenticate(PrincipalDevice, r)
	if err != nil {
		t.Fatal(err)
	}
	if p.DeviceID != "AA:BB:CC:DD:EE:01" || p.Subject != "alice" {
		t.Fatalf("设备主体错误: %+v", p)
	}
	// 设备：错误 token。
	r.Header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.wrong")
	if _, err := auth.Authenticate(PrincipalDevice, r); err == nil {
		t.Fatal("错误 token 应拒绝")
	}
	// Desktop：合法 token。
	r = httptest.NewRequest(http.MethodGet, "/v1/clients/connect", nil)
	r.Header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.dt-token")
	p, err = auth.Authenticate(PrincipalDesktop, r)
	if err != nil {
		t.Fatal(err)
	}
	if p.DesktopID != "desktop-1" {
		t.Fatalf("Desktop 主体错误: %+v", p)
	}
	// 类型错配：Desktop token 用于设备。
	r = httptest.NewRequest(http.MethodGet, "/v1/devices/connect", nil)
	r.Header.Set("Sec-WebSocket-Protocol", "linxbox.v1, linxbox-token.dt-token")
	if _, err := auth.Authenticate(PrincipalDevice, r); err == nil {
		t.Fatal("Desktop token 不应通过设备认证")
	}
	// 缺少凭据。
	r = httptest.NewRequest(http.MethodGet, "/v1/devices/connect", nil)
	if _, err := auth.Authenticate(PrincipalDevice, r); err == nil {
		t.Fatal("缺少凭据应拒绝")
	}
	// 未知主体类型。
	if _, err := auth.Authenticate(PrincipalKind(99), r); err == nil {
		t.Fatal("未知主体类型应报错")
	}
}

func TestToWireError(t *testing.T) {
	e := toWireError(domain.ErrInvalidArgument("x"))
	if e.Code != "INVALID_ARGUMENT" || e.Message == "" {
		t.Fatalf("错误映射错误: %+v", e)
	}
}
