export interface ConversationRefreshOptions<T> {
  readonly getSelectedThreadId: () => string | null;
  readonly read: (threadId: string) => Promise<T | undefined>;
  readonly apply: (conversation: T) => void;
  readonly isActive?: () => boolean;
  readonly intervalMs?: number;
}

export interface ConversationRefreshController {
  start(): void;
  stop(): void;
  refreshNow(): Promise<void>;
}

/**
 * Keep the selected Codex conversation synchronized without overlapping reads.
 *
 * A response is applied only if the same thread is still selected when the
 * asynchronous read completes. This prevents a slow response from replacing
 * the conversation after the user has selected a different thread.
 */
export function createConversationRefreshController<T>(
  options: ConversationRefreshOptions<T>,
): ConversationRefreshController {
  const intervalMs = options.intervalMs ?? 1500;
  let timer: ReturnType<typeof setInterval> | undefined;
  let inFlight = false;

  const refreshNow = async (): Promise<void> => {
    if (inFlight || options.isActive?.() === false) {
      return;
    }
    const requestedThreadId = options.getSelectedThreadId();
    if (requestedThreadId === null) {
      return;
    }

    inFlight = true;
    try {
      const conversation = await options.read(requestedThreadId);
      if (conversation !== undefined && options.getSelectedThreadId() === requestedThreadId) {
        options.apply(conversation);
      }
    } catch {
      // Background refresh is best-effort. Explicit user actions still report
      // their own read errors through the normal UI path.
    } finally {
      inFlight = false;
    }
  };

  return {
    start(): void {
      if (timer !== undefined) {
        void refreshNow();
        return;
      }
      void refreshNow();
      timer = setInterval(() => {
        void refreshNow();
      }, intervalMs);
    },
    stop(): void {
      if (timer !== undefined) {
        clearInterval(timer);
        timer = undefined;
      }
    },
    refreshNow,
  };
}
