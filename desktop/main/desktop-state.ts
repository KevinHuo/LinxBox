/**
 * In-memory desktop state backing the IPC handlers.
 *
 * The persistent store (`local-store`) lands in a later slice. Until then this
 * module keeps the facts the renderer needs: the connection configuration,
 * locally authorised projects and the locally known tasks.
 *
 * Nothing here is a simulation of a remote system. There is no Gateway
 * connection and no Codex provider, so those facts are reported as absent
 * rather than faked.
 */
import {
  maskToken,
  type ConnectionView,
  type DiagnosticsView,
  type ProjectView,
  type TaskDetailView,
  type TaskSummaryView,
} from '../shared/ipc.js';

export interface ConnectionRecord {
  readonly configured: boolean;
  readonly connected: boolean;
  readonly gatewayUrl: string | null;
  readonly accessToken: string | null;
  readonly computerName: string | null;
}

export interface ProjectRecord {
  readonly projectId: string;
  readonly displayName: string;
  readonly rootPath: string;
  readonly capabilities: readonly string[];
  readonly grantVersion: number;
  readonly enabled: boolean;
}

export interface TaskRecord {
  readonly taskId: string;
  readonly title: string;
  readonly projectId: string;
  readonly state: string;
  readonly stage: string;
  readonly intent: string;
  readonly summary: string;
  readonly updatedAt: string;
  readonly needsAttention: boolean;
  readonly changes: readonly { readonly path: string; readonly description: string }[];
  readonly verification: readonly {
    readonly command: string;
    readonly outcome: string;
    readonly summary: string;
  }[];
  readonly risks: readonly { readonly code: string; readonly summary: string }[];
  readonly needsUserAction: { readonly summary: string } | null;
  readonly localDiff: string | null;
}

export interface DesktopState {
  connection: ConnectionRecord;
  readonly projects: Map<string, ProjectRecord>;
  readonly tasks: Map<string, TaskRecord>;
  lastError: string | null;
  readonly desktopVersion: string;
}

export interface DesktopStateOptions {
  readonly desktopVersion?: string;
  readonly projects?: readonly ProjectRecord[];
  readonly tasks?: readonly TaskRecord[];
}

/** Create the initial desktop state. */
export function createDesktopState(options: DesktopStateOptions = {}): DesktopState {
  return {
    connection: {
      configured: false,
      connected: false,
      gatewayUrl: null,
      accessToken: null,
      computerName: null,
    },
    projects: new Map((options.projects ?? []).map((project) => [project.projectId, project])),
    tasks: new Map((options.tasks ?? []).map((task) => [task.taskId, task])),
    lastError: null,
    desktopVersion: options.desktopVersion ?? '0.1.0',
  };
}

/** Connection facts for the renderer. The token never leaves this module. */
export function readConnectionView(state: DesktopState): ConnectionView {
  const { connection } = state;
  return {
    configured: connection.configured,
    connected: connection.connected,
    gatewayUrl: connection.gatewayUrl,
    tokenHint: connection.accessToken ? maskToken(connection.accessToken) : null,
    computerName: connection.computerName,
  };
}

/** Store a connection configuration. */
export function saveConnection(
  state: DesktopState,
  input: { gatewayUrl: string; accessToken: string; computerName: string },
): ConnectionView {
  state.connection = {
    configured: true,
    connected: false,
    gatewayUrl: input.gatewayUrl,
    accessToken: input.accessToken,
    computerName: input.computerName,
  };
  return readConnectionView(state);
}

/** Project facts for the renderer. */
export function listProjects(state: DesktopState): ProjectView[] {
  return [...state.projects.values()].map((project) => ({
    projectId: project.projectId,
    displayName: project.displayName,
    rootPath: project.rootPath,
    capabilities: project.capabilities,
    grantVersion: project.grantVersion,
    enabled: project.enabled,
  }));
}

