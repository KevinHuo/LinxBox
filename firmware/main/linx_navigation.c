#include "linx_navigation.h"

#include <string.h>

static size_t utf8_bytes(unsigned char first)
{
    if (first < 0x80U) return 1U;
    if ((first & 0xe0U) == 0xc0U) return 2U;
    if ((first & 0xf0U) == 0xe0U) return 3U;
    if ((first & 0xf8U) == 0xf0U) return 4U;
    return 1U;
}

static uint32_t codepoint_at(const char *text, size_t position, size_t limit)
{
    if (position >= limit) return 0U;
    unsigned char first = (unsigned char)text[position];
    size_t bytes = utf8_bytes(first);
    if (bytes == 1U || position + bytes > limit) return first;
    uint32_t codepoint = first & (bytes == 2U ? 0x1fU : bytes == 3U ? 0x0fU : 0x07U);
    for (size_t i = 1U; i < bytes; i++) {
        codepoint = (codepoint << 6U) | ((unsigned char)text[position + i] & 0x3fU);
    }
    return codepoint;
}

static size_t previous_character(const char *text, size_t start, size_t position)
{
    if (position <= start) return start;
    position--;
    while (position > start && ((unsigned char)text[position] & 0xc0U) == 0x80U) position--;
    return position;
}

static bool prohibited_line_start(uint32_t codepoint)
{
    switch (codepoint) {
        case ',': case '.': case '!': case '?': case ':': case ';':
        case ')': case ']': case '}':
        case 0x3001U: case 0x3002U: /* Ideographic comma and full stop. */
        case 0xff0cU: case 0xff0eU: case 0xff01U: case 0xff1fU:
        case 0xff1aU: case 0xff1bU: case 0x2026U:
        case 0x2019U: case 0x201dU: /* Closing quotation marks. */
        case 0x3009U: case 0x300bU: case 0x300dU: case 0x300fU:
        case 0x3011U: case 0x3015U: case 0x3017U: case 0x3019U: case 0x301bU:
        case 0xff09U: case 0xff3dU: case 0xff5dU:
            return true;
        default: return false;
    }
}

static bool prohibited_line_end(uint32_t codepoint)
{
    switch (codepoint) {
        case '(': case '[': case '{':
        case 0x2018U: case 0x201cU: /* Opening quotation marks. */
        case 0x3008U: case 0x300aU: case 0x300cU: case 0x300eU:
        case 0x3010U: case 0x3014U: case 0x3016U: case 0x3018U: case 0x301aU:
        case 0xff08U: case 0xff3bU: case 0xff5bU:
            return true;
        default: return false;
    }
}

static size_t legal_wrap(const char *text, size_t start, size_t candidate, size_t limit,
                          size_t minimum)
{
    size_t original = candidate;
    while (candidate > minimum) {
        size_t after = candidate;
        while (after < limit && (text[after] == ' ' || text[after] == '\t')) after++;
        size_t before = previous_character(text, start, candidate);
        while (before > start && (text[before] == ' ' || text[before] == '\t')) {
            before = previous_character(text, start, before);
        }
        if (!prohibited_line_start(codepoint_at(text, after, limit)) &&
            !prohibited_line_end(codepoint_at(text, before, limit))) return candidate;
        /* Move the previous character too: never squeeze an extra glyph beyond
         * the measured line width, and never discard a punctuation byte. */
        candidate = before;
    }
    /* A one-character-wide line cannot fit a character plus closing mark.
     * Keep progress for such unsupported geometry; the 208px UI has room. */
    return original;
}

static bool cjk_character(uint32_t codepoint)
{
    return (codepoint >= 0x2e80U && codepoint <= 0x9fffU) ||
           (codepoint >= 0xac00U && codepoint <= 0xd7afU) ||
           (codepoint >= 0xf900U && codepoint <= 0xfaffU) ||
           (codepoint >= 0xff00U && codepoint <= 0xffefU) ||
           (codepoint >= 0x20000U && codepoint <= 0x323afU);
}

static size_t list_content_start(const char *text, size_t start, size_t limit)
{
    if (start > 0U && text[start - 1U] != '\n') return start;
    size_t position = start;
    while (position < limit && (text[position] == ' ' || text[position] == '\t')) position++;
    if (position == limit) return start;
    if (text[position] == '-' || text[position] == '*') position++;
    else {
        size_t first = position;
        while (position < limit && text[position] >= '0' && text[position] <= '9') position++;
        if (position == first || position == limit ||
            (text[position] != '.' && text[position] != ')')) return start;
        position++;
    }
    if (position == limit || (text[position] != ' ' && text[position] != '\t')) return start;
    while (position < limit && (text[position] == ' ' || text[position] == '\t')) position++;
    return position;
}

