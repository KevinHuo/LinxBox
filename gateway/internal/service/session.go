package service

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"sync"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/storage"
)

// SessionService 处理 Desktop 目录同步与固件目录读取。
type SessionService struct {
	cursorMu sync.Mutex
	cursors  map[string]catalogCursor
	repos    *storage.Repositories
	cfg      *config.Config
	notifier Notifier
	logger   *slog.Logger
}

// NewSessionService 构造目录服务。
func NewSessionService(repos *storage.Repositories, cfg *config.Config, notifier Notifier, logger *slog.Logger) *SessionService {
	return &SessionService{repos: repos, cfg: cfg, notifier: notifier, logger: logger, cursors: map[string]catalogCursor{}}
}

// Sync 全量替换 Desktop 的会话目录并推进版本；任一记录非法或超限时整批拒绝，
// 保留旧目录。成功后通知绑定到该 Desktop 的在线设备刷新。
func (s *SessionService) Sync(ctx context.Context, desktopID domain.DesktopClientID, records []contracts.SessionRecord) (int64, error) {
	if err := s.validateRecords(records); err != nil {
		return 0, err
	}
	now := time.Now().UTC()
	rows := make([]storage.SessionRow, 0, len(records))
	for _, r := range records {
		rows = append(rows, storage.SessionRow{
			DesktopClientID: string(desktopID),
			AgentSessionID:  r.AgentSessionID,
			ThreadID:        r.ThreadID,
			ProjectID:       r.ProjectID,
			ProjectName:     r.ProjectName,
			Agent:           r.Agent,
			Title:           r.Title,
			Status:          r.Status,
			Summary:         r.Summary,
		})
	}
	var version int64
	err := s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		current, err := s.repos.Sessions.GetVersion(ctx, tx, string(desktopID))
		if err != nil {
			return err
		}
		version = current + 1
		return s.repos.Sessions.ReplaceAll(ctx, tx, string(desktopID), rows, version, now)
	})
	if err != nil {
		return 0, err
	}
	devices, err := s.repos.Bindings.ListDevicesByDesktop(ctx, s.repos.DB(), string(desktopID))
	if err != nil {
		s.logger.Warn("读取绑定设备失败", "client_id", desktopID, "err", err)
		return version, nil
	}
	for _, d := range devices {
		s.notifier.NotifyCatalogUpdated(d)
	}
	return version, nil
}

// validateRecords 检查重复 agent_session_id 与数量上限；全量列表不重复。
func (s *SessionService) validateRecords(records []contracts.SessionRecord) error {
	if len(records) > s.cfg.Limits.MaxCatalogItems {
		return domain.ErrInvalidArgument("目录记录数超过上限")
	}
	seen := make(map[string]struct{}, len(records))
	for _, r := range records {
		if _, dup := seen[r.AgentSessionID]; dup {
			return domain.ErrInvalidArgument("目录中存在重复的 agent_session_id")
		}
		seen[r.AgentSessionID] = struct{}{}
	}
	return nil
}

// List 向固件分页提供目录；cursor 绑定目录版本，目录更新后旧 cursor 作废。
func (s *SessionService) List(ctx context.Context, deviceID domain.DeviceID, cursor string) (contracts.SessionPage, error) {
	desktopID, err := s.boundDesktop(ctx, deviceID)
	if err != nil {
		return contracts.SessionPage{}, err
	}
	var token catalogCursor
	if cursor != "" {
		s.cursorMu.Lock()
		var ok bool
		token, ok = s.cursors[cursor]
		s.cursorMu.Unlock()
		if !ok {
			return contracts.SessionPage{}, domain.ErrConflict("cursor 已失效，请从首页重新读取")
		}
		if token.DeviceID != string(deviceID) || token.DesktopID != desktopID {
			return contracts.SessionPage{}, domain.ErrInvalidArgument("cursor 不属于当前查询")
		}
		if !time.Now().Before(token.Expires) {
			return contracts.SessionPage{}, domain.ErrConflict("cursor 已过期，请从首页重新读取")
		}
	}
	offset := token.Offset
	limit := s.cfg.Limits.SessionPageSize
	var current int64
	var rows []storage.SessionRow
	// 版本和分页数据来自同一读事务，避免同步恰好发生在两次读取之间。
	err = s.repos.WithTx(ctx, func(tx *sql.Tx) error {
		var err error
		current, err = s.repos.Sessions.GetVersion(ctx, tx, desktopID)
		if err != nil {
			return err
		}
		if cursor != "" && token.Version != current {
			return domain.ErrConflict("目录已更新，请从首页重新读取")
		}
		rows, err = s.repos.Sessions.List(ctx, tx, desktopID, offset, limit+1)
		return err
	})
	if err != nil {
		return contracts.SessionPage{}, err
	}
	page := make([]contracts.AgentSession, 0, min(len(rows), limit))
	for _, r := range rows {
		if len(page) >= limit {
			break
		}
		page = append(page, contracts.AgentSession{
			AgentSessionID: r.AgentSessionID,
			Title:          r.Title,
			ProjectName:    r.ProjectName,
			Agent:          r.Agent,
			Status:         r.Status,
			Summary:        r.Summary,
		})
	}
	var next *string
	if len(rows) > limit {
		nc, err := s.newCursor(catalogCursor{DeviceID: string(deviceID), DesktopID: desktopID, Version: current, Offset: offset + limit, Expires: time.Now().Add(5 * time.Minute)})
		if err != nil {
			return contracts.SessionPage{}, err
		}
		next = &nc
	}
	return contracts.SessionPage{Items: page, NextCursor: next}, nil
}

