(() => {
  "use strict";

  const fixtures = window.LINXBOX_PROTOTYPE_FIXTURES;
  const labels = {
    overview: ["LINXBOX HOME", "你好，这里是 LinxBox"],
    lingxi: ["MCP WEBSOCKET", "连接 MCP WebSocket"],
    projects: ["LOCAL PROJECTS", "本地项目"],
    tasks: ["TASK HISTORY", "任务记录"],
  };
  const state = {
    connected: false,
    projects: fixtures.projects.map((project) => ({ ...project })),
    runningOnly: false,
    selectedTask: fixtures.tasks[0]?.id,
  };

  const qs = (selector, root = document) => root.querySelector(selector);
  const qsa = (selector, root = document) => [...root.querySelectorAll(selector)];
  const escapeHtml = (value) => String(value).replace(/[&<>"']/g, (character) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[character]);

  function showToast(message) {
    const toast = qs("#toast");
    toast.textContent = message;
    toast.classList.add("is-visible");
    clearTimeout(showToast.timer);
    showToast.timer = setTimeout(() => toast.classList.remove("is-visible"), 2600);
  }

  function switchView(view) {
    if (!labels[view]) return;
    qsa(".nav-item").forEach((button) => {
      const selected = button.dataset.view === view;
      button.classList.toggle("is-active", selected);
      if (selected) button.setAttribute("aria-current", "page");
      else button.removeAttribute("aria-current");
    });
    qsa(".view").forEach((panel) => panel.classList.toggle("is-active", panel.dataset.viewPanel === view));
    qs("#view-kicker").textContent = labels[view][0];
    qs("#view-title").textContent = labels[view][1];
    qs("#workspace").focus({ preventScroll: true });
  }

  function renderActivities() {
    qs("#activity-list").innerHTML = fixtures.activities.map((activity) => `
      <li class="activity-row tone-${escapeHtml(activity.tone)}">
        <span class="activity-dot" aria-hidden="true"></span>
        <div><strong>${escapeHtml(activity.title)}</strong><p>${escapeHtml(activity.detail)}</p></div>
        <time>${escapeHtml(activity.time)}</time>
      </li>
    `).join("");
  }

  function renderProjects() {
    qs("#project-grid").innerHTML = state.projects.map((project) => `
      <article class="project-card ${project.enabled ? "is-enabled" : ""}">
        <div class="project-topline"><span>${escapeHtml(project.id)}</span><span>${escapeHtml(project.updated)}</span></div>
        <div class="folder-mark" aria-hidden="true"><i></i></div>
        <h3>${escapeHtml(project.name)}</h3>
        <p class="mock-path">${escapeHtml(project.path)}</p>
        <div class="project-footer">
          <span>${project.enabled ? "可供灵矽使用" : "当前已停用"}</span>
          <button type="button" class="switch ${project.enabled ? "is-on" : ""}" data-toggle-project="${escapeHtml(project.id)}" aria-pressed="${project.enabled}"><i></i><span>${project.enabled ? "已启用" : "已停用"}</span></button>
        </div>
      </article>
    `).join("");
    qs("#hero-project-count").textContent = `${state.projects.filter((project) => project.enabled).length} 个`;
  }

  function taskItems() {
    return state.runningOnly ? fixtures.tasks.filter((task) => task.status === "running") : fixtures.tasks;
  }

  function renderTasks() {
    const tasks = taskItems();
    if (!tasks.some((task) => task.id === state.selectedTask)) state.selectedTask = tasks[0]?.id;
    qs("#task-list").innerHTML = tasks.map((task) => `
      <button class="task-row-large ${task.id === state.selectedTask ? "is-selected" : ""}" type="button" data-task-id="${escapeHtml(task.id)}">
        <span class="task-status status-${escapeHtml(task.status)}" aria-hidden="true"></span>
        <span><strong>${escapeHtml(task.title)}</strong><small>${escapeHtml(task.project)} · ${escapeHtml(task.time)}</small></span>
        <b>${escapeHtml(task.statusLabel)}</b>
      </button>
    `).join("") || '<p class="empty-state">没有符合条件的任务。</p>';
    renderTaskDetail();
  }

  function renderTaskDetail() {
    const task = fixtures.tasks.find((item) => item.id === state.selectedTask);
    qs("#task-detail").innerHTML = task ? `
      <p class="eyebrow">SELECTED TASK</p>
      <span class="task-detail-status status-text-${escapeHtml(task.status)}">${escapeHtml(task.statusLabel)}</span>
      <h3>${escapeHtml(task.title)}</h3>
      <p>${escapeHtml(task.summary)}</p>
      <dl>
        <div><dt>MCP 指令</dt><dd>${escapeHtml(task.source)}</dd></div>
        <div><dt>项目</dt><dd>${escapeHtml(task.project)}</dd></div>
        <div><dt>Codex 对话</dt><dd>${escapeHtml(task.thread)}</dd></div>
        <div><dt>任务编号</dt><dd>${escapeHtml(task.id)}</dd></div>
        <div><dt>时间</dt><dd>${escapeHtml(task.time)}</dd></div>
      </dl>
      <div class="conversation-preview">
        <p class="eyebrow">SYNCED CONVERSATION</p>
        ${task.messages.map((message) => `<div class="message message-${escapeHtml(message.role)}"><span>${message.role === "user" ? "灵矽" : "Codex"}</span><p>${escapeHtml(message.text)}</p></div>`).join("")}
      </div>
      <div class="local-note">此页面仅显示本地 mock 数据</div>
    ` : '<p class="empty-state">选择一个任务查看详情。</p>';
  }

  function setConnection(connected) {
    state.connected = connected;
    qs("#connection-pill").dataset.state = connected ? "online" : "offline";
    qs("#connection-label").textContent = connected ? "MCP 已连接" : "MCP 未连接";
    qs("#hero-lingxi-status").textContent = connected ? "已连接" : "等待连接";
    qs("#hero-lingxi-status").classList.toggle("status-good", connected);
    qs("#connection-preview").dataset.state = connected ? "online" : "offline";
    qs("#preview-title").textContent = connected ? "LinxBox 已连接" : "等待连接";
    qs("#preview-copy").textContent = connected ? "MCP WebSocket 已模拟在线，LinxBox 可以双向转发指令、状态和对话。" : "输入 MCP WebSocket 信息，然后测试 LinxBox 是否可以建立双向连接。";
    qs("#preview-network").textContent = connected ? "模拟在线" : "未发起";
    qs("#connect-button").textContent = connected ? "重新测试" : "测试并连接";
    qs("#disconnect-button").disabled = !connected;
    qs("#route-flow").classList.toggle("is-connected", connected);
  }

  qsa(".nav-item").forEach((button) => button.addEventListener("click", () => switchView(button.dataset.view)));
  qsa("[data-open-view]").forEach((button) => button.addEventListener("click", () => switchView(button.dataset.openView)));

  qs("#connection-form").addEventListener("submit", (event) => {
    event.preventDefault();
    const url = qs("#gateway-url").value.trim();
    const token = qs("#access-token").value.trim();
    const computer = qs("#computer-name").value.trim();
    if (!url || !token || !computer) {
      showToast("请填写服务地址、访问令牌和电脑名称");
      return;
    }
    qs("#connect-button").textContent = "连接中…";
    qs("#connect-button").disabled = true;
    qs("#preview-network").textContent = "模拟检测中";
    setTimeout(() => {
      qs("#connect-button").disabled = false;
      qs("#preview-computer").textContent = computer;
      setConnection(true);
      showToast("MCP WebSocket 模拟连接成功；没有发起真实网络请求");
    }, 700);
  });

  qs("#disconnect-button").addEventListener("click", () => {
    setConnection(false);
    showToast("模拟连接已断开");
  });

  qs("#toggle-token").addEventListener("click", (event) => {
    const input = qs("#access-token");
    const show = input.type === "password";
    input.type = show ? "text" : "password";
    event.currentTarget.textContent = show ? "隐藏" : "显示";
  });

  qs("#computer-name").addEventListener("input", (event) => {
    qs("#preview-computer").textContent = event.currentTarget.value || "未命名 Mac";
  });

  qs("#add-project").addEventListener("click", () => {
    if (state.projects.some((project) => project.id === "PRJ_NEW")) {
      showToast("模拟项目已经添加");
      return;
    }
    state.projects.push({ id: "PRJ_NEW", name: "新的本地项目", path: "/Users/demo/Projects/new-project", enabled: true, updated: "刚刚" });
    renderProjects();
    showToast("已添加模拟项目；未访问文件系统");
  });

  qs("#project-grid").addEventListener("click", (event) => {
    const button = event.target.closest("[data-toggle-project]");
    if (!button) return;
    const project = state.projects.find((item) => item.id === button.dataset.toggleProject);
    project.enabled = !project.enabled;
    project.updated = "刚刚";
    renderProjects();
    showToast(`${project.name}已${project.enabled ? "启用" : "停用"}`);
  });

  qs("#filter-tasks").addEventListener("click", (event) => {
    state.runningOnly = !state.runningOnly;
    event.currentTarget.textContent = state.runningOnly ? "查看全部" : "仅看进行中";
    renderTasks();
  });

  qs("#task-list").addEventListener("click", (event) => {
    const row = event.target.closest("[data-task-id]");
    if (!row) return;
    state.selectedTask = row.dataset.taskId;
    renderTasks();
  });

  renderActivities();
  renderProjects();
  renderTasks();
  setConnection(false);
})();
