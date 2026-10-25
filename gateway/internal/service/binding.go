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

// Bind 由已认证 Desktop 提交设备 MAC：校验配对权限与主体一致性，保存唯一绑定。
// 同一 Desktop 重复绑定幂等；已绑定其他 Desktop 返回 CONFLICT。
func (s *BindingService) Bind(ctx context.Context, desktopID domain.DesktopClientID, subject domain.Subject, mac string) (contracts.BoundBinding, error) {
	deviceID, err := domain.NormalizeMAC(mac)
	if err != nil {
		return contracts.BoundBinding{}, err
	}
	if !s.pairingAllowed(subject, deviceID) {
		return contracts.BoundBinding{}, domain.ErrPermissionDenied("该账号无权绑定此设备")
	}
	if devSubject, ok := s.cfg.DeviceSubject(deviceID); !ok || devSubject != subject {
		return contracts.BoundBinding{}, domain.ErrPermissionDenied("设备与 Desktop 不属于同一账号")
	}

	var created bool
	err = s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		existing, err := s.repos.Bindings.GetByDevice(ctx, tx, string(deviceID))
		switch {
		case err == nil:
			if existing.DesktopClientID == string(desktopID) {
				return nil // 幂等
			}
			return domain.ErrConflict("设备已绑定其他 Desktop")
		case !errors.Is(err, sql.ErrNoRows):
			return err
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

// Hello 处理固件 device.hello：MAC 必须与认证设备一致，返回当前绑定。
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
	clientID := row.DesktopClientID
	return contracts.Binding{DeviceID: string(deviceID), DesktopClientID: &clientID}, nil
}

// pairingAllowed 判断账号主体是否被配置允许绑定该设备（知道 MAC 不等于拥有配对权）。
func (s *BindingService) pairingAllowed(subject domain.Subject, deviceID domain.DeviceID) bool {
	for _, rule := range s.cfg.Pairing.Rules {
		if rule.Subject != string(subject) {
			continue
		}
		for _, d := range rule.DeviceIDs {
			if d == string(deviceID) {
				return true
			}
		}
	}
	return false
}
