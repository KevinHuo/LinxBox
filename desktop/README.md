# LinxBox Desktop

macOS 本地客户端：Electron UI、Main/Daemon 进程边界、本地项目授权、任务列表与本地结果视图。

## 状态

**这是 Proposed 阶段的实现，不是已验收的客户端。**

- 开发 Issue：[Task #9 实现 LinxBox Desktop 本地客户端与 Companion Daemon](https://github.com/KevinHuo/LinxBox/issues/9)
- 执行 Spec：`SPEC-0003`（`Proposed`），状态未 `Accepted`
- 出站 Gateway client、Codex app-server adapter 和 Electron UI 已有本地实现与自动化测试
- Electron main 尚未正式接线 Gateway client；SQLite schema 已定义但尚未执行真实持久化

当前真实驱动过本机 Codex app-server，并通过 mock Gateway 验证 WSS 链路；没有连过真实云端
Gateway，也没有创建业务数据库。这些本地结果不能描述为生产链路已经验收。

## 目录结构

```text
desktop/
├── contracts/        契约草稿切片（SPEC-0001 占位，待迁出）
├── daemon/           Gateway/Codex adapter 与领域层：delivery / policy / projector / registry / schema
├── main/             Electron 主进程：安全策略、IPC allowlist、本地状态
├── preload/          预加载桥接：固定方法集，无通用 invoke
├── renderer/         渲染层：展示模型 + 一次性用户意图
└── shared/           IPC 契约与脱敏日志
```

## 环境要求

- macOS
- Node.js >= 22.12（当前验证版本 22.22）
- pnpm 11（当前验证版本 11.5.2）

## 命令

```bash
cd desktop

pnpm install        # 安装依赖
pnpm check          # 格式检查 + lint + 类型检查 + 测试与覆盖率
pnpm dev            # 构建后启动 Electron 窗口
pnpm build          # 构建 dist/ 与 dist-preload/
pnpm package        # 构建未签名的 macOS 开发包（release/mac-arm64/LinxBox.app）
```

`pnpm package` 产出的是**未签名、未 notarize** 的开发包，只用于本机验证。代码签名与最低安全版本属于功能启用门禁，不在本阶段。

单项命令：

```bash
pnpm format:check   # Prettier
pnpm lint           # ESLint（type-aware）
pnpm typecheck      # tsc --noEmit
pnpm test           # Vitest
pnpm coverage       # Vitest + 覆盖率门槛（lines/branches/functions/statements 均 >= 90%）
```

## 测试覆盖率

当前 Desktop 覆盖率由 `pnpm coverage` 生成，统计范围为 `desktop/` 内纳入 Vitest 配置的自有可测代码；不代表固件、Gateway 或真实端到端链路覆盖率。

| 指标       | 覆盖率 |
| ---------- | -----: |
| Statements | 98.35% |
| Branches   | 93.48% |
| Functions  | 98.55% |
| Lines      | 98.33% |

最近验证：2026-10-24，24 个测试文件通过、1 个跳过；485 个测试通过、3 个跳过。覆盖率报告为本地生成物，不提交到仓库。

首次安装若 pnpm 未执行 Electron 的 postinstall（下载预编译二进制），手动补一次：

```bash
node node_modules/.pnpm/electron@*/node_modules/electron/install.js
```

## 安全边界

这一层是客户端的核心约束，实现位置与负例：

| 约束                                                | 实现                                                                                                         | 测试                                                           |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------- |
| Renderer 无 Node、无 token、无文件系统、无子进程    | `main/security.ts`：`sandbox: true`、`contextIsolation: true`、`nodeIntegration: false`、`webviewTag: false` | `main/security.test.ts`                                        |
| 拒绝窗口导航与新开窗口                              | `isNavigationAllowed` / `isWindowOpenAllowed` / `toExternalOpenUrl`（只放行 `https` 给系统浏览器）           | `main/security.test.ts`                                        |
| 拒绝一切权限请求                                    | `isPermissionRequestAllowed` 恒为 `false`                                                                    | `main/security.test.ts`                                        |
| IPC 具名 allowlist，运行时校验，拒绝未知字段        | `shared/ipc.ts` 的 `parseIpcRequest`                                                                         | `shared/ipc.test.ts`                                           |
| 预加载桥接不暴露 `ipcRenderer`，也没有通用 `invoke` | `preload/api.ts`                                                                                             | `preload/api.test.ts`                                          |
| 令牌不回显                                          | `readConnectionView` 只给掩码提示                                                                            | `main/desktop-state.test.ts`                                   |
| 项目授权只能来自系统目录选择器                      | `projects.grant` 的请求体**不含路径**，路径由主进程 `dialog.showOpenDialog` 取得后 `realpath`                | `shared/ipc.test.ts`（负例：带 `rootPath` 字段的授权请求被拒） |
| 日志不含令牌与绝对路径                              | `shared/logger.ts` 的 `redactValue` / `redactText`                                                           | `shared/logger.test.ts`                                        |

## 契约来源

当前 Gateway ↔ Desktop 输入、输出、示例和实际接入范围见
[Gateway / Desktop API（当前实现整理）](GATEWAY_API.md)。该文档只整理当前代码事实，不建立或修改共享契约。

`contracts/` 是 SPEC-0001 的**本地占位切片**，内容转写自 SPEC-0003 起草的 MVP 契约基线（`feat/9-desktop-client` 分支上的 `docs/contracts/`）。

- 状态是 `Proposed`，**不是权威契约**
- SPEC-0001 落地后，`contracts/` 必须改为引用共享 schema，而不是保留第二份事实源
- 契约标识：`Gateway/Desktop 1.0`、`task-domain/v1`、`task-result/v1`

## 已实现 / 未实现

已实现：

- TypeScript strict + pnpm + Electron 工具链，统一检查与覆盖率命令
- Electron 安全壳、preload 桥接、渲染层与"设置与诊断"页
- IPC allowlist 与运行时校验（含越权负例）
- 契约草稿切片：`Gateway/Desktop 1.0` 帧、`task-domain/v1` 状态机、`task-result/v1`
- 领域纯函数层：delivery 去重与 disposition、任务状态转换、grant/realpath/R0-R3 授权、三级结果投影、项目注册表、入站帧校验、SQLite schema 与迁移
- **真实 Codex 接入**：`codex-adapter` 用 stdio JSON-RPC 驱动本机 Codex app-server（initialize / thread start-resume / turn start-interrupt / 事件映射 / 崩溃收敛），不读 Codex 私有数据库
- **真实 Gateway 链路**：出站 WSS 客户端 + `task.dispatch` 调度 + mock Gateway 联调脚本
- **本机会话浏览**：读 Codex 的会话列表与聊天记录（`thread/list` / `thread/read`），**本地读取、不调用模型，没有额度也能用**

未实现（属于后续 Slice）：

- 真实云端 Gateway（当前只有本地 mock）
- SQLite 真实持久化与迁移执行
- 结果投影接入真实任务（`result-projector` 已就绪，尚未接上真实 turn 的产物）

## 真实 Codex 接入

### 探针：Codex 到底能给我们什么

```bash
pnpm build
pnpm probe:codex        # 写入 codex-capabilities.json
```

app-server 没有自描述的 method list，但对一个不存在的方法发请求时，它会在错误信息里回
`expected one of ...`，把**完整支持的方法集合**吐出来。探针用这个拿权威目录。

本机 `codex-cli 0.164.0-alpha.11.1` 的观测结果：

| 分类       | 方法数 | 代表方法                                                                                                                                 |
| ---------- | ------ | ---------------------------------------------------------------------------------------------------------------------------------------- |
| 会话与轮次 | 61     | `thread/start`、`thread/resume`、`turn/start`、`turn/interrupt`、`turn/steer`、`thread/turns/list`、`thread/items/list`、`thread/search` |
| 扩展与插件 | 36     | `plugin/list`、`skills/list`、`mcpServerStatus/list`、`hooks/list`                                                                       |
| 文件与进程 | 23     | `fs/readFile`、`fs/readDirectory`、`process/spawn`、`command/exec`                                                                       |
| 账号与额度 | 21     | `account/read`、`account/rateLimits/read`、`account/usage/read`、`getAuthStatus`                                                         |
| 模型与配置 | 18     | `model/list`、`config/read`、`permissionProfile/list`                                                                                    |
| 项目与记忆 | 14     | `project/list`、`threadSection/list`、`memory/status`                                                                                    |
| 其他       | 6      | `server/diagnostics`、`environment/info` 等                                                                                              |

通知侧观测到 14 种，包括 `thread/started`、`turn/started`、`item/started`、`item/completed`、
`turn/completed`、`error`、`warning`、`account/updated`、`account/rateLimits/updated`、
`thread/status/changed`、`thread/readState/changed`、`mcpServer/startupStatus/updated`、
`configWarning`、`remoteControl/status/changed`。

`item/completed` 的 `item.type === 'agentMessage'` 时携带真实文本，`phase` 为 `commentary` 是
过程解说，`final_answer`（或无 phase）是最终回答。

### 读 Codex 的会话与聊天记录

页面上「Codex 会话」那一页就是干这个的：左边列本机 Codex 已知���会话，右边显示选中的
会话的真实聊天记录（用户消息与 Codex 回复，含 `commentary` 过程解说）。

对应 app-server 方法：

| 方法                | 用途                                    | 是否吃额度 |
| ------------------- | --------------------------------------- | ---------- |
| `thread/list`       | 会话列表（标题、cwd、模型、时间、状态） | 否         |
| `thread/read`       | 单个会话的完整 transcript               | 否         |
| `thread/turns/list` | 按轮次取历史（`itemsView: 'full'`）     | 否         |
| `turn/start`        | 发起新一轮对话                          | 是         |

前三个都是 provider 的本地存储读取，**账号没额度时照样能用**。只有 `turn/start` 需要模型。

会话内容是用户自己的对话，只留在这台机器上，不进任何上行帧。

### 远端 WSS 命令如何驱动本地 Codex

链路是真的：真实 WebSocket over TCP，真实本机 Codex app-server 子进程，真实
Gateway/Desktop 1.0 帧格式。

```bash
pnpm build
pnpm e2e:gateway-codex   # mock Gateway(云端) -> Desktop -> 本机 Codex -> 回传
pnpm mock:gateway        # 单独跑 mock Gateway，供 Electron 客户端联调
```

**远端下发的帧格式**（`Gateway/Desktop 1.0`）：

```json
{
  "protocol_version": "1.0",
  "message_id": "msg_01H...",
  "sequence": 1043,
  "type": "task.dispatch",
  "sent_at": "2026-10-24T01:00:00Z",
  "expires_at": "2026-10-24T01:30:00Z",
  "payload": {
    "task_id": "tsk_01H...",
    "project_id": "prj_01H...",
    "agent": "codex",
    "capabilities": ["read", "write", "test"],
    "prompt": "修复登录页错误并运行测试",
    "acceptance_criteria": ["空值提交不再抛错"],
    "policy_version": "2026-10-24",
    "continuation": null
  }
}
```

远端只给 `project_id`，**绝对路径永远由本机解析**。Desktop 回传：

```json
{ "type": "task.ack",  "payload": { "message_id": "...", "sequence": 1043, "task_id": "...", "disposition": "accepted" } }
{ "type": "task.started",  "payload": { "task_id": "...", "event_sequence": 1, "state": "running", "stage": "preparing", "...": "" } }
{ "type": "task.completed", "payload": { "task_id": "...", "event_sequence": 2, "state": "succeeded", "result": { "contract_version": "task-result-v1", "...": "" } } }
```

`disposition` 取 `accepted | duplicate | expired | rejected`；`accepted` 只表示"已落盘并通过
初检"，不等于 started 或 succeeded。同一个 `message_id` 重放返回 `duplicate`，绝不创建第二个
turn。

### 本机接入的安全边界

| 约束                  | 实现                                                                                                       |
| --------------------- | ---------------------------------------------------------------------------------------------------------- |
| 不读 Codex 私有数据库 | 适配器只走 stdio JSON-RPC，不碰 `~/.codex` 的任何文件                                                      |
| 不经 shell 启动       | 参数是数组，`shell: false`                                                                                 |
| 环境白名单            | 子进程只继承 `PATH`/`HOME`/`TMPDIR`/`USER`/`LANG`/`TERM`/`CODEX_HOME`；token、cookie、`LINXBOX_*` 全部剔除 |
| stderr 不外传         | 只进本地环形缓冲，不进任何上行帧                                                                           |
| 帧大小有上限          | 单帧超过 16 MB 视为协议错误，不做无界缓冲                                                                  |
| 服务端请求一律拒绝    | app-server 的审批请求（`item/*/requestApproval`）回 `-32601`，本客户端没有可信审批面                       |
| 终态不可证明就中断    | 只有 provider 明确 `completed` 才算 succeeded，其余（含未知状态）收敛为 `interrupted`                      |

## 已知限制

- `connection.test` 会如实返回"出站 Gateway 客户端尚未实现"，不会伪造握手成功
- 本地状态保存在内存，进程退出即丢失；数据库路径在诊断页显示为"尚未创建"
- 项目路径显示时折叠家目录并只保留末两段

## 已验证与未验证

已验证（本机，2026-10-24）：

- `pnpm check` 全绿：Prettier、ESLint（type-aware）、`tsc --noEmit`、281 个测试
- 覆盖率门槛通过：语句 100%、行 100%、函数 100%、分支 99.41%
- `pnpm dev` 启动后主窗口就绪、渲染进程加载完成、preload 无报错、页面无 CSP 警告
- `pnpm package` 产出 `release/mac-arm64/LinxBox.app`，该产物可启动且 renderer 从 `app.asar` 内正常加载

未验证：

- 真实云端 Gateway 连接、真实 SQLite 持久化、真实 macOS 全链路任务验收
- 安装包签名与 notarize
- 其他 macOS 架构与系统版本
