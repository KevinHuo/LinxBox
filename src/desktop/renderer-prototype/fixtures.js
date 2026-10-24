window.LINXBOX_PROTOTYPE_FIXTURES = Object.freeze({
  activities: [
    { time: "14:32", title: "任务正在本机执行", detail: "商城前端 · 修复登录页空值错误", tone: "active" },
    { time: "13:08", title: "任务已完成", detail: "商城前端 · 解释缓存失效策略", tone: "done" },
    { time: "昨天", title: "本地项目已添加", detail: "订单服务已加入 LinxBox", tone: "quiet" },
  ],
  projects: [
    { id: "PRJ_SHOP", name: "商城前端", position: 0, conversations: 3, updated: "刚刚" },
    { id: "PRJ_API", name: "订单服务", position: 1, conversations: 2, updated: "昨天" },
  ],
  tasks: [
    { id: "TSK_84B2", title: "修复登录页空值错误", project: "商城前端", status: "running", statusLabel: "running", time: "今天 14:29", summary: "Codex app-server 正在执行 turn，PC Agent 通过 task.updated 回传 progress。", source: "task.run", thread: "codex-thread-7A2", turn: "turn-84B2", clientMessageId: "client-msg-17", messages: [{ role: "user", text: "修复登录页空值错误，并运行相关测试。" }, { role: "assistant", text: "已定位表单提交前的空值处理，正在修改并准备测试。" }] },
    { id: "TSK_612A", title: "解释缓存失效策略", project: "商城前端", status: "completed", statusLabel: "completed", time: "今天 13:08", summary: "turn/completed 已确认，最终 reply 已通过 task.updated 回传 Cloud。", source: "task.run", thread: "codex-thread-7A2", turn: "turn-612A", clientMessageId: "client-msg-16", messages: [{ role: "user", text: "解释当前项目的缓存失效策略。" }, { role: "assistant", text: "当前实现采用版本键与主动失效结合，写入成功后清除对应查询缓存。" }] },
    { id: "TSK_2C11", title: "补充订单状态测试", project: "订单服务", status: "interrupted", statusLabel: "interrupted", time: "昨天 18:22", summary: "收到 task.interrupt，Codex 返回 interrupted 后由 task.updated 回传。", source: "task.interrupt", thread: "codex-thread-91C", turn: "turn-2C11", clientMessageId: "client-msg-09", messages: [{ role: "user", text: "补充订单状态流转测试。" }, { role: "assistant", text: "任务已停止，没有继续写入文件。" }] },
  ],
});
