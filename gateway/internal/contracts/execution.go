package contracts

import "encoding/json"

// TaskDispatchPayload 沿用共享协议；追问始终指定已固定的 thread。
type TaskDispatchPayload struct {
	TaskID             string       `json:"task_id"`
	ProjectID          string       `json:"project_id"`
	Agent              string       `json:"agent"`
	Capabilities       []string     `json:"capabilities"`
	Prompt             string       `json:"prompt"`
	AcceptanceCriteria []string     `json:"acceptance_criteria"`
	PolicyVersion      string       `json:"policy_version"`
	Continuation       Continuation `json:"continuation"`
}

type Continuation struct {
	AgentSessionID string `json:"agent_session_id"`
	ThreadID       string `json:"thread_id"`
}

// TaskAckPayload 只报告投递结果，不等于任务运行或完成。
type TaskAckPayload struct {
	MessageID   string       `json:"message_id"`
	Sequence    int64        `json:"sequence"`
	TaskID      string       `json:"task_id"`
	Disposition string       `json:"disposition"`
	Error       *PublicError `json:"error,omitempty"`
}

type PublicError struct {
	Code      string         `json:"code"`
	Message   string         `json:"message"`
	Retryable bool           `json:"retryable"`
	RequestID string         `json:"request_id"`
	Details   map[string]any `json:"details,omitempty"`
}

// TaskEventPayload 在入口通过完整共享 Schema 校验；Result 保留全部结构化事实。
type TaskEventPayload struct {
	TaskID        string          `json:"task_id"`
	EventID       string          `json:"event_id"`
	EventSequence int64           `json:"event_sequence"`
	OccurredAt    string          `json:"occurred_at"`
	State         string          `json:"state"`
	Stage         string          `json:"stage"`
	Summary       string          `json:"summary"`
	Result        json.RawMessage `json:"result"`
	Error         *PublicError    `json:"error"`
	ThreadID      *string         `json:"thread_id"`
	TurnID        *string         `json:"turn_id"`
}
