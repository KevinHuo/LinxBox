package contracts

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"time"
)

// 本文件仅用于 JSON 编解码与业务表达；字段合法性以共享 Schema 为准。
// 显式处理可省略字段、必填 null、空数组与整数精度，防止 Go 零值改变线上语义。

// Error 是公共最小错误（新增接口使用）。
type Error struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

// Binding 是固件 hello 返回的当前绑定（desktop_client_id 可为 null）。
type Binding struct {
	DeviceID        string  `json:"device_id"`
	DesktopClientID *string `json:"desktop_client_id"`
}

// BoundBinding 是已绑定的设备与 Desktop 组合（desktop_client_id 必填）。
type BoundBinding struct {
	DeviceID        string `json:"device_id"`
	DesktopClientID string `json:"desktop_client_id"`
}

// AgentSession 是固件可见的会话投影（不含本机 thread/project 标识）。
type AgentSession struct {
	AgentSessionID string `json:"agent_session_id"`
	Title          string `json:"title"`
	ProjectName    string `json:"project_name"`
	Agent          string `json:"agent"`
	Status         string `json:"status"`
	Summary        string `json:"summary"`
}

// SessionRecord 是 Desktop 全量同步的会话记录。
type SessionRecord struct {
	AgentSessionID string `json:"agent_session_id"`
	Title          string `json:"title"`
	ProjectName    string `json:"project_name"`
	Agent          string `json:"agent"`
	Status         string `json:"status"`
	Summary        string `json:"summary"`
	ThreadID       string `json:"thread_id"`
	ProjectID      string `json:"project_id"`
}

// SessionPage 是目录分页（每页最多 5 项）。
type SessionPage struct {
	Items      []AgentSession `json:"items"`
	NextCursor *string        `json:"next_cursor"`
}

// Empty 是空对象负载。
type Empty struct{}

// FirmwareMessage 是固件 → Gateway 消息 envelope。
type FirmwareMessage struct {
	ID      string          `json:"id"`
	Type    string          `json:"type"`
	Payload json.RawMessage `json:"payload"`
}

// FirmwareResponse 是 Gateway → 固件的 response。
type FirmwareResponse struct {
	ID      string               `json:"id"`
	Type    string               `json:"type"`
	Payload FirmwareResponseBody `json:"payload"`
	ReplyTo string               `json:"reply_to"`
}

// FirmwareResponseBody 是 response 负载。
type FirmwareResponseBody struct {
	OK    bool   `json:"ok"`
	Data  any    `json:"data,omitempty"`
	Error *Error `json:"error,omitempty"`
}

// DeviceBoundMessage 是固件绑定成功推送。
type DeviceBoundMessage struct {
	ID      string       `json:"id"`
	Type    string       `json:"type"`
	Payload BoundBinding `json:"payload"`
}

// AgentSessionsUpdatedMessage 通知固件刷新目录。
type AgentSessionsUpdatedMessage struct {
	ID      string `json:"id"`
	Type    string `json:"type"`
	Payload Empty  `json:"payload"`
}

// DeviceHelloPayload 是 device.hello 的负载。
type DeviceHelloPayload struct {
	MacAddress string `json:"mac_address"`
}

// AgentSessionsListPayload 是 agent_sessions.list 的负载。
type AgentSessionsListPayload struct {
	Cursor *string `json:"cursor"`
}

// AgentSessionGetPayload 是 agent_session.get 的负载。
type AgentSessionGetPayload struct {
	AgentSessionID string `json:"agent_session_id"`
}

// DesktopMessage 是 Desktop → Gateway 消息 envelope。
type DesktopMessage struct {
	ProtocolVersion string          `json:"protocol_version"`
	MessageID       string          `json:"message_id"`
	Sequence        int64           `json:"sequence"`
	Type            string          `json:"type"`
	SentAt          string          `json:"sent_at"`
	ExpiresAt       *string         `json:"expires_at,omitempty"`
	Payload         json.RawMessage `json:"payload"`
}

// DesktopEnvelope 是 Gateway → Desktop 消息 envelope。
type DesktopEnvelope struct {
	ProtocolVersion string  `json:"protocol_version"`
	MessageID       string  `json:"message_id"`
	Sequence        int64   `json:"sequence"`
	Type            string  `json:"type"`
	SentAt          string  `json:"sent_at"`
	ExpiresAt       *string `json:"expires_at,omitempty"`
	Payload         any     `json:"payload"`
}

// BindingResultPayload 是 binding.result 负载。
type BindingResultPayload struct {
	ReplyToMessageID string        `json:"reply_to_message_id"`
	OK               bool          `json:"ok"`
	Data             *BoundBinding `json:"data,omitempty"`
	Error            *Error        `json:"error,omitempty"`
}

// SyncResultPayload 是 agent_sessions.sync.result 负载。
type SyncResultPayload struct {
	ReplyToMessageID string `json:"reply_to_message_id"`
	OK               bool   `json:"ok"`
	Error            *Error `json:"error,omitempty"`
}

// DeviceBindPayload 是 device.bind 负载。
type DeviceBindPayload struct {
	MacAddress string `json:"mac_address"`
}

// SessionSyncPayload 是 agent_sessions.sync 负载。
type SessionSyncPayload struct {
	Items []SessionRecord `json:"items"`
}

// ProtocolVersion 是 Desktop 协议版本。
const ProtocolVersion = "1.0"

// NewMessageID 生成符合 Id 约束（1～128 字符）的消息标识。
func NewMessageID(prefix string) string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return fmt.Sprintf("%s%x", prefix, time.Now().UnixNano())
	}
	return prefix + hex.EncodeToString(b[:])
}
