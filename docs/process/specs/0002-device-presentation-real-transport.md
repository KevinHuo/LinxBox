# Device Presentation v1 真实传输与双向确认

| 字段 | 值 |
| --- | --- |
| Spec ID | `SPEC-0002` |
| 状态 | `Proposed` |
| 主模块 / Slice | `device` / Presentation wire + firmware integration |
| 上游架构/契约/ADR | `architecture.md`、`device-presentation/v1`、ADR-0005 |
| DRI | `待指派` |
| 必要 Reviewer | `device`、`linx`、`gateway`、`contracts`、Security |
| Integration Owner | `待指派` |
| 最近更新 | `2026-10-24` |

## 1. 定位、依赖和消费者

- 上游契约：`docs/contracts/device-linx.md` 语义基线；
- 下游消费者：ESP32 固件、灵矽 Presentation Bridge、Gateway presentation；
- 参考实现：`JuneLeGency/codex-passport` 的有界帧、ACK、加密 BLE、私有 relay 和真机验证；
- 接受阻塞项：真实灵矽 custom/WSS 能力、图片传输上限、DRI/Integration Owner、固件工程版本和安全评审。

## 2. 当前实现事实

- Issue #4 定义并待验收 240×320 三键离线 UI、任务/回复/图片/确认/回执状态机；数据为模拟内容。
- `codex-passport` 已公开验证 Hooks 通知经电脑 BLE、Android fallback 和可选私有 Wi-Fi relay 到 Passport；当前固件帧上限 2048 bytes，展示最多三个会话卡，标题和摘要有界。
- `codex-passport` 不发送回复、不续接会话、不批准动作，也未证明全文分页或图片传输。
- LinxBox 当前没有 Gateway、Desktop、灵矽 Bridge 或固件真实联网实现。

## 3. 目标与非目标

目标：冻结并实现任务卡、分页回复、受限图片、draft confirm、receipt、ACK/replay 和 snapshot 恢复的真实 wire profile，在目标硬件和真实灵矽链路通过验收。

非目标：不把 Hooks 当控制 API；不传完整 diff、代码、路径、日志或内部推理；不允许硬件批准 R2/R3；不承诺沿用 `codex-passport` wire format。

## 4. 权威边界与不变量

Task/Delivery 由 Gateway 掌握，本地执行由 Desktop 掌握，Device 只掌握 UI、草稿和缓存。MCP 与 Presentation 分离；确认前零执行；同一 command 至多一个 turn；sequence 缺口请求 snapshot。

## 5. 进入 Accepted 前必须冻结

- 灵矽 custom/WSS 的 envelope、最大帧、分块、ACK 和重传；
- 文本页和历史的数量、字节、保留期；
- RGB565 图片 variant、尺寸、压缩、分块、校验和淘汰；
- 加密、设备认证、token 轮换和 relay 网络边界；
- Hooks observer 与 app-server event 的 ownership/dedup key；
- 正反例 schema fixtures、DRI、必要 Reviewer 和 Integration Owner。

## 6. 失败、安全与恢复

未知版本/字段、超限 payload、校验失败、过期 command、目标变化和未确认草稿全部 fail closed。覆盖重复、乱序、超时、掉线、transport 切换、旧 callback、部分图片和固件重启。Relay 不暴露公网；设备内容不得包含本地路径、源码、凭据或 provider ID。

## 7. 验收证据

| 类型 | 必须证明 | 状态 |
| --- | --- | --- |
| Schema/Contract | 双向正反例、边界和兼容 fixture | Not run |
| Host/Firmware | parser、分页、按键、去重、乱序、内存水位 | Not run |
| Integration | 真实 Gateway/Linx/Desktop/app-server | Not run |
| Security/Fault | token、重放、断线、坏包、过限、回滚 | Not run |
| Real E2E | 真机全文、图片、确认、receipt、重试 | Not run |
| Soak | 30 分钟、100 导航、25 回复、资源无持续下降 | Not run |

## 8. 实现与验收记录

- 实现 PR/审阅 commit：未开始；
- 必要批准：未获得；
- 未覆盖风险：全部真实传输和双向控制门禁仍关闭。
