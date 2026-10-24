---
name: linxbox-contracts
description: |
  Use this skill when working inside the LinxBox project
  (/Users/tongjie/Desktop/LinxBox) — implementing features, fixing bugs, writing
  schemas/contracts, filing Issues/Proposals/Execution Specs/PRs, or reviewing changes.
  按仓库已接受契约（docs/contracts/、docs/adr/）与质量门禁（docs/quality-gates.md）
  执行：先定位开发 Issue 与关联 Proposal，读契约再动手，不静默改协议、状态机、
  错误码、字段名或版本号，交付前按证据分级逐条核对。
  Triggers on phrases like: LinxBox、灵矽、林碶、MCP 工具、list_projects、start_task、
  task-result-v1、task-domain/v1、device-presentation/v1、linxbox.mcp/v1、
  Gateway/Desktop WSS、Codex app-server、Desktop Daemon、Execution Spec、质量门禁、
  证据分级、契约、schema、Proposal #1、Task Issue。
  Do NOT use for unrelated coding or voice-agent work in other repos (miku-rti,
  mini-rti, UniSpeaking, rti-project, agent 体验中心)。
---

# LinxBox 契约执行

## 共享边界

本 skill 随仓库分发，是团队共享的助手工作规范：把 `AGENTS.md` 与 `docs/` 里已接受的契约读成可执行步骤，少走弯路。契约的权威仍在仓库文档，本 skill 只负责"怎么读、怎么查、怎么判断合格"。

- 权威顺序不变：仓库 `AGENTS.md` 与 `docs/` 优先于本 skill；两者表述冲突时以仓库为准，并把差异指出来。
- 本 skill 里的每条规则都应能在仓库文档里找到出处；发现只有本 skill 有的规则，说明它缺一份仓库依据，应先补进仓库。
- 无论用 Claude Code、Codex 还是其他助手加载本 skill，约束层级、产出格式和门禁要求相同；工具不同不改变交付物是否合格。
- 修改本 skill 属于仓库维护：改了共享规则要同步 `AGENTS.md` 或 `docs/`，并按 `CONTRIBUTING.md` 走 PR。

## Inputs to collect

- **开发 Issue 编号**。用户指定了就用它；没指定就从当前 PR 的关联项和分支名里的编号推断（分支名形如 `docs/5-contract-baseline` → Issue #5）；仍推断不出来就问用户，不要挑一个"看起来相关"的 Issue，因为仓库规范要求每次交付绑定一个明确的开发 Issue。
- **是否触及公共契约**。协议、状态权威、持久化、失败语义、运行时职责、安全边界、跨模块行为、公共 schema，任一命中就进入"改契约的流程"。
- **仓库与分支**。契约与 Issue 的唯一落点是私有 upstream `KevinHuo/LinxBox`；`origin`（`yydounai1234/LinxBox`）的 Issues 已禁用，只能当提交镜像。

## 权威层级（冲突时从高到低）

1. 用户对当前工作的明确要求。
2. 已接受的仓库契约与 ADR：`docs/architecture.md` → `docs/contracts/*` → `docs/adr/*`。
3. 当前开发 Issue（Task/Bug）中与契约一致的目标、范围、非目标和验收条件。Issue 可以细化上层契约，不能扩大或改变其范围。
4. Issue 关联的 Proposal，作为产品背景与追踪信息，不覆盖已接受契约。
5. 相关测试。
6. 现有实现与 README 概述。

历史 Proposal 与仓库契约冲突时，以仓库契约为实现依据，并在交付说明里指出差异。用户没明确要求就不要改 Issue 正文；用户要求的范围变化，先改 Issue 再改代码，并留一条范围变更评论记录原因。

## 开始前

1. 读 `docs/README.md` 索引，按任务范围读对应契约、模块文档和 ADR。冻结事实速查见 `references/contracts-cheatsheet.md`，但它只是索引，字段语义以仓库文档为准。
2. 确认主模块和受影响模块。一个 Task 只能有一个主模块；跨模块契约变更由边界两侧共同评审。
3. 用一句话复述目标，列出范围、非目标和可检查的验收条件，并把每条验收条件映射到具体实现或检查项。
4. 检查工作区已有改动，保留用户工作。

