package domain

import (
	"errors"
	"testing"
)

func TestNormalizeMAC(t *testing.T) {
	tests := []struct {
		name    string
		in      string
		want    string
		wantErr bool
	}{
		{"连续大写", "AABBCCDDEE01", "AA:BB:CC:DD:EE:01", false},
		{"连续小写", "aabbccddee01", "AA:BB:CC:DD:EE:01", false},
		{"冒号分隔", "aa:bb:cc:dd:ee:01", "AA:BB:CC:DD:EE:01", false},
		{"横线分隔", "aa-bb-cc-dd-ee-01", "AA:BB:CC:DD:EE:01", false},
		{"太短", "AABBCCDDEE0", "", true},
		{"非法字符", "GG:BB:CC:DD:EE:01", "", true},
		{"混合分隔", "AA-BB:CC-DD:EE-01", "", true},
		{"空串", "", "", true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := NormalizeMAC(tt.in)
			if tt.wantErr {
				if err == nil {
					t.Fatalf("期望错误，得到 %q", got)
				}
				if !errors.Is(err, NewError(CodeInvalidArgument, "")) {
					t.Fatalf("错误码应为 INVALID_ARGUMENT，得到 %v", err)
				}
				return
			}
			if err != nil {
				t.Fatalf("不应报错: %v", err)
			}
			if string(got) != tt.want {
				t.Fatalf("得到 %q，期望 %q", got, tt.want)
			}
		})
	}
}

func TestIsDeviceID(t *testing.T) {
	if !IsDeviceID("AA:BB:CC:DD:EE:01") {
		t.Fatal("合法设备标识应通过")
	}
	if IsDeviceID("aa:bb:cc:dd:ee:01") {
		t.Fatal("小写格式不是规范化标识")
	}
	if IsDeviceID("not-a-mac") {
		t.Fatal("非法标识不应通过")
	}
}

func TestValidateID(t *testing.T) {
	if !ValidateID("abc") {
		t.Fatal("1～128 字符应通过")
	}
	if ValidateID("") {
		t.Fatal("空串不应通过")
	}
	long := ""
	for i := 0; i < 129; i++ {
		long += "a"
	}
	if ValidateID(long) {
		t.Fatal("超过 128 字符不应通过")
	}
	if !ValidateID("中文字符") {
		t.Fatal("Unicode 码点计长应通过")
	}
}

func TestValidateSubject(t *testing.T) {
	if !ValidateSubject("alice") {
		t.Fatal("非空主体应通过")
	}
	if ValidateSubject("") {
		t.Fatal("空主体不应通过")
	}
}
