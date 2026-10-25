package transport

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"net/http"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"

	"github.com/KevinHuo/LinxBox/gateway/internal/config"
	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/lifecycle"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
)

// frameReadMargin 是底层读限制相对配置帧上限的余量：让超限帧先到达业务层，
// 由业务层以 1009 关闭帧明确拒绝，而不是由 gorilla 在读循环静默报错。
const frameReadMargin = 4 * 1024

// Server 是单端口 HTTP/WSS 服务：健康检查、固件与 Desktop 入口共用监听端口。
type Server struct {
	cfg          *config.Config
	logger       *slog.Logger
	validator    *contracts.Validator
	auth         *Authenticator
	conns        *lifecycle.Connections
	binding      *service.BindingService
	sessions     *service.SessionService
	queries      *service.QueryRouter
	requests     *service.RequestService
	mcp          *mcpEndpoint
	upgrader     websocket.Upgrader
	ready        atomic.Bool
	httpSrv      *http.Server
	helloTimeout time.Duration
}

// NewServer 构造传输层服务；绑定与目录服务随后通过 SetServices 注入。
func NewServer(cfg *config.Config, logger *slog.Logger, validator *contracts.Validator, auth *Authenticator, conns *lifecycle.Connections) *Server {
	s := &Server{
		cfg:          cfg,
		logger:       logger,
		validator:    validator,
		auth:         auth,
		conns:        conns,
		helloTimeout: deviceHelloTimeout,
		upgrader: websocket.Upgrader{
			Subprotocols: []string{"linxbox.v1"},
			CheckOrigin:  func(*http.Request) bool { return true },
		},
	}
	s.queries = service.NewQueryRouter(validator, cfg.Timeouts.QueryTimeout.Std(), cfg.Limits.MaxPendingQueries, cfg.Limits.MaxDeviceQueries,
		func(q service.Query, data json.RawMessage, err error) {
			s.logQueryCompletion(q, data, err)
			conn := conns.Device(q.DeviceID)
			if conn != nil && conn.Generation() == q.DeviceGeneration {
				s.respondFirmware(conn, q.Operation, q.FirmwareID, data, err)
			}
		})
	return s
}

// SetServices 注入业务服务（app 组装时调用）。
func (s *Server) SetServices(binding *service.BindingService, sessions *service.SessionService) {
	s.binding = binding
	s.sessions = sessions
}

// Handler 返回路由。
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", s.handleHealthz)
	mux.HandleFunc("/readyz", s.handleReadyz)
	mux.HandleFunc("/v1/devices/connect", s.handleDeviceConnect)
	mux.HandleFunc("/v1/clients/connect", s.handleDesktopConnect)
	mux.HandleFunc("/mcp", s.handleMCP)
	mux.HandleFunc("/mcp/message", s.handleMCP)
	return mux
}

// Run 启动服务并阻塞，直到 ctx 取消或监听失败；收尾时停止接收新请求。
func (s *Server) Run(ctx context.Context) error {
	ctx, cancelRun := context.WithCancel(ctx)
	defer cancelRun()
	if s.requests != nil {
		if err := s.requests.Expire(ctx); err != nil {
			return err
		}
		go s.expireRequests(ctx)
	}
	defer s.CloseMCP()
	s.ready.Store(true)
	s.httpSrv = &http.Server{
		Addr:              s.cfg.Listen.Addr,
		Handler:           s.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
	}
	errCh := make(chan error, 1)
	go func() {
		s.logger.Info("Gateway 开始监听", "event", "server.starting", "addr", s.cfg.Listen.Addr, "tls", s.cfg.Listen.TLSCert != "")
		var err error
		if s.cfg.Listen.TLSCert != "" {
			err = s.httpSrv.ListenAndServeTLS(s.cfg.Listen.TLSCert, s.cfg.Listen.TLSKey)
		} else {
			err = s.httpSrv.ListenAndServe()
		}
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			errCh <- err
		}
	}()
	select {
	case err := <-errCh:
		s.logger.Error("Gateway 监听失败", "event", "server.listen_failed")
		s.ready.Store(false)
		return err
	case <-ctx.Done():
		s.logger.Info("Gateway 正在停止", "event", "server.stopping")
		s.ready.Store(false)
		s.CloseMCP()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), s.cfg.Timeouts.ShutdownGrace.Std())
		defer cancel()
		_ = s.httpSrv.Shutdown(shutdownCtx)
		s.conns.CloseAll()
		return nil
	}
}

func (s *Server) expireRequests(ctx context.Context) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			if err := s.requests.Expire(ctx); err != nil && ctx.Err() == nil {
				s.logger.Error("请求过期处理失败")
			}
		}
	}
}

// CloseMCP 结束 SSE 长连接，释放 SDK 会话。
func (s *Server) CloseMCP() {
	if s.mcp != nil {
		s.mcp.close()
	}
}

// NotifyRequest 推送已提交请求快照；未连接设备用 request.get 恢复。
func (s *Server) NotifyRequest(deviceID, event string, request contracts.Request) {
	if conn := s.conns.Device(deviceID); conn != nil {
		s.sendFirmwarePush(conn, event, struct {
			ID      string            `json:"id"`
			Type    string            `json:"type"`
			Payload contracts.Request `json:"payload"`
		}{contracts.NewMessageID("msg_"), event, request})
	}
}

