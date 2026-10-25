// Package service 组织绑定、目录等业务事务；入口层不直接写数据库，
// 执行事件也统一走服务层，避免两套状态逻辑。
package service

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

// Notifier 在事务提交后通知在线对端；由传输层实现。
// 推送失败不回滚已保存事实，因此通知方法不返回错误。
type Notifier interface {
	DeviceOnline(deviceID string) bool
	NotifyDeviceBound(deviceID string, binding contracts.BoundBinding)
	NotifyCatalogUpdated(deviceID string)
}

// BindingService 处理设备绑定与固件 hello。
type BindingService struct {
	repos    *storage.Repositories
	cfg      *config.Config
	notifier Notifier
	logger   *slog.Logger
}

// NewBindingService 构造绑定服务。
func NewBindingService(repos *storage.Repositories, cfg *config.Config, notifier Notifier, logger *slog.Logger) *BindingService {
	return &BindingService{repos: repos, cfg: cfg, notifier: notifier, logger: logger}
}

// Bind 由已认证 Desktop 提交已动态登记的设备 MAC，校验账号归属并保存唯一绑定。
// 同一 Desktop 重复绑定幂等；已绑定其他 Desktop 返回 CONFLICT。
func (s *BindingService) Bind(ctx context.Context, desktopID domain.DesktopClientID, subject domain.Subject, mac string) (contracts.BoundBinding, error) {
	deviceID, err := domain.NormalizeMAC(mac)
	if err != nil {
		return contracts.BoundBinding{}, err
	}
	if desktopSubject, ok := s.cfg.DesktopSubject(desktopID); !ok || desktopSubject != subject {
		return contracts.BoundBinding{}, domain.ErrPermissionDenied("Desktop 与认证账号不匹配")
	}
	var created bool
	err = s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		device, err := s.repos.Devices.Get(ctx, tx, string(deviceID))
		if errors.Is(err, sql.ErrNoRows) {
			return domain.ErrDependencyUnavailable("设备尚未完成 hello 登记")
		}
		if err != nil {
			return err
		}
		registeredSubject, credentialOK := s.cfg.DeviceSubjectForCredential(device.CredentialKey)
		if device.Subject != string(subject) || !credentialOK || registeredSubject != subject {
			return domain.ErrPermissionDenied("设备与 Desktop 不属于同一账号")
		}
		existing, err := s.repos.Bindings.GetByDevice(ctx, tx, string(deviceID))
		switch {
		case err == nil:
			if err := validateBindingOwner(ctx, tx, s.cfg, existing); err != nil {
				return err
			}
			if existing.DesktopClientID == string(desktopID) {
				return nil // 幂等
			}
			return domain.ErrConflict("设备已绑定其他 Desktop")
		case !errors.Is(err, sql.ErrNoRows):
			return err
		}
		if !s.notifier.DeviceOnline(string(deviceID)) {
			return domain.ErrDependencyUnavailable("首次绑定需要设备在线")
		}
		if err := s.repos.Bindings.Insert(ctx, tx, storage.BindingRow{
			DeviceID:        string(deviceID),
			DesktopClientID: string(desktopID),
			Subject:         string(subject),
			CreatedAt:       time.Now().UTC(),
		}); err != nil {
			return err
		}
		created = true
		return nil
	})
	if err != nil {
		return contracts.BoundBinding{}, err
	}

	binding := contracts.BoundBinding{DeviceID: string(deviceID), DesktopClientID: string(desktopID)}
	if created {
		s.notifier.NotifyDeviceBound(string(deviceID), binding)
	}
	return binding, nil
}

