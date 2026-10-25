import { describe, expect, it } from 'vitest';
import { parseJsonRpcMessage } from './protocol.js';

describe('JSON-RPC 对象入口', () => {
  it.each(['null', '[]', '1', '"text"', 'true', '{invalid'])('拒绝非对象或损坏的帧：%s', (raw) => {
    expect(parseJsonRpcMessage(raw)).toBeNull();
  });
  it('保留合法对象，后续由消息类型校验处理', () => {
    expect(parseJsonRpcMessage('{"jsonrpc":"2.0","id":1,"result":{}}')).toEqual({
      jsonrpc: '2.0',
      id: 1,
      result: {},
    });
  });
});
