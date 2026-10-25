package service

import (
	"context"
	"log/slog"
	"path/filepath"
	"sync"
	"testing"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

// fakeNotifier 记录推送，供服务层测试断言。
type fakeNotifier struct {
	mu      sync.Mutex
	online  map[string]bool
	bound   []contracts.BoundBinding
	updates []string
}

func (f *fakeNotifier) DeviceOnline(deviceID string) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.online[deviceID]
}

func (f *fakeNotifier) NotifyDeviceBound(deviceID string, binding contracts.BoundBinding) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.bound = append(f.bound, binding)
}

func (f *fakeNotifier) NotifyCatalogUpdated(deviceID string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.updates = append(f.updates, deviceID)
}

func (f *fakeNotifier) boundCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.bound)
}

func (f *fakeNotifier) updateCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.updates)
}

// serviceEnv 组装真实存储与业务服务。
type serviceEnv struct {
	cfg      *config.Config
	binding  *BindingService
	sessions *SessionService
	notify   *fakeNotifier
	ctx      context.Context
}

func newServiceEnv(t *testing.T) *serviceEnv {
	t.Helper()
	cfg := &config.Config{
		Database: config.DatabaseConfig{Path: filepath.Join(t.TempDir(), "gw.db")},
		Credentials: config.CredentialsConfig{
			Devices: []config.DeviceCredential{
				{Token: "dev-token", DeviceID: "AA:BB:CC:DD:EE:01", Subject: "alice"},
				{Token: "dev2-token", DeviceID: "AA:BB:CC:DD:EE:02", Subject: "alice"},
			},
			Desktops: []config.DesktopCredential{
				{Token: "dt-token", ClientID: "desktop-1", Subject: "alice"},
				{Token: "dt2-token", ClientID: "desktop-2", Subject: "alice"},
				{Token: "bob-token", ClientID: "desktop-bob", Subject: "bob"},
			},
		},
		Pairing: config.PairingConfig{Rules: []config.PairingRule{
			{Subject: "alice", DeviceIDs: []string{"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"}},
			{Subject: "bob", DeviceIDs: []string{"AA:BB:CC:DD:EE:01"}},
		}},
		Limits: config.LimitsConfig{MaxCatalogItems: 200, SessionPageSize: 5},
	}
	db, err := storage.Open(cfg.Database.Path)
	if err != nil {
		t.Fatalf("打开数据库失败: %v", err)
	}
	t.Cleanup(func() { _ = db.Close() })
	repos := storage.NewRepositories(db.DB())
	notify := &fakeNotifier{online: map[string]bool{"AA:BB:CC:DD:EE:01": true, "AA:BB:CC:DD:EE:02": true}}
	logger := slog.New(slog.DiscardHandler)
	binding := NewBindingService(repos, cfg, notify, logger)
	for _, credential := range cfg.Credentials.Devices {
		if _, err := binding.RegisterDevice(t.Context(), domain.Subject(credential.Subject), credential.DeviceID, config.DeviceCredentialKey(credential.Token)); err != nil {
			t.Fatal(err)
		}
	}
	return &serviceEnv{
		cfg:      cfg,
		binding:  binding,
		sessions: NewSessionService(repos, cfg, notify, logger),
		notify:   notify,
		ctx:      t.Context(),
	}
}

func TestRegistrationStorageFailure(t *testing.T) {
	env := newServiceEnv(t)
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "AA:BB:CC:DD:EE:01"); err != nil {
		t.Fatal(err)
	}
	if _, err := env.binding.repos.DB().Exec(`DROP TABLE device_registrations`); err != nil {
		t.Fatal(err)
	}
	if _, err := env.binding.RegisterDevice(env.ctx, "alice", "AA:BB:CC:DD:EE:01", config.DeviceCredentialKey("dev-token")); err == nil {
		t.Fatal("登记存储失效不能返回成功")
	}
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "AA:BB:CC:DD:EE:01"); err == nil {
		t.Fatal("登记存储失效不能继续绑定")
	}
	if _, err := authorizedDevices(env.ctx, env.binding.repos, env.cfg, "desktop-1"); err == nil {
		t.Fatal("通知授权查询失效必须返回错误")
	}
}

