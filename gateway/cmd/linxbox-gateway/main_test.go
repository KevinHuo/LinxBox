package main

import (
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func TestRunHealthcheck(t *testing.T) {
	for _, tc := range []struct {
		name   string
		status int
		want   int
	}{
		{"ready", http.StatusOK, 0},
		{"not-ready", http.StatusServiceUnavailable, 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path != "/readyz" {
					t.Errorf("健康检查路径错误: %s", r.URL.Path)
				}
				w.WriteHeader(tc.status)
			}))
			defer server.Close()
			if got := run([]string{"--healthcheck", server.URL + "/readyz"}); got != tc.want {
				t.Fatalf("健康检查退出码 = %d，期望 %d", got, tc.want)
			}
		})
	}
	t.Run("unreachable", func(t *testing.T) {
		server := httptest.NewServer(http.NotFoundHandler())
		server.Close()
		if got := run([]string{"--healthcheck", server.URL + "/readyz"}); got != 1 {
			t.Fatalf("不可达健康检查应失败，得到 %d", got)
		}
	})
}

// TestRunMissingFlag 无 --config 参数时应返回 2。
func TestRunMissingFlag(t *testing.T) {
	if code := run([]string{}); code != 2 {
		t.Fatalf("缺少配置参数应返回 2: %d", code)
	}
}

// TestRunParseError 非法参数应返回 2。
func TestRunParseError(t *testing.T) {
	if code := run([]string{"--nope"}); code != 2 {
		t.Fatalf("非法参数应返回 2: %d", code)
	}
}

// TestRunBadConfig 配置缺失时应返回 1。
func TestRunBadConfig(t *testing.T) {
	if code := run([]string{"--config", filepath.Join(t.TempDir(), "nope.json")}); code != 1 {
		t.Fatalf("配置加载失败应返回 1: %d", code)
	}
}

// TestRunInvalidConfig 配置内容非法时应返回 1。
func TestRunInvalidConfig(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bad.json")
	if err := os.WriteFile(path, []byte("{"), 0o600); err != nil {
		t.Fatal(err)
	}
	if code := run([]string{"--config", path}); code != 1 {
		t.Fatalf("非法配置应返回 1: %d", code)
	}
}

// TestRunFullLifecycle 启动完整服务、等待就绪后以信号优雅退出，返回 0。
func TestRunFullLifecycle(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	_ = ln.Close()

	cfg := map[string]any{
		"database": map[string]any{"path": filepath.Join(t.TempDir(), "gw.db")},
		"listen":   map[string]any{"addr": "127.0.0.1:" + itoa(port), "tls_cert_file": "", "tls_key_file": ""},
		"credentials": map[string]any{
			"devices":  []map[string]any{{"token": "t1", "device_id": "AA:BB:CC:DD:EE:01", "subject": "alice"}},
			"desktops": []map[string]any{{"token": "t2", "client_id": "d1", "subject": "alice"}},
		},
		"pairing": map[string]any{"rules": []map[string]any{{"subject": "alice", "device_ids": []string{"AA:BB:CC:DD:EE:01"}}}},
	}
	path := filepath.Join(t.TempDir(), "gw.json")
	raw, err := jsonMarshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatal(err)
	}

	done := make(chan int, 1)
	go func() { done <- run([]string{"--config", path}) }()
	// 等待健康检查通过。
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		resp, err := http.Get("http://127.0.0.1:" + itoa(port) + "/healthz")
		if err == nil {
			resp.Body.Close()
			if resp.StatusCode == 200 {
				break
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
	// 触发优雅退出。
	if err := syscall.Kill(os.Getpid(), syscall.SIGINT); err != nil {
		t.Fatal(err)
	}
	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("优雅退出应返回 0: %d", code)
		}
	case <-time.After(15 * time.Second):
		t.Fatal("服务未按时退出")
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b []byte
	for n > 0 {
		b = append([]byte{byte('0' + n%10)}, b...)
		n /= 10
	}
	return string(b)
}

func jsonMarshal(v any) ([]byte, error) { return json.Marshal(v) }

// TestRunAppNewFailure 配置可加载但应用初始化失败时应返回 1。
func TestRunAppNewFailure(t *testing.T) {
	cfg := map[string]any{
		"database": map[string]any{"path": filepath.Join(t.TempDir(), "no-dir", "gw.db")},
		"listen":   map[string]any{"addr": "127.0.0.1:0"},
		"credentials": map[string]any{
			"devices":  []map[string]any{{"token": "t1", "device_id": "AA:BB:CC:DD:EE:01", "subject": "alice"}},
			"desktops": []map[string]any{{"token": "t2", "client_id": "d1", "subject": "alice"}},
		},
		"pairing": map[string]any{"rules": []map[string]any{{"subject": "alice", "device_ids": []string{"AA:BB:CC:DD:EE:01"}}}},
	}
	path := filepath.Join(t.TempDir(), "gw.json")
	raw, err := jsonMarshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	if code := run([]string{"--config", path}); code != 1 {
		t.Fatalf("初始化失败应返回 1: %d", code)
	}
}

// TestRunServerFailure 端口被占用导致服务启动失败时应返回 1。
func TestRunServerFailure(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	port := ln.Addr().(*net.TCPAddr).Port
	cfg := map[string]any{
		"database": map[string]any{"path": filepath.Join(t.TempDir(), "gw.db")},
		"listen":   map[string]any{"addr": "127.0.0.1:" + itoa(port)},
		"credentials": map[string]any{
			"devices":  []map[string]any{{"token": "t1", "device_id": "AA:BB:CC:DD:EE:01", "subject": "alice"}},
			"desktops": []map[string]any{{"token": "t2", "client_id": "d1", "subject": "alice"}},
		},
		"pairing": map[string]any{"rules": []map[string]any{{"subject": "alice", "device_ids": []string{"AA:BB:CC:DD:EE:01"}}}},
	}
	path := filepath.Join(t.TempDir(), "gw.json")
	raw, err := jsonMarshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	if code := run([]string{"--config", path}); code != 1 {
		t.Fatalf("服务启动失败应返回 1: %d", code)
	}
}
