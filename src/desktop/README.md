# LinxBox Desktop

macOS 本地客户端：Electron UI、Main/Daemon 进程边界、本地项目授权、任务列表与本地结果视图。

## 状态

**这是 Proposed 阶段的实现，不是已验收的客户端。**

- 开发 Issue：[Task #9 实现 LinxBox Desktop 本地客户端与 Companion Daemon](https://github.com/KevinHuo/LinxBox/issues/9)
- 执行 Spec：`SPEC-0003`（`Proposed`），状态未 `Accepted`
- 出站 Gateway 客户端（Slice 3）、Codex app-server adapter（Slice 4）、SQLite 持久化（Slice 2）**尚未实现**
- 本目录当前只提供：安全壳、IPC 边界、契约草稿切片与领域纯函数层

因此本目录里**没有**任何真实链路证据：没有连过真实 Gateway，没有驱动过真实 Codex app-server，没有创建过业务数据库。README 里写"能跑"指的是界面与本地逻辑能跑，不代表任务链路已打通。

## 目录结构

```text
src/desktop/
├── contracts/        契约草稿切片（SPEC-0001 占位，待迁出）
├── daemon/           领域纯函数层：delivery / 状态机 / policy / projector / registry / envelope / schema
├── main/             Electron 主进程：安全策略、IPC allowlist、本地状态
├── preload/          预加载桥接：固定方法集，无通用 invoke
├── renderer/         渲染层：展示模型 + 一次性用户意图
├── shared/           IPC 契约与脱敏日志
└── renderer-prototype/  早期静态原型（展示层已迁入 renderer/，待 Spec Accepted 后删除）
```

## 环境要求

- macOS
- Node.js >= 22.12（当前验证版本 22.22）
- pnpm 11（当前验证版本 11.5.2）

## 命令

```bash
cd src/desktop

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

未实现（属于后续 Slice）：

- 出站 Gateway WSS 客户端、hello/heartbeat/sequence、重连重放
- Codex app-server stdio adapter、thread/turn 生命周期
- SQLite 持久化与真实 schema 迁移执行
- 真实 macOS + 真实 app-server 的端到端验收

## 已知限制

- `connection.test` 会如实返回"出站 Gateway 客户端尚未实现"，不会伪造握手成功
- 本地状态保存在内存，进程退出即丢失；数据库路径在诊断页显示为"尚未创建"
- 项目路径显示时折叠家目录并只保留末两段
- `renderer-prototype/` 与 `renderer/` 的展示层存在重复，等 Spec Accepted 后逐文件复核迁移并删除原型

## 已验证与未验证

已验证（本机，2026-10-24）：

- `pnpm check` 全绿：Prettier、ESLint（type-aware）、`tsc --noEmit`、281 个测试
- 覆盖率门槛通过：语句 100%、行 100%、函数 100%、分支 99.41%
- `pnpm dev` 启动后主窗口就绪、渲染进程加载完成、preload 无报错、页面无 CSP 警告
- `pnpm package` 产出 `release/mac-arm64/LinxBox.app`，该产物可启动且 renderer 从 `app.asar` 内正常加载

未验证：

- 真实 Gateway 连接、真实 Codex app-server、真实 SQLite 持久化、真实 macOS 端到端任务链路
- 安装包签名与 notarize
- 其他 macOS 架构与系统版本
