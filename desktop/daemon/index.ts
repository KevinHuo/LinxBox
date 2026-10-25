/**
 * LinxBox Desktop Companion Daemon 的公开领域层接口。
 *
 * 这里只做再导出：真正的实现分散在各个子模块里，便于按边界单独测试与评审。
 */

export * from './tasks/delivery.js';
export * from './tasks/task-state.js';
export * from './policy/authorization.js';
export * from './result-projector/projection.js';
export * from './project-registry/registry.js';
export * from './gateway-client/frames.js';
export * from './gateway-client/client.js';
export * from './gateway-client/dispatcher.js';
export * from './local-store/schema.js';
