# LinxBox 客户端原型

这是本地静态界面原型，用于确认 **LinxBox** 作为 macOS 灵矽客户端的信息架构和交互方向。

界面围绕四个入口组织：

1. 首页：解释灵矽 MCP → LinxBox → Codex Desktop 的双向桥接；
2. MCP 连接：配置 WebSocket 地址、访问令牌和电脑名称；
3. 本地项目：选择允许 LinxBox 使用的项目；
4. 任务记录：查看 MCP 指令、Codex Desktop 状态和同步对话。

## 运行

```bash
python3 -m http.server 4173 --directory src/desktop/renderer-prototype
```

访问 `http://127.0.0.1:4173/`。

## 原型边界

- 所有连接、项目和任务均来自 `fixtures.js`；
- 不访问网络、文件系统、Keychain、SQLite、Electron IPC 或 Codex Desktop；
- “连接 MCP WebSocket”“添加项目”和启用开关只修改页面内存；
- 不能作为 MCP WebSocket、灵矽、固件或 Codex Desktop 已接入的证据；
- 当前目录保持本地，不进入 PR #8。