func (s *Server) handleHealthz(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("ok\n"))
}

func (s *Server) handleReadyz(w http.ResponseWriter, _ *http.Request) {
	if s.ready.Load() {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ready\n"))
		return
	}
	w.WriteHeader(http.StatusServiceUnavailable)
	_, _ = w.Write([]byte("shutting down\n"))
}

// upgradeAndAuthenticate 升级为 WebSocket 并认证；失败时以 4401 关闭。
func (s *Server) upgradeAndAuthenticate(w http.ResponseWriter, r *http.Request, kind PrincipalKind) (*websocket.Conn, Principal, bool) {
	conn, err := s.upgrader.Upgrade(w, r, nil)
	if err != nil {
		s.logger.Warn("WebSocket 升级失败：检查完整连接路径及 Upgrade 请求头", "event", "connection.upgrade_failed", "kind", kindName(kind), "remote_addr", r.RemoteAddr,
			"error_message", "HTTP 请求未满足 WebSocket 升级条件", "error_hint", "使用完整 /v1/devices/connect 或 /v1/clients/connect 地址并进行 WebSocket 握手")
		return nil, Principal{}, false
	}
	principal, err := s.auth.Authenticate(kind, r)
	if err != nil {
		fields := []any{"event", "connection.auth_failed", "kind", kindName(kind), "remote_addr", r.RemoteAddr}
		fields = append(fields, errorFields(err, "gateway")...)
		s.logger.Warn("WebSocket 认证失败："+domain.MessageOf(err), fields...)
		_ = conn.WriteControl(websocket.CloseMessage,
			websocket.FormatCloseMessage(4401, "unauthenticated"), time.Now().Add(time.Second))
		_ = conn.Close()
		return nil, Principal{}, false
	}
	s.logger.Info("连接认证成功", "event", "connection.authenticated", "kind", kindName(kind), "subject", principal.Subject, "client_id", principal.DesktopID, "remote_addr", r.RemoteAddr)
	return conn, principal, true
}

func (s *Server) handleDeviceConnect(w http.ResponseWriter, r *http.Request) {
	conn, principal, ok := s.upgradeAndAuthenticate(w, r, PrincipalDevice)
	if !ok {
		return
	}
	s.serveFirmware(conn, principal)
}

func (s *Server) handleDesktopConnect(w http.ResponseWriter, r *http.Request) {
	conn, principal, ok := s.upgradeAndAuthenticate(w, r, PrincipalDesktop)
	if !ok {
		return
	}
	s.serveDesktop(conn, principal)
}

func kindName(k PrincipalKind) string {
	if k == PrincipalDevice {
		return "device"
	}
	return "desktop"
}

// DeviceOnline 提供绑定事务所需的设备当前在线状态。
func (s *Server) DeviceOnline(deviceID string) bool {
	return s.conns.DeviceOnline(deviceID)
}

// NotifyDeviceBound 实现 service.Notifier：向在线设备推送 device.bound。
func (s *Server) NotifyDeviceBound(deviceID string, binding contracts.BoundBinding) {
	conn := s.conns.Device(deviceID)
	if conn == nil {
		return
	}
	msg := contracts.DeviceBoundMessage{
		ID:      contracts.NewMessageID("msg_"),
		Type:    "device.bound",
		Payload: binding,
	}
	s.sendFirmwarePush(conn, "device.bound", msg)
}

// NotifyCatalogUpdated 实现 service.Notifier：向在线设备推送 agent_sessions.updated。
func (s *Server) NotifyCatalogUpdated(deviceID string) {
	conn := s.conns.Device(deviceID)
	if conn == nil {
		return
	}
	msg := contracts.AgentSessionsUpdatedMessage{
		ID:      contracts.NewMessageID("msg_"),
		Type:    "agent_sessions.updated",
		Payload: contracts.Empty{},
	}
	s.sendFirmwarePush(conn, "agent_sessions.updated", msg)
}

func (s *Server) sendFirmwarePush(conn *lifecycle.Connection, typ string, msg any) {
	raw, err := json.Marshal(msg)
	if err != nil {
		s.logger.Error("推送编码失败", "type", typ, "err", err)
		return
	}
	if len(raw) > s.cfg.Limits.MaxFrameBytes {
		s.logger.Warn("推送超过帧限制", "type", typ)
		conn.Close()
		return
	}
	if err := s.validator.ValidateFirmwarePush(typ, raw); err != nil {
		s.logger.Error("推送校验失败", "type", typ, "error_code", domain.CodeInternal)
		return
	}
	if err := conn.Send(raw, s.conns.SendTimeout()); err != nil {
		s.logger.Warn("推送发送失败", "type", typ, "err", err)
	}
}

// toWireError 将业务错误映射为契约 Error。
func toWireError(err error) contracts.Error {
	return contracts.Error{
		Code:    string(domain.CodeOf(err)),
		Message: domain.MessageOf(err),
	}
}

var _ service.Notifier = (*Server)(nil)