// Get 返回固件请求的单条会话投影；必须属于设备绑定 Desktop 的目录。
func (s *SessionService) Get(ctx context.Context, deviceID domain.DeviceID, sessionID string) (contracts.AgentSession, error) {
	desktopID, err := s.boundDesktop(ctx, deviceID)
	if err != nil {
		return contracts.AgentSession{}, err
	}
	row, err := s.repos.Sessions.Get(ctx, s.repos.DB(), desktopID, sessionID)
	if errors.Is(err, sql.ErrNoRows) {
		return contracts.AgentSession{}, domain.ErrNotFound("会话不存在")
	}
	if err != nil {
		return contracts.AgentSession{}, err
	}
	return contracts.AgentSession{
		AgentSessionID: row.AgentSessionID,
		Title:          row.Title,
		ProjectName:    row.ProjectName,
		Agent:          row.Agent,
		Status:         row.Status,
		Summary:        row.Summary,
	}, nil
}

// boundDesktop 返回设备绑定 Desktop；未绑定拒绝。
func (s *SessionService) boundDesktop(ctx context.Context, deviceID domain.DeviceID) (string, error) {
	row, err := s.repos.Bindings.GetByDevice(ctx, s.repos.DB(), string(deviceID))
	if errors.Is(err, sql.ErrNoRows) {
		return "", domain.ErrPermissionDenied("设备未绑定 Desktop")
	}
	if err != nil {
		return "", err
	}
	return row.DesktopClientID, nil
}

// DesktopForSession 解析已绑定设备的查询路由，并核实目标属于该 Desktop。
func (s *SessionService) DesktopForSession(ctx context.Context, deviceID domain.DeviceID, sessionID string) (string, error) {
	desktopID, err := s.boundDesktop(ctx, deviceID)
	if err != nil {
		return "", err
	}
	if _, err := s.repos.Sessions.Get(ctx, s.repos.DB(), desktopID, sessionID); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return "", domain.ErrNotFound("会话不存在")
		}
		return "", err
	}
	return desktopID, nil
}

// DevicesForSession 只允许认证 Desktop 通知其自身目录中的会话。
func (s *SessionService) DevicesForSession(ctx context.Context, desktopID domain.DesktopClientID, sessionID string) ([]string, error) {
	if _, err := s.repos.Sessions.Get(ctx, s.repos.DB(), string(desktopID), sessionID); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, domain.ErrNotFound("会话不存在")
		}
		return nil, err
	}
	return s.repos.Bindings.ListDevicesByDesktop(ctx, s.repos.DB(), string(desktopID))
}

type catalogCursor struct {
	DeviceID, DesktopID string
	Version             int64
	Offset              int
	Expires             time.Time
}

func (s *SessionService) newCursor(cursor catalogCursor) (string, error) {
	s.cursorMu.Lock()
	defer s.cursorMu.Unlock()
	now := time.Now()
	for id, c := range s.cursors {
		if !now.Before(c.Expires) {
			delete(s.cursors, id)
		}
	}
	if len(s.cursors) >= 1024 {
		return "", domain.ErrResourceExhausted("分页快照数量达到上限")
	}
	id := contracts.NewMessageID("cur_")
	s.cursors[id] = cursor
	return id, nil
}