/**
 * Record a project grant.
 *
 * `rootPath` must come from the system directory picker inside the main
 * process. The renderer never supplies it.
 */
export function grantProject(
  state: DesktopState,
  input: { displayName: string; capabilities: readonly string[] },
  rootPath: string,
): ProjectView {
  const existing = [...state.projects.values()].find((project) => project.rootPath === rootPath);
  const projectId = existing?.projectId ?? `prj_local_${state.projects.size + 1}`;
  const grantVersion = (existing?.grantVersion ?? 0) + 1;
  const record: ProjectRecord = {
    projectId,
    displayName: input.displayName,
    rootPath,
    capabilities: input.capabilities,
    grantVersion,
    enabled: true,
  };
  state.projects.set(projectId, record);
  return {
    projectId: record.projectId,
    displayName: record.displayName,
    rootPath: record.rootPath,
    capabilities: record.capabilities,
    grantVersion: record.grantVersion,
    enabled: record.enabled,
  };
}

/** Revoke a grant. Returns `false` when the project is unknown. */
export function revokeProject(state: DesktopState, projectId: string): boolean {
  const existing = state.projects.get(projectId);
  if (!existing) {
    return false;
  }
  state.projects.set(projectId, {
    ...existing,
    grantVersion: existing.grantVersion + 1,
    enabled: false,
  });
  return true;
}

/** Task rows for the renderer, most recently updated first. */
export function listTasks(state: DesktopState, limit?: number): TaskSummaryView[] {
  const rows = [...state.tasks.values()]
    .map((task) => ({
      taskId: task.taskId,
      title: task.title,
      projectId: task.projectId,
      state: task.state,
      stage: task.stage,
      updatedAt: task.updatedAt,
      needsAttention: task.needsAttention,
    }))
    .sort((left, right) => right.updatedAt.localeCompare(left.updatedAt));
  return typeof limit === 'number' ? rows.slice(0, limit) : rows;
}

/** Full local detail for one task. */
export function getTask(state: DesktopState, taskId: string): TaskDetailView | null {
  const task = state.tasks.get(taskId);
  if (!task) {
    return null;
  }
  return {
    taskId: task.taskId,
    title: task.title,
    projectId: task.projectId,
    state: task.state,
    stage: task.stage,
    intent: task.intent,
    summary: task.summary,
    changes: task.changes,
    verification: task.verification,
    risks: task.risks,
    needsUserAction: task.needsUserAction,
    localDiff: task.localDiff,
  };
}

/**
 * Request a task cancellation.
 *
 * A cancel request is not a terminal fact: the task stays in
 * `cancel_requested` until a provider proves `cancelled` or `interrupted`.
 */
export function cancelTask(state: DesktopState, taskId: string): boolean {
  const task = state.tasks.get(taskId);
  if (!task) {
    return false;
  }
  state.tasks.set(taskId, {
    ...task,
    state: 'cancel_requested',
    stage: 'finished',
    updatedAt: new Date().toISOString(),
  });
  return true;
}

/**
 * Find the authorised project that covers a directory.
 *
 * The boundary check is path-segment aware: a root of `/a/proj` does not
 * cover `/a/proj-other`.
 */
export function findAuthorizedProject(
  state: DesktopState,
  directory: string,
): ProjectRecord | null {
  for (const project of state.projects.values()) {
    if (!project.enabled) {
      continue;
    }
    const root = project.rootPath;
    if (directory === root) {
      return project;
    }
    const prefix = root.endsWith('/') ? root : `${root}/`;
    if (directory.startsWith(prefix)) {
      return project;
    }
  }
  return null;
}

/** Local diagnostics for the settings page. */
export function readDiagnostics(state: DesktopState): DiagnosticsView {
  return {
    desktopVersion: state.desktopVersion,
    protocolVersions: ['1.0'],
    databasePath: null,
    connected: state.connection.connected,
    lastError: state.lastError,
  };
}