### 跨契约交叉核对

同一个概念常常同时出现在多份契约里（turn 与 lineage、receipt、幂等键、sequence、保留期、"最近八轮"）。只读一份就下结论，容易把契约里并存的两种读法当成一种：

- 把涉及的概念在四份契约 + `docs/architecture.md` + 相关模块文档里一起检索，列出各自怎么说的。
- 发现两种读法并存时，把两种读法和各自影响面一起交给维护者裁定，不要自己选一个写进方案。已知的未冻结点列在 `references/contracts-cheatsheet.md` 的"已知的未冻结点"。
- 同一个语义字段可能有多个载体，跨通道、跨实体各有一份（例：摘要类字段在结果契约、progress 通知、Desktop→Gateway 事件、Task 实体、状态查询结果里各有一个出口）。定规则时要确认覆盖了**所有载体**——只管住其中一个，实现方就会把规则理解成"那一个字段的事"，剩下出口各写各的。写规则前先把这个字段的全部出口列出来。
- 新的公共标识跨越边界时，先确认它是否在 `docs/contracts/domain.md` §1 的值对象清单里（`DeviceId`、`ClientId`、`ProjectId`、`TaskId`、`MessageId`、`RequestId`、`ProviderThreadId`、`ProviderTurnId`）。不在清单里就是契约缺口，要显式提出，而不是在实现里临时造一个裸 string。

### 仓库当前没有实现代码

`apps/`、`packages/`、`plugins/`、`tests/` 目前只有空骨架（`.gitkeep`），且 SPEC-0001 的工具链选型（D-2 校验库、D-3 类型生成、D-4 测试与覆盖率工具）还没锁定。这种状态下：

- 交付物是契约文本 + Task Issue + 关联 Proposal + Execution Spec + 验收条件，不是代码。
- 不要写依赖具体库 API 的实现，也不要在仓库里预置 `package.json`、`tsconfig.json`、`pnpm-workspace.yaml` 或 CI——那些属于 SPEC-0001 Slice 1 的工具链 bootstrap，选型锁定前进入仓库就是替维护者做了决定。
- 文件清单可以按 `docs/code-structure.md` §2 的冻结目录树给出落位示意，但要注明实际布局以 Slice 1 的结果为准。

缺少实现所必需的契约、Issue 未关联 Proposal、或 Issue 与契约互相矛盾时，先说明缺口并请维护者裁定，不要靠推断先写实现。

## 硬约束

这些是 `docs/architecture.md` §1 的不可退化条件，违反即改变产品形态，不是实现细节：

- 用户不部署、不配置、不暴露本地 MCP Server。
- 设备路由来自 Gateway 可信绑定，不来自模型参数。
- Desktop 是本地资源的最终授权边界；云端允许投递不代表本地必须执行。
- 远端主动编码只走 Desktop Daemon + Codex app-server。
- MCP 控制通道与 Device Presentation 通道相互隔离，展示消息不能绕过任务授权。
- 本地 MCP/插件只做可选工具、Skill 和 Hooks，不能反向启动当前 Codex Desktop 对话。
- 不使用 UI 自动化、截图或私有数据库控制官方 Codex Desktop。
- 代码、绝对路径、完整 diff、原始日志和 Codex 凭据默认不离开用户电脑。

具体到编码时：不要自行改名消息，不要混合控制与展示通道，不要添加绕过 Gateway/Desktop 的路径，不要把 app-server 原始事件暴露为公共契约，不要复制别的模块的状态机或 DTO。

## 改契约的流程

改动公共 MCP、Device Presentation、Gateway/Desktop、任务状态、身份授权、Desktop 协议、Linx progress 路由或 Codex 集成方式时，顺序是：

1. 先改 `docs/contracts/`（影响不变量或技术路线时新增 ADR），再改实现。
2. 版本策略：新增 optional 字段可在同一主版本演进；删除、改名、改变类型或改变语义必须升约主版本。
3. 同步 schema fixtures、producer/consumer 契约测试和迁移说明。
4. 安全请求默认拒绝未知字段；只有显式声明可扩展的 metadata 容器允许未知键。
5. 更新 `docs/README.md` 索引与受影响的模块文档。

