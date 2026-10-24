# `device`：AI Passport / ESP32

> Status: accepted  
> Version: 1.0  
> Last updated: 2026-10-24  
> Owner: `OWNERS.md` 中的 `device`

## 责任

目标硬件为 ESP32-C3、8 MB Flash、无 PSRAM、240×320 RGB565 竖屏和 UP/DOWN/OK 三个按键。Device 提供任务阅读和明确确认入口，不判断云端或 Agent 的真实状态。

必须实现：

- 展示有界任务列表、选中态、未读和连接状态；
- 展示任务摘要、分页后的用户可见回复和最近八轮有界历史；
- 展示经过设备适配的图片总览与预设局部，保留 task/turn/image 归属；
- UP/DOWN 选择或翻页，短按 OK 执行，长按 OK 返回；禁止长按释放触发短按、首尾循环和页面穿透；
- 输入开始时冻结 `task_id`、`turn_id` 和可选 `image_id`，显示转写与目标，只有明确确认才发送；
- 区分 sending、desktop_received、queued、agent_accepted、running、turn_completed 和 failed 回执；
- 对 `message_id` 去重；断线、失败和重试保留同一 draft/command 身份，不自动重发；
- 每任务最多一条待执行指令；恢复初始场景清理缓存、草稿、队列、未读和旧计时事件。

## 状态所有权

Device 只拥有当前页面、选中项、页码、图片位置、未确认草稿和有界展示缓存。Task、Turn 和 Delivery 状态只显示服务端投影，不能由页面跳转或动画反向修改。

## 输入与输出

- 输入：[`device-presentation/v1`](../contracts/device-linx.md) snapshot/delta/content/image/receipt/error；
- 输出：同契约的 sync、content request、draft submit/confirm/cancel、retry 和 stop intent；
- 不输出：本地路径、Codex/provider ID、权限决定、完整 diff、原始日志或凭据。

## 门禁

- 主机测试覆盖导航、分页、上下文冻结、按键抑制、去重、乱序、旧事件和恢复；
- 固件资源、字体、Flash/RAM 水位和镜像 SHA-256 有证据；
- 真机检查圆角裁切、中文可读性、100 次导航、25 次回复及 30 分钟稳定性；
- 离线演示证据与真实联网证据分开报告，模拟数据必须显著标识。
