package contracts

import (
	"strings"
	"testing"
	"unicode/utf8"
)

func TestNewMessageID(t *testing.T) {
	id := NewMessageID("msg_")
	if !strings.HasPrefix(id, "msg_") {
		t.Fatalf("缺少前缀: %s", id)
	}
	if n := utf8.RuneCountInString(id); n < 1 || n > 128 {
		t.Fatalf("ID 长度不合法: %d", n)
	}
	// 两次生成不重复。
	if id == NewMessageID("msg_") {
		t.Fatal("消息 ID 不应重复")
	}
}

func TestProtocolVersion(t *testing.T) {
	if ProtocolVersion != "1.0" {
		t.Fatalf("协议版本错误: %s", ProtocolVersion)
	}
}
