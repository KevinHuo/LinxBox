package contracts

import "testing"

func TestRejectInvalidUTF8BeforeJSONRepair(t *testing.T) {
	v, err := NewValidator()
	if err != nil {
		t.Fatal(err)
	}
	raw := []byte("{\"id\":\"\xff\",\"type\":\"device.hello\",\"payload\":{\"mac_address\":\"AA:BB:CC:DD:EE:01\"}}")
	if err := v.ValidateFirmwareIncoming(raw); err == nil {
		t.Fatal("不能把无效 UTF-8 静默替换为有效字符")
	}
}
