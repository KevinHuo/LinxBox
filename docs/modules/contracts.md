# `contracts`：契约与领域

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24  
> Owner: `OWNERS.md` 中的 `contracts`

## 责任

维护 MCP、Device/Linx、Gateway/Desktop、Task/Turn/Receipt 状态、结果和错误的版本化 schema、语义文档及正反例 fixtures。

规则：

- schema 是实现阶段的单一事实源，类型从 schema 生成或双向一致性检查；
- ID 使用不同值对象，禁止裸字符串互换；
- 删除、改名、类型或语义改变升主版本，并写迁移/回滚；
- 安全请求默认拒绝未知字段和超限 payload；
- producer、consumer 使用同一组 sanitized fixtures；
- Task state、Turn receipt、Delivery ACK 和 Device UI state 分开定义；
- 跨边界变更需要 Spec、双方 Reviewer 和 Integration Owner。

契约索引见 [`docs/contracts/README.md`](../contracts/README.md)。模块不拥有 transport、数据库、UI 或 provider 实现。