// RegisterDevice 在首次 hello 时登记认证账号与真实 MAC，不能覆盖其他账号的归属。
func (s *BindingService) RegisterDevice(ctx context.Context, subject domain.Subject, mac, key string) (contracts.Binding, error) {
	deviceID, err := domain.NormalizeMAC(mac)
	if err != nil {
		return contracts.Binding{}, err
	}
	if currentSubject, ok := s.cfg.DeviceSubjectForCredential(key); !ok || currentSubject != subject {
		return contracts.Binding{}, domain.ErrPermissionDenied("设备接入账号已失效")
	}
	result := contracts.Binding{DeviceID: string(deviceID)}
	err = s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		device, err := s.repos.Devices.Register(ctx, tx, string(deviceID), string(subject), key)
		if err != nil {
			return err
		}
		if device.Subject != string(subject) {
			return domain.ErrPermissionDenied("设备已属于其他账号")
		}
		binding, err := s.repos.Bindings.GetByDevice(ctx, tx, string(deviceID))
		if errors.Is(err, sql.ErrNoRows) {
			return nil
		}
		if err != nil {
			return err
		}
		if err := validateBindingOwner(ctx, tx, s.cfg, binding); err != nil {
			return err
		}
		result.DesktopClientID = &binding.DesktopClientID
		return nil
	})
	return result, err
}

// Hello 处理同一连接的重复 hello；登记后的连接不能中途换成另一个 MAC。
func (s *BindingService) Hello(ctx context.Context, deviceID domain.DeviceID, mac string) (contracts.Binding, error) {
	got, err := domain.NormalizeMAC(mac)
	if err != nil {
		return contracts.Binding{}, err
	}
	if got != deviceID {
		return contracts.Binding{}, domain.ErrPermissionDenied("MAC 与认证设备不一致")
	}
	row, err := s.repos.Bindings.GetByDevice(ctx, s.repos.DB(), string(deviceID))
	if errors.Is(err, sql.ErrNoRows) {
		return contracts.Binding{DeviceID: string(deviceID), DesktopClientID: nil}, nil
	}
	if err != nil {
		return contracts.Binding{}, err
	}
	if err := validateBindingOwner(ctx, s.repos.DB(), s.cfg, row); err != nil {
		return contracts.Binding{}, err
	}
	clientID := row.DesktopClientID
	return contracts.Binding{DeviceID: string(deviceID), DesktopClientID: &clientID}, nil
}

// validateBindingOwner 核对持久登记、绑定和当前接入账号，不根据配置 MAC 推断设备归属。
func validateBindingOwner(ctx context.Context, q storage.Queryer, cfg *config.Config, row *storage.BindingRow) error {
	device, err := (storage.DeviceRepo{}).Get(ctx, q, row.DeviceID)
	if err != nil {
		return err
	}
	desktopSubject, desktopOK := cfg.DesktopSubject(domain.DesktopClientID(row.DesktopClientID))
	deviceSubject, deviceOK := cfg.DeviceSubjectForCredential(device.CredentialKey)
	if !deviceOK || string(deviceSubject) != device.Subject || !desktopOK || row.Subject == "" || device.Subject != row.Subject || string(desktopSubject) != row.Subject {
		return domain.ErrPermissionDenied("绑定所属账号与当前设备或 Desktop 不匹配")
	}
	return nil
}

// authorizedDevices 仅向双方当前账号仍匹配持久化绑定的设备发布通知。
func authorizedDevices(ctx context.Context, repos *storage.Repositories, cfg *config.Config, desktopID string) ([]string, error) {
	devices, err := repos.Bindings.ListDevicesByDesktop(ctx, repos.DB(), desktopID)
	if err != nil {
		return nil, err
	}
	var authorized []string
	for _, device := range devices {
		row, err := repos.Bindings.GetByDevice(ctx, repos.DB(), device)
		if err != nil {
			return nil, err
		}
		if err := validateBindingOwner(ctx, repos.DB(), cfg, row); err != nil {
			if errors.Is(err, domain.ErrPermissionDenied("")) {
				continue
			}
			return nil, err
		}
		authorized = append(authorized, device)
	}
	return authorized, nil
}
