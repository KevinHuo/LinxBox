package domain

import (
	"errors"
	"fmt"
)

// Code 是公共错误码，与共享契约 Error.code 枚举一致。
type Code string

// RequestRecoveryMessage 不能将查不到记录误解为原任务一定没有执行。
const RequestRecoveryMessage = "当前 Gateway 未找到该请求记录；请停止轮询此 ID，若曾确认执行请先在 Desktop 核对原任务"

const (
	CodeInvalidArgument       Code = "INVALID_ARGUMENT"
	CodeUnauthenticated       Code = "UNAUTHENTICATED"
	CodePermissionDenied      Code = "PERMISSION_DENIED"
	CodeNotFound              Code = "NOT_FOUND"
	CodeConflict              Code = "CONFLICT"
	CodeDependencyUnavailable Code = "DEPENDENCY_UNAVAILABLE"
	CodeResourceExhausted     Code = "RESOURCE_EXHAUSTED"
	CodeInternal              Code = "INTERNAL"
)

// Error 表示业务错误；Message 可对外展示，Cause 仅用于内部日志。
type Error struct {
	Code    Code
	Message string
	Cause   error
}

func (e *Error) Error() string {
	if e.Cause != nil {
		return fmt.Sprintf("%s: %s (%v)", e.Code, e.Message, e.Cause)
	}
	return fmt.Sprintf("%s: %s", e.Code, e.Message)
}

func (e *Error) Unwrap() error { return e.Cause }

// Is 使 errors.Is 可按错误码匹配。
func (e *Error) Is(target error) bool {
	t, ok := target.(*Error)
	return ok && t.Code == e.Code
}

// NewError 构造业务错误。
func NewError(code Code, message string) *Error {
	return &Error{Code: code, Message: message}
}

// WrapError 构造带内部原因的业务错误。
func WrapError(code Code, message string, cause error) *Error {
	return &Error{Code: code, Message: message, Cause: cause}
}

func ErrInvalidArgument(msg string) *Error       { return NewError(CodeInvalidArgument, msg) }
func ErrUnauthenticated(msg string) *Error       { return NewError(CodeUnauthenticated, msg) }
func ErrPermissionDenied(msg string) *Error      { return NewError(CodePermissionDenied, msg) }
func ErrNotFound(msg string) *Error              { return NewError(CodeNotFound, msg) }
func ErrConflict(msg string) *Error              { return NewError(CodeConflict, msg) }
func ErrDependencyUnavailable(msg string) *Error { return NewError(CodeDependencyUnavailable, msg) }
func ErrResourceExhausted(msg string) *Error     { return NewError(CodeResourceExhausted, msg) }
func ErrInternal(msg string) *Error              { return NewError(CodeInternal, msg) }

// CodeOf 提取任意错误的业务错误码；未知错误视为 INTERNAL。
func CodeOf(err error) Code {
	if err == nil {
		return CodeInternal
	}
	var e *Error
	if errors.As(err, &e) && e != nil {
		return e.Code
	}
	return CodeInternal
}

// MessageOf 提取可对外展示的错误信息；未知错误使用默认文案。
func MessageOf(err error) string {
	if err == nil {
		return "内部错误"
	}
	var e *Error
	if errors.As(err, &e) && e != nil {
		return e.Message
	}
	return "内部错误"
}
