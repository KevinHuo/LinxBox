import { afterEach, describe, expect, it, vi } from 'vitest';

import { createConversationRefreshController } from './conversation-refresh.js';

afterEach(() => {
  vi.useRealTimers();
});

describe('conversation refresh controller', () => {
  it('refreshes immediately and continues while a conversation is selected', async () => {
    vi.useFakeTimers();
    const applied: string[] = [];
    const read = vi.fn(async (threadId: string) => `${threadId}:${read.mock.calls.length}`);
    const controller = createConversationRefreshController({
      getSelectedThreadId: () => 'th_1',
      read,
      apply: (conversation) => applied.push(conversation),
      intervalMs: 1000,
    });

    controller.start();
    await vi.runOnlyPendingTimersAsync();

    expect(read).toHaveBeenCalledTimes(2);
    expect(applied).toEqual(['th_1:1', 'th_1:2']);
    controller.stop();
  });

  it('ignores a response when the user selected another conversation', async () => {
    let selectedThreadId = 'th_1';
    let resolveRead: ((value: string) => void) | undefined;
    const applied: string[] = [];
    const controller = createConversationRefreshController({
      getSelectedThreadId: () => selectedThreadId,
      read: () =>
        new Promise<string>((resolve) => {
          resolveRead = resolve;
        }),
      apply: (conversation) => applied.push(conversation),
    });

    const refreshing = controller.refreshNow();
    selectedThreadId = 'th_2';
    resolveRead?.('old conversation');
    await refreshing;

    expect(applied).toEqual([]);
  });

  it('does not overlap reads and pauses while inactive', async () => {
    let active = true;
    let resolveRead: (() => void) | undefined;
    const read = vi.fn(
      () =>
        new Promise<string>((resolve) => {
          resolveRead = () => resolve('updated');
        }),
    );
    const controller = createConversationRefreshController({
      getSelectedThreadId: () => 'th_1',
      read,
      apply: () => undefined,
      isActive: () => active,
    });

    const first = controller.refreshNow();
    await controller.refreshNow();
    expect(read).toHaveBeenCalledTimes(1);
    resolveRead?.();
    await first;

    active = false;
    await controller.refreshNow();
    expect(read).toHaveBeenCalledTimes(1);
  });
});