加法也是契约变更，容易被当成"顺手加个字段"。扩展已接受契约的枚举或取值集合时（新增工具名、新增 `speaker` 值、新增状态取值）：同一主版本内演进，但要同时给出旧 consumer 遇到未知值时的行为（忽略而非报错）、正反例 fixture 和兼容说明段落，并同步所有提到该集合的文档——包括 Spec 里的"六个工具"这类写死的计数。

改动协议、状态权威、持久化、失败语义、运行时职责、安全边界或跨模块行为时，先按 `docs/specs/README.md` 建 bounded Spec。`Proposed` 状态不授权实现，接受动作需要 DRI、决策、契约和必要评审都冻结。

## 产出格式

条目和文档都按仓库模板分段，模板原文在 `.github/ISSUE_TEMPLATE/` 和 `docs/specs/template.md`。

| 产出 | 章节骨架 | 位置 |
| --- | --- | --- |
| Task / Bug Issue | 关联 / 目标与范围（含显式非目标）/ 依赖与运行条件 / 验收标准（可勾选、可检查）/ 验证方式 | `.github/ISSUE_TEMPLATE/task.md`、`bug.md` |
| Proposal | 背景与目标 / 本次交付范围 / 实现思路 / 任务拆分 / 验收标准 / 待确认事项 | `.github/ISSUE_TEMPLATE/proposal.md` |
| Execution Spec | `docs/specs/template.md` 全部 15 节；新增后同步 `docs/specs/README.md` 索引表（状态、主模块、DRI、下一门禁） | `docs/specs/` |
| PR | 任务（`Closes`/`Refs`）/ 契约对应 / 模块影响 / 改动 / 验证 / 限制 | `.github/pull_request_template.md` + `CONTRIBUTING.md` 六项 |
| Commit | `type(scope): 简述`；scope 用 `device`/`linx`/`gateway`/`desktop`/`codex`/`contracts`/`quality`，纯项目维护用 `project` | `CONTRIBUTING.md` |

写法要点：

- 验收条件写成可勾选、可核对的行为，例如"两个 device ID 只能路由到各自绑定的 Desktop"，而不是"功能正常"。
- 非目标显式列出，防止后续 PR 夹带。
- 验证方式区分"实际执行过的命令"与"不适用项"（纯文档阶段写不适用，并说明原因，不要编造构建/测试命令）。
- 流程顺序：先提 Task Issue，再提关联 Proposal，最后互相回填编号。远端提交、合并、部署按用户明确授权执行，不自批自合。

## 交付门禁

合并前逐条核对（来自 `docs/quality-gates.md`）：

- Spec / Task / PR 的目标、非目标和验收条件三者一致。
- 公共 schema 有正反例 fixture、producer/consumer 契约测试和兼容说明。
- 格式、lint、typecheck、构建、单元测试、增量及关键模块覆盖率通过。自有可测代码的整体、增量及关键模块覆盖率分别不低于 90%（`CODING_STANDARDS.md`）。
- 安全边界包含越权、路径逃逸、重放、乱序、payload 上限和脱敏负例。
- 模拟、真实集成、未执行项分别报告，不把启动或编译成功当作业务验收通过。
- 文档链接、示例、生成来源、许可证和敏感信息检查通过。
- 必要的 DRI、模块 Reviewer 和跨边界 Reviewer 已记录。

证据分级要分开报：`docs/schema`、`unit`、`contract`、`integration`、`fault/security`、`simulated-e2e`、`real-e2e`、`device-soak`，低等级不能替代高等级。生成固件不等于刷写成功，桌面截图不等于真机可读，mock app-server 不等于真实 Codex 可用。

合并门禁与功能启用门禁是两件事：默认关闭的新通道不因为代码合并就自动启用；启用前需要目标环境兼容性、迁移/回滚、真实依赖、容量水位、指标告警 runbook、安全审查和 owner 签字。

## 仓库操作要点

