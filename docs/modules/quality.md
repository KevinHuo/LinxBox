# `quality`：质量、集成与发布证据

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24  
> Owner: `OWNERS.md` 中的 `quality`

## 责任

维护 Spec 流程、契约 fixtures、CI、端到端环境、安全回归、故障/负载/稳定性验证和证据索引。Quality 不拥有业务规则，也不替代模块自己的单元测试。

必须区分：

- 文档/Schema 检查；
- 单元和增量覆盖率；
- producer/consumer 契约测试；
- 真实数据库、WSS、Electron、app-server 集成；
- 模拟 Device/Linx E2E；
- 真实 AI Passport、灵矽、macOS 和 app-server E2E；
- 真机资源、按键、屏幕和 soak；
- 合并门禁与功能启用/发布门禁。

不得把 mock、截图、构建成功、生成固件、作者自审或 Spec 状态字段修改表述为真实验收通过。详细门禁见 [`docs/quality-gates.md`](../quality-gates.md)。
