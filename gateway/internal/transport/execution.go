package transport

import (
	"encoding/json"
	"time"

	"github.com/KevinHuo/LinxBox/gateway/internal/contracts"
	"github.com/KevinHuo/LinxBox/gateway/internal/domain"
	"github.com/KevinHuo/LinxBox/gateway/internal/service"
)

// PrepareDispatch 只准备固定连接上的有界任务帧；业务层落盘后才调用 Send。
func (s *Server) PrepareDispatch(desktop string, payload contracts.TaskDispatchPayload) (service.TaskDelivery, error) {
	conn := s.conns.Desktop(desktop)
	if conn == nil {
		return service.TaskDelivery{}, domain.ErrDependencyUnavailable("Desktop 未连接，请在确认前恢复连接")
	}
	frame := contracts.DesktopEnvelope{ProtocolVersion: contracts.ProtocolVersion,
		MessageID: contracts.NewMessageID("dispatch_"), Sequence: conn.NextSequence(), Type: "task.dispatch",
		SentAt: time.Now().UTC().Format(time.RFC3339Nano), Payload: payload}
	raw, err := json.Marshal(frame)
	if err != nil {
		return service.TaskDelivery{}, err
	}
	if len(raw) > s.cfg.Limits.MaxFrameBytes {
		return service.TaskDelivery{}, domain.ErrResourceExhausted("任务投递帧超过上限")
	}
	if err := s.validator.ValidateDesktopType("task.dispatch", raw); err != nil {
		return service.TaskDelivery{}, domain.ErrDependencyUnavailable("固定会话的路由信息不完整，无法投递")
	}
	return service.TaskDelivery{MessageID: frame.MessageID, Sequence: frame.Sequence, Generation: conn.Generation(), Send: func() error {
		if s.conns.Desktop(desktop) != conn {
			return domain.ErrDependencyUnavailable("Desktop 连接已更换")
		}
		if err := conn.Send(raw, s.conns.SendTimeout()); err != nil {
			return err
		}
		s.logger.Info("Gateway → Desktop：任务投递已入队（不代表已执行）", "event", "task.enqueued", "client_id", desktop,
			"connection_id", conn.Generation(), "task_id", payload.TaskID, "message_id", frame.MessageID)
		return nil
	}}, nil
}
