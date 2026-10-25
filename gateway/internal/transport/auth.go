// Package transport 提供连接、认证、解包、大小限制、Schema 校验、发送与背压。
// 入口层不直接写数据库、不自行推进请求状态。
package transport

import (
	"net/http"
	"strings"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
)

// PrincipalKind 标识连接主体类型。
type PrincipalKind int

const (
	// PrincipalDevice 是固件设备。
	PrincipalDevice PrincipalKind = iota
	// PrincipalDesktop 是 Mac Desktop 客户端。
	PrincipalDesktop
)

// Principal 是认证后的连接主体。
type Principal struct {
	Kind      PrincipalKind
	DeviceID  domain.DeviceID        // 设备连接有效
	DesktopID domain.DesktopClientID // Desktop 连接有效
	Subject   domain.Subject
}

// Authenticator 按配置凭据精确匹配握手 token。
type Authenticator struct {
	deviceCreds  map[string]config.DeviceCredential
	desktopCreds map[string]config.DesktopCredential
}

// NewAuthenticator 构造认证器。
func NewAuthenticator(cfg *config.Config) *Authenticator {
	a := &Authenticator{
		deviceCreds:  map[string]config.DeviceCredential{},
		desktopCreds: map[string]config.DesktopCredential{},
	}
	for _, d := range cfg.Credentials.Devices {
		a.deviceCreds[d.Token] = d
	}
	for _, d := range cfg.Credentials.Desktops {
		a.desktopCreds[d.Token] = d
	}
	return a
}

// Authenticate 从握手请求解析并精确校验凭据，返回认证主体。
func (a *Authenticator) Authenticate(kind PrincipalKind, r *http.Request) (Principal, error) {
	token, ok := extractToken(r)
	if !ok {
		return Principal{}, domain.ErrUnauthenticated("缺少凭据")
	}
	switch kind {
	case PrincipalDevice:
		cred, ok := a.deviceCreds[token]
		if !ok {
			return Principal{}, domain.ErrUnauthenticated("设备凭据无效")
		}
		deviceID, err := domain.NormalizeMAC(cred.DeviceID)
		if err != nil {
			return Principal{}, domain.ErrInternal("设备凭据配置不合法")
		}
		return Principal{Kind: kind, DeviceID: deviceID, Subject: domain.Subject(cred.Subject)}, nil
	case PrincipalDesktop:
		cred, ok := a.desktopCreds[token]
		if !ok {
			return Principal{}, domain.ErrUnauthenticated("Desktop 凭据无效")
		}
		return Principal{
			Kind:      kind,
			DesktopID: domain.DesktopClientID(cred.ClientID),
			Subject:   domain.Subject(cred.Subject),
		}, nil
	default:
		return Principal{}, domain.ErrInternal("未知主体类型")
	}
}

// extractToken 解析完整子协议项（linxbox.v1, linxbox-token.<token>）或
// Authorization: Bearer <token>；使用精确匹配，不做字符串包含匹配，不回显 token。
func extractToken(r *http.Request) (string, bool) {
	protocols := parseSubprotocols(r.Header.Get("Sec-WebSocket-Protocol"))
	hasV1 := false
	token := ""
	for _, p := range protocols {
		switch {
		case p == "linxbox.v1":
			hasV1 = true
		case strings.HasPrefix(p, "linxbox-token."):
			t := strings.TrimPrefix(p, "linxbox-token.")
			if t == "" || strings.ContainsAny(t, " \t,") {
				return "", false
			}
			token = t
		}
	}
	if hasV1 && token != "" {
		return token, true
	}
	// 允许 Authorization 头（如固件接入或非浏览器客户端）。
	auth := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if len(auth) > len(prefix) && strings.EqualFold(auth[:len(prefix)], prefix) {
		t := strings.TrimSpace(auth[len(prefix):])
		if t != "" {
			return t, true
		}
	}
	return "", false
}

// parseSubprotocols 按逗号拆分并去除空白。
func parseSubprotocols(header string) []string {
	if header == "" {
		return nil
	}
	parts := strings.Split(header, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		if t := strings.TrimSpace(p); t != "" {
			out = append(out, t)
		}
	}
	return out
}