static linx_text_line_t next_line(const char *text, size_t start, size_t limit,
                              int32_t width, linx_text_width_fn measure, void *context)
{
    size_t position = start;
    size_t word_break = start;
    size_t prefix_end = list_content_start(text, start, limit);
    while (position < limit) {
        if (text[position] == '\n') return (linx_text_line_t){position, position + 1U};
        size_t bytes = utf8_bytes((unsigned char)text[position]);
        if (position + bytes > limit) bytes = 1U;
        int32_t used = measure(text + start, position + bytes - start, context);
        if (used > width && position > start) {
            /* A list marker is attached to its first content character, even
             * when a long English first word needs a hard wrap. */
            size_t end = word_break > prefix_end ? word_break : position;
            end = legal_wrap(text, start, end, limit, prefix_end);
            return (linx_text_line_t){end, end};
        }
        uint32_t current = codepoint_at(text, position, limit);
        position += bytes;
        /* CJK can break between characters and at Latin/CJK boundaries.
         * Retain spaces as word boundaries inside ordinary English text. */
        if (current == ' ' || current == '\t' || cjk_character(current) ||
            cjk_character(codepoint_at(text, position, limit))) {
            word_break = position;
        }
    }
    return (linx_text_line_t){limit, limit};
}

bool linx_wrap_span(const char *text, size_t start, size_t limit, int32_t width,
                    linx_text_width_fn measure, void *context, linx_text_line_t *line)
{
    if (!line) return false;
    *line = (linx_text_line_t){0U, 0U};
    if (!text || !measure || width <= 0 || start >= limit) return false;
    size_t length = strlen(text);
    if (limit > length || ((unsigned char)text[start] & 0xc0U) == 0x80U ||
        (limit < length && ((unsigned char)text[limit] & 0xc0U) == 0x80U)) return false;
    linx_text_line_t result = next_line(text, start, limit, width, measure, context);
    if (result.next <= start || result.next > limit) return false;
    *line = result;
    return true;
}

static bool short_heading(const char *text, size_t start, size_t length)
{
    if (start > 0U && text[start - 1U] != '\n') return false;
    const char *end = memchr(text + start, '\n', length - start);
    if (!end || end == text + start || (size_t)(end - text - start) > 48U) return false;
    if (end[1] != '\n') return false;
    /* Fixed-content section titles have no terminal punctuation or list marker. */
    if (text[start] == '-' || text[start] == '*' || (text[start] >= '0' && text[start] <= '9')) return false;
    size_t bytes = (size_t)(end - text - start);
    const char *last = end - 1;
    while (last > text + start && ((unsigned char)*last & 0xc0U) == 0x80U) last--;
    const char *punctuation[] = {"。", "，", "：", "；", "？", "！", ".", ":", "?", "!"};
    for (size_t i = 0U; i < sizeof(punctuation) / sizeof(punctuation[0]); i++) {
        size_t mark = strlen(punctuation[i]);
        if (bytes >= mark && (size_t)(end - last) == mark && memcmp(last, punctuation[i], mark) == 0) return false;
    }
    return true;
}

bool linx_paginate(linx_pagination_t *pages, const char *text, int32_t width,
                   size_t lines, linx_text_width_fn measure, void *context)
{
    if (!pages) return false;
    memset(pages, 0, sizeof(*pages));
    if (!text || !measure || width <= 0 || lines == 0U) return false;
    size_t length = strlen(text);
    if (length > LINX_TEXT_LIMIT) return false;
    pages->count = 1U;
    size_t position = 0U;
    size_t line = 0U;
    while (position < length) {
        if (lines > 1U && line == lines - 1U && short_heading(text, position, length)) {
            if (pages->count == LINX_PAGE_LIMIT) {
                pages->offsets[pages->count] = position;
                return false;
            }
            pages->offsets[pages->count++] = position;
            line = 0U;
        }
        linx_text_line_t next = next_line(text, position, length, width, measure, context);
        if (next.next <= position) return false;
        position = next.next;
        line++;
        if (line == lines && position < length) {
            if (pages->count == LINX_PAGE_LIMIT) {
                pages->offsets[pages->count] = position;
                return false;
            }
            pages->offsets[pages->count++] = position;
            line = 0U;
        }
    }
    pages->offsets[pages->count] = length;
    pages->complete = true;
    return true;
}

bool linx_page_text(const linx_pagination_t *pages, size_t page, const char *text,
                    int32_t width, linx_text_width_fn measure, void *context,
                    char *out, size_t capacity)
{
    if (!out || capacity == 0U) return false;
    out[0] = '\0';
    if (!pages || !pages->complete || page >= pages->count || !text || !measure) return false;
    size_t position = pages->offsets[page];
    size_t limit = pages->offsets[page + 1U];
    size_t used = 0U;
    while (position < limit) {
        linx_text_line_t line = next_line(text, position, limit, width, measure, context);
        size_t bytes = line.end - position;
        bool another = line.next < limit;
        if (used + bytes + (another ? 1U : 0U) >= capacity) {
            out[0] = '\0';
            return false;
        }
        memcpy(out + used, text + position, bytes);
        used += bytes;
        if (another) out[used++] = '\n';
        position = line.next;
    }
    out[used] = '\0';
    return true;
}

size_t linx_menu_step(size_t selected, int direction, const bool *enabled, size_t count)
{
    if (count == 0U) return 0U;
    if (selected >= count) selected = count - 1U;
    if (direction == 0) return selected;
    size_t next = selected;
    while ((direction > 0 && next + 1U < count) || (direction < 0 && next > 0U)) {
        next = direction > 0 ? next + 1U : next - 1U;
        if (!enabled || enabled[next]) return next;
    }
    return selected;
}