- 提 Issue 一律 `gh issue create -R KevinHuo/LinxBox`；跨 fork 开 PR 用 `gh pr create -R KevinHuo/LinxBox --base main --head yydounai1234:<branch>`。
- 目录树本身是冻结契约（`docs/code-structure.md` §2）。新增目录要同步该文档，并说明归属哪个 Spec。
- Markdown 行尾两空格是硬换行（metadata 头），仓库用 `.gitattributes` 的 `*.md whitespace=-trailing-space` 把它排除在 `git diff --check` 之外。
- 纯文档阶段没有构建、测试、覆盖率命令，不要编造；按 `docs/quality-gates.md` §2 记 `Spec: N/A` 并说明理由。
- 规范文档头部字段：常规文档用 `Status` / `Version` / `Last updated`，ADR 用 `Status` + `Date`，契约文档额外带 `Contract` 或 `Protocol version`。
- 交付前跑契约一致性自检：`node .minimax/skills/linxbox-contracts/scripts/check-docs.js .`（在仓库根执行；脚本随本 skill 分发，校验相对链接与锚点、UTF-8、契约标识与工具名一致性、状态机一致性、目录树与 `docs/code-structure.md` 的一致性）。

## 常见错误

这些是真实踩过、且能被检查脚本发现的：

- **锚点指错节号**：跨文档引用落到错误标题（如指向"风险能力"而不是结果契约）。脚本会报锚点未命中。
- **标识与拼写漂移**：六个 MCP 工具名、五个版本标识（`linxbox.mcp/v1`、`device-presentation/v1`、`task-domain/v1`、`task-result-v1`、Gateway/Desktop `1.0`）在各文档写法不一致。
- **目录树与实际目录不一致**：只建目录没同步文档，或只改文档没建目录。
- **死链**：Issue 或文档引用了仓库里还不存在的文件（如 `docs/` 未入库时写 `blob/main/docs/...`）。入库前用"随 Task #N 入库"措辞代替链接。
- **用截断代替脱敏**：截断路径会留下半截路径（`/Users/tongjie/Des`），仍然是路径。脱敏要用受控模板或整段替换，不发原文；也不要用正则黑名单逐个删路径，改成"只允许由受控词表拼装"的正向产出规则。
- **范围与 Issue 非目标冲突**：Issue 写了"不创建某目录"，实现却建了。先改 Issue 并留评论，再改代码。

## Output contract

交付时给出：做了什么、引用了哪些契约（精确到文件与章节）、实际执行的验证命令与结果、未执行项与已知限制。每条结论落到文件路径，不写"按契约处理"这类无法核对的话。范围、契约或 ADR 需要变更时，说明这触发了契约变更流程，并指出该走哪一步。

## Failure handling

- 定位不到开发 Issue、Issue 没关联 Proposal、或缺实现所需的契约 → 说明缺口并请维护者确认，先不写业务实现。
- 发现契约自相矛盾或实现需要改已接受契约 → 指出矛盾位置，给出候选方案与影响面，交维护者裁定；不要自行选一个改掉，因为契约是跨模块共享的决策。
- 门禁过不了且改不动 → 如实报告未通过的项和原因，不降低门槛、不跳过检查、不伪造结果。

## Examples

**输入**：帮我在 LinxBox 加一个 MCP 工具 `list_task_history`，查询某个任务最近 8 轮对话历史。

**正确路径**：先定位开发 Issue；没有就按模板提 Task Issue 并关联 Proposal。读 `docs/contracts/mcp.md` 确认新工具属于公共契约、需同步 `docs/contracts/README.md` 版本与 fixtures；读 `docs/contracts/device-linx.md` 确认"最近八轮"是设备侧保留上限、云端保留与分页上限由实现 Spec 固定，属于待冻结项；确认隐私边界（历史内容要走 device-safe 投影，不能带绝对路径与 diff）。改动落在 `contracts` 主模块、`gateway`/`desktop` 受影响，属公共契约变更 → 先按 `docs/specs/template.md` 建 Spec。

**输入**：修复 Desktop 重复执行同一任务的问题。

**正确路径**：读 `docs/contracts/gateway-desktop.md` §6，ACK 只证明已持久化，Desktop 按 `message_id` 至多执行一次，重连从 `last_acked_sequence + 1` 重放。定位实际缺陷（本地持久化或去重逻辑），在 `desktop` 主模块修复并补回归测试；不改协议语义，不改 `task-domain/v1` 状态机。
