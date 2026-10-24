# Agent 工作规范

适用于参与 LinxBox 开发的 Claude Code、Codex 等 AI 助手。

当前 MVP 为通过 AI Passport/ESP32 和灵矽 AI 发起语音编码任务，由 Cloud MCP Gateway 路由到 macOS 上的 LinxBox Desktop，再通过 Codex app-server 在已授权本地项目内执行。

## 开发依据

开始业务实现或 Bug 修复前，先确定当前开发 Issue（Task 或 Bug），再按 Issue 中的链接读取对应 Proposal 和契约。不能只根据 README、Issue 标题或现有代码推断需求。

按以下顺序定位当前开发 Issue：

1. 优先使用用户明确指定的 Issue。
2. 用户未指定时，从当前 PR 的关联项和分支名中的 Issue 编号定位。
3. 仍无法唯一确定时，先请用户指定或创建 Task/Bug Issue，不自行选择看似相关的任务。

当前 MVP 的仓库内产品与技术契约是 [docs/architecture.md](docs/architecture.md)，完整链路以 [docs/end-to-end.md](docs/end-to-end.md) 为准，AI 行为以 [docs/ai-specification.md](docs/ai-specification.md) 为准，模块功能、代码树、边界契约和门禁分别以 [docs/modules/README.md](docs/modules/README.md)、[docs/code-structure.md](docs/code-structure.md)、[docs/contracts/README.md](docs/contracts/README.md) 和 [docs/quality-gates.md](docs/quality-gates.md) 为准。GitHub Proposal/Issue 提供讨论和任务追踪；除非用户明确要求，不修改其正文。历史 Proposal 与已接受仓库契约不一致时，以仓库契约为实现依据，并在交付说明中指出差异，不擅自改 Issue。

依据的优先级从高到低为：

1. 用户对当前工作的明确要求，用于确定要处理的开发 Issue，或明确要修订的 Proposal、契约或 ADR。
2. 已接受的仓库内产品契约、接口 schema、协议文档和 ADR。
3. 当前开发 Issue 中与仓库契约一致的目标、范围、非目标、契约引用和验收条件。开发 Issue 可以细化上层契约，但不得扩大或改变其范围。
4. 开发 Issue 关联的 Proposal，作为产品背景和追踪信息，不覆盖已接受仓库契约。
5. 相关测试，作为契约的可执行证明。
6. 现有实现和 README 中的概述。

上一层与下一层冲突时以上一层为准。用户要求如果需要改变已接受的契约或 ADR，该要求触发契约变更流程，不直接授权静默改变实现；先修改仓库契约或 ADR。GitHub Issue 仅在用户明确要求时同步。

MCP 工具、Device/Linx Presentation、Gateway/Desktop WSS、任务/回执状态、结果结构和 Codex app-server 边界已在 `docs/` 冻结。开发 Agent 不得自行改名消息、混合控制与展示通道、添加绕过 Gateway/Desktop 的路径，或把 app-server 原始事件暴露为公共契约。

只有已提交到仓库、并被开发 Issue 或 Proposal 明确引用的契约或 ADR 才是已接受依据。未跟踪文件、stash 恢复内容、未关联草稿和未确认的 Issue 讨论不得当作契约。

## 开始前

- 读取 [README.md](README.md)、[docs/README.md](docs/README.md)、[CODING_STANDARDS.md](CODING_STANDARDS.md)、[CONTRIBUTING.md](CONTRIBUTING.md)、[OWNERS.md](OWNERS.md)、主模块文档、当前开发 Issue、其关联的 Proposal 与契约，以及相关实现和测试。
- 若任务改变协议、状态权威、持久化、失败语义、运行时职责、安全边界或跨模块行为，先按 [Spec 流程](docs/specs/README.md)建立 bounded Spec；`Proposed` 不授权实现。
- 用一句话复述本次目标，列出范围、非目标和可检查的验收条件，并确认实现与每项条件的对应关系。
- 确认开发 Issue 的主模块、受影响模块和对应负责人；跨模块契约变更须由边界两侧共同评审。
- 无法确定开发 Issue、Issue 未关联 Proposal，或缺少实现所必需的契约时，不开始业务实现或 Bug 修复；先说明缺口并请求确认。
- 无法访问开发 Issue、Proposal 或其引用契约时，不依赖记忆或标题猜测内容；如实说明阻塞。
- 检查工作区已有改动，保留用户工作。明确、可逆的任务内步骤直接推进；关键歧义再澄清。

## 编码时

- 只完成当前任务，不擅自增加功能、更换技术路线或夹带无关重构、依赖升级及全仓库格式化。
- 修改限定在开发 Issue 声明的主模块和受影响模块内；不得为了绕过契约而把其他模块的逻辑复制进本模块。
- 遵守 CODING_STANDARDS.md，复用已有模块和接口，避免重复实现。
- 不编造 API、能力、日志或测试结果；缺少环境时先按工程流程准备，确实阻塞如实说明。
- 不提交密钥，不绕过权限，不关闭检查或降低质量门槛来让代码通过。
- 仅操作授权项目；被控制的目标仓库遵守其自身规则，不擅自覆盖其指令、配置和 CI。

## 交付前

- 按 CODING_STANDARDS.md 执行适用的格式检查、构建、测试及覆盖率验证。纯文档初始化说明不适用项；尚无检查命令时不编造命令。
- 未执行、失败、模拟和真实集成结果分别说明，不把启动或编译成功当作业务验收通过。
- 按 CONTRIBUTING.md 准备提交和 PR，说明改动、验证及限制。
- 远端提交、合并和部署按明确授权执行；不自批自合。
