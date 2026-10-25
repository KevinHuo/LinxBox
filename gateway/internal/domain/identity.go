// Package domain 提供状态转换、固定目标、幂等与归属规则；
// 不依赖 socket 或 SQL。
package domain

import (
	"regexp"
	"strings"
	"unicode/utf8"
)

// DeviceID 是规范化后的大写冒号 MAC 格式设备标识（如 AA:BB:CC:DD:EE:FF）。
type DeviceID string

// DesktopClientID 是已认证的 Mac 客户端标识。
type DesktopClientID string

// Subject 是账号主体标识，绑定与会话查找始终带主体过滤。
type Subject string

var (
	macPattern    = regexp.MustCompile(`^(?:[0-9A-Fa-f]{12}|(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}|(?:[0-9A-Fa-f]{2}-){5}[0-9A-Fa-f]{2})$`)
	devicePattern = regexp.MustCompile(`^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$`)
)

// NormalizeMAC 校验并规范化 MAC：接受连续 12 位十六进制、统一冒号或统一横线
// 格式，输出大写冒号格式的 DeviceID。
func NormalizeMAC(raw string) (DeviceID, error) {
	if !macPattern.MatchString(raw) {
		return "", ErrInvalidArgument("MAC 格式不合法")
	}
	cleaned := strings.NewReplacer(":", "", "-", "").Replace(raw)
	cleaned = strings.ToUpper(cleaned)
	var sb strings.Builder
	for i := 0; i < 12; i += 2 {
		if i > 0 {
			sb.WriteByte(':')
		}
		sb.WriteString(cleaned[i : i+2])
	}
	return DeviceID(sb.String()), nil
}

// IsDeviceID 判断是否为规范化的设备标识。
func IsDeviceID(s string) bool {
	return devicePattern.MatchString(s)
}

// ValidateID 校验 Id 约束（1～128 个 Unicode 码点）。
func ValidateID(s string) bool {
	n := utf8.RuneCountInString(s)
	return n >= 1 && n <= 128
}

// ValidateSubject 校验账号主体非空。
func ValidateSubject(s string) bool {
	return s != ""
}
