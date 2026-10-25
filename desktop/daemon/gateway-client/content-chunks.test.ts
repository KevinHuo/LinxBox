import { describe, expect, it } from 'vitest';

import { readUtf8Chunk } from './content-chunks.js';

describe('UTF-8 正文分页', () => {
  it('按字节边界完整重组含中文和 emoji 的长正文', () => {
    const text = 'a'.repeat(1023) + '中🙂文'.repeat(300);
    const chunks: string[] = [];
    let offset = 0;
    for (;;) {
      const chunk = readUtf8Chunk(text, offset);
      expect(Buffer.byteLength(chunk.text)).toBeLessThanOrEqual(1024);
      expect(chunk.text).not.toContain('\uFFFD');
      chunks.push(chunk.text);
      if (chunk.nextOffset === null) break;
      expect(chunk.nextOffset).toBeGreaterThan(offset);
      offset = chunk.nextOffset;
    }
    expect(chunks.join('')).toBe(text);
    expect(chunks[0]).toBe('a'.repeat(1023));
  });

  it.each([-1, 4, 0.5, Number.NaN, Number.POSITIVE_INFINITY])('拒绝非法字节偏移 %s', (offset) => {
    expect(() => readUtf8Chunk('中', offset)).toThrow('正文 offset 不合法');
  });

  it.each([1, 2, 4, 5, 6])('拒绝落在多字节字符内部的偏移 %s', (offset) => {
    expect(() => readUtf8Chunk('中🙂', offset)).toThrow('正文 offset 不在字符边界');
  });

  it('支持空正文、末尾读取和正好等长的分块', () => {
    expect(readUtf8Chunk('', 0)).toEqual({ text: '', nextOffset: null });
    expect(readUtf8Chunk('中🙂', 7)).toEqual({ text: '', nextOffset: null });
    expect(readUtf8Chunk('中🙂', 3, 4)).toEqual({ text: '🙂', nextOffset: null });
    expect(readUtf8Chunk('abcdef', 0, 3)).toEqual({ text: 'abc', nextOffset: 3 });
  });
});
