window.LINXBOX_PROTOTYPE_FIXTURES = Object.freeze({
  activities: [
    { time: "14:32", title: "任务正在本机执行", detail: "商城前端 · 修复登录页空值错误", tone: "active" },
    { time: "13:08", title: "任务已完成", detail: "商城前端 · 解释缓存失效策略", tone: "done" },
    { time: "昨天", title: "本地项目已添加", detail: "订单服务已加入 LinxBox", tone: "quiet" },
  ],
  projects: [
    { id: "PRJ_SHOP", name: "商城前端", path: "/Users/demo/Projects/storefront", enabled: true, updated: "刚刚" },
    { id: "PRJ_API", name: "订单服务", path: "/Users/demo/Projects/order-api", enabled: false, updated: "昨天" },
  ],
  tasks: [
    { id: "TSK_84B2", title: "修复登录页空值错误", project: "商城前端", status: "running", statusLabel: "Codex 编码中", time: "今天 14:29", summary: "Codex Desktop 正在修改登录表单，LinxBox 持续同步任务状态。", source: "MCP · code.run", thread: "codex-thread-7A2", messages: [{ role: "user", text: "修复登录页空值错误，并运行相关测试。" }, { role: "assistant", text: "已定位表单提交前的空值处理，正在修改并准备测试。" }] },
    { id: "TSK_612A", title: "解释缓存失效策略", project: "商城前端", status: "completed", statusLabel: "已回传", time: "今天 13:08", summary: "Codex Desktop 已完成回答，LinxBox 已将最终对话同步回 MCP WebSocket。", source: "MCP · chat.send", thread: "codex-thread-7A2", messages: [{ role: "user", text: "解释当前项目的缓存失效策略。" }, { role: "assistant", text: "当前实现采用版本键与主动失效结合，写入成功后清除对应查询缓存。" }] },
    { id: "TSK_2C11", title: "补充订单状态测试", project: "订单服务", status: "stopped", statusLabel: "已停止", time: "昨天 18:22", summary: "停止指令已转给 Codex Desktop，结束状态已同步回 MCP WebSocket。", source: "MCP · code.run", thread: "codex-thread-91C", messages: [{ role: "user", text: "补充订单状态流转测试。" }, { role: "assistant", text: "任务已停止，没有继续写入文件。" }] },
  ],
});
