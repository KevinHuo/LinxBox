/** UTF-8 byte based content chunks required by the Gateway contract. */
export function readUtf8Chunk(text: string, offset: number, maxBytes = 1024): { text: string; nextOffset: number | null } {
  const bytes = Buffer.from(text, 'utf8');
  if (offset < 0 || offset > bytes.length || !Number.isInteger(offset)) throw new Error('正文 offset 不合法');
  if (offset > 0 && ((bytes[offset] ?? 0) & 0xc0) === 0x80) throw new Error('正文 offset 不在字符边界');
  const end = Math.min(bytes.length, offset + maxBytes);
  let safeEnd = end;
  while (safeEnd > offset && (((bytes[safeEnd] ?? 0) & 0xc0) === 0x80)) safeEnd -= 1;
  const chunk = bytes.subarray(offset, safeEnd).toString('utf8');
  return { text: chunk, nextOffset: safeEnd < bytes.length ? safeEnd : null };
}