func TestBindFlow(t *testing.T) {
	env := newServiceEnv(t)
	// 首次绑定。
	b, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:01")
	if err != nil {
		t.Fatal(err)
	}
	if b.DesktopClientID != "desktop-1" {
		t.Fatalf("绑定 Desktop 错误: %s", b.DesktopClientID)
	}
	if env.notify.boundCount() != 1 {
		t.Fatal("绑定成功应推送 device.bound")
	}
	// 同 Desktop 重复绑定幂等，不重复推送。
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:01"); err != nil {
		t.Fatal(err)
	}
	if env.notify.boundCount() != 1 {
		t.Fatal("幂等绑定不应重复推送")
	}
	// 其他 Desktop 绑定冲突。
	if _, err := env.binding.Bind(env.ctx, "desktop-2", "alice", "aa:bb:cc:dd:ee:01"); err == nil {
		t.Fatal("应返回 CONFLICT")
	} else if domain.CodeOf(err) != domain.CodeConflict {
		t.Fatalf("错误码应为 CONFLICT: %v", err)
	}
	// 无配对权限（bob 无该设备）。
	if _, err := env.binding.Bind(env.ctx, "desktop-bob", "bob", "aa:bb:cc:dd:ee:02"); err == nil {
		t.Fatal("应返回 PERMISSION_DENIED")
	}
	// 主体不一致：bob 绑定 desktop-1 的设备。
	if _, err := env.binding.Bind(env.ctx, "desktop-bob", "bob", "aa:bb:cc:dd:ee:01"); err == nil {
		t.Fatal("设备与 Desktop 主体不一致应拒绝")
	}
	// 非法 MAC。
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "nope"); err == nil {
		t.Fatal("非法 MAC 应拒绝")
	}
}

func TestHello(t *testing.T) {
	env := newServiceEnv(t)
	// 未绑定：desktop_client_id 为 null。
	b, err := env.binding.Hello(env.ctx, "AA:BB:CC:DD:EE:01", "aa:bb:cc:dd:ee:01")
	if err != nil {
		t.Fatal(err)
	}
	if b.DesktopClientID != nil {
		t.Fatalf("未绑定应为 null: %v", *b.DesktopClientID)
	}
	// 绑定后 hello 返回 Desktop。
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", "aa:bb:cc:dd:ee:01"); err != nil {
		t.Fatal(err)
	}
	b, err = env.binding.Hello(env.ctx, "AA:BB:CC:DD:EE:01", "aa:bb:cc:dd:ee:01")
	if err != nil {
		t.Fatal(err)
	}
	if b.DesktopClientID == nil || *b.DesktopClientID != "desktop-1" {
		t.Fatalf("绑定后 hello 应返回 Desktop: %+v", b)
	}
	// MAC 与认证设备不一致。
	if _, err := env.binding.Hello(env.ctx, "AA:BB:CC:DD:EE:01", "aa:bb:cc:dd:ee:02"); err == nil {
		t.Fatal("MAC 不一致应拒绝")
	}
	// 非法 MAC。
	if _, err := env.binding.Hello(env.ctx, "AA:BB:CC:DD:EE:01", "bad"); err == nil {
		t.Fatal("非法 MAC 应拒绝")
	}
}

func TestRegisterWithoutConfiguredMAC(t *testing.T) {
	env := newServiceEnv(t)
	const mac = "34:00:00:00:00:99"
	env.cfg.Credentials.Devices = []config.DeviceCredential{{Token: "access", Subject: "alice"}}
	env.cfg.Pairing.Rules = nil
	binding, err := env.binding.RegisterDevice(env.ctx, "alice", mac, config.DeviceCredentialKey("access"))
	if err != nil || binding.DeviceID != mac || binding.DesktopClientID != nil {
		t.Fatalf("动态登记失败: %+v %v", binding, err)
	}
	if _, err := env.binding.RegisterDevice(env.ctx, "alice", mac, config.DeviceCredentialKey("access")); err != nil {
		t.Fatal("同账号重复 hello 应幂等", err)
	}
	env.cfg.Credentials.Devices = append(env.cfg.Credentials.Devices, config.DeviceCredential{Token: "bob", Subject: "bob"})
	if _, err := env.binding.RegisterDevice(env.ctx, "bob", mac, config.DeviceCredentialKey("bob")); domain.CodeOf(err) != domain.CodePermissionDenied {
		t.Fatal("其他账号不能接管已登记 MAC", err)
	}
	if _, err := env.binding.RegisterDevice(env.ctx, "unknown", mac, config.DeviceCredentialKey("unknown")); domain.CodeOf(err) != domain.CodePermissionDenied {
		t.Fatal("失效账号不能登记设备", err)
	}
	if _, err := env.binding.RegisterDevice(env.ctx, "alice", "invalid", config.DeviceCredentialKey("access")); domain.CodeOf(err) != domain.CodeInvalidArgument {
		t.Fatal("非法 MAC 应拒绝", err)
	}
	env.notify.online[mac] = true
	if _, err := env.binding.Bind(env.ctx, "desktop-1", "alice", mac); err != nil {
		t.Fatal("无需 MAC 白名单即可绑定同账号在线设备", err)
	}
}
