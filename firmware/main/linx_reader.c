#include "linx_reader.h"

#include <string.h>

typedef struct {
    linx_reader_measure_fn measure;
    void *context;
    linx_reader_kind_t kind;
} measure_context_t;

typedef struct {
    size_t content;
    size_t marker;
    size_t marker_length;
} list_prefix_t;

static int32_t row_width(const char *text, size_t bytes, void *context)
{
    const measure_context_t *font = context;
    return font->measure(text, bytes, font->kind, font->context);
}

static bool horizontal_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static size_t line_end(const char *text, size_t start, size_t length)
{
    const char *end = memchr(text + start, '\n', length - start);
    return end ? (size_t)(end - text) : length;
}

static bool blank_line(const char *text, size_t start, size_t end)
{
    while (start < end && horizontal_space(text[start])) start++;
    return start == end;
}

static list_prefix_t list_prefix(const char *text, size_t start, size_t end)
{
    size_t marker = start;
    while (marker < end && horizontal_space(text[marker])) marker++;
    size_t at = marker;
    if (at < end && (text[at] == '-' || text[at] == '*')) {
        at++;
    } else {
        while (at < end && text[at] >= '0' && text[at] <= '9') at++;
        if (at == marker || at == end || (text[at] != '.' && text[at] != ')')) {
            return (list_prefix_t){start, 0U, 0U};
        }
        at++;
    }
    size_t marker_end = at;
    if (at == end || !horizontal_space(text[at])) return (list_prefix_t){start, 0U, 0U};
    while (at < end && horizontal_space(text[at])) at++;
    /* Marker-only input stays ordinary text rather than silently disappearing. */
    if (at == end) return (list_prefix_t){start, 0U, 0U};
    return (list_prefix_t){at, marker, marker_end - marker};
}

static bool list_indent(const char *text, list_prefix_t prefix,
                         const measure_context_t *font, size_t *indent)
{
    *indent = prefix.marker_length ? LINX_READER_LIST_INDENT : 0U;
    if (prefix.marker_length && text[prefix.marker] >= '0' && text[prefix.marker] <= '9') {
        int32_t marker_width = font->measure(text + prefix.marker, prefix.marker_length,
                                            LINX_READER_BODY, font->context);
        if (marker_width < 0) return false;
        size_t numbered_indent = (size_t)marker_width + 2U;
        if (numbered_indent > *indent) *indent = numbered_indent;
    }
    return true;
}

static bool section_heading(const char *text, size_t start, size_t end, size_t length)
{
    if (end == length || start == end || end - start > 48U) return false;
    while (start < end && horizontal_space(text[start])) start++;
    while (end > start && horizontal_space(text[end - 1U])) end--;
    if (start == end || text[start] == '-' || text[start] == '*' ||
        (text[start] >= '0' && text[start] <= '9')) return false;
    static const char *const punctuation[] = {
        "。", "，", "：", "；", "？", "！", "、", "…", ".", ",", ":", ";", "?", "!"
    };
    for (size_t i = 0U; i < sizeof(punctuation) / sizeof(punctuation[0]); i++) {
        size_t bytes = strlen(punctuation[i]);
        if (end - start >= bytes && memcmp(text + end - bytes, punctuation[i], bytes) == 0) return false;
    }
    /* A title must introduce actual content, not terminate an empty document. */
    size_t next = line_end(text, start, length);
    while (next < length) {
        next++;
        size_t next_end = line_end(text, next, length);
        if (!blank_line(text, next, next_end)) return true;
        next = next_end;
    }
    return false;
}

static bool heading_reserve(const char *text, size_t start, size_t end, size_t length,
                            uint16_t width, measure_context_t *font,
                            size_t *rows, size_t *height)
{
    *rows = 0U;
    *height = 0U;
    for (;;) {
        size_t at = start;
        while (at < end) {
            linx_text_line_t line;
            if (!linx_wrap_span(text, at, end, width, row_width, font, &line) ||
                row_width(text + at, line.end - at, font) > width) return false;
            (*rows)++;
            *height += LINX_READER_LINE_HEIGHT;
            at = line.next;
        }
        size_t following = end;
        bool gap = false;
        size_t next_end = end;
        while (following < length) {
            following++;
            next_end = line_end(text, following, length);
            if (!blank_line(text, following, next_end)) break;
            gap = true;
            following = next_end;
        }
        if (gap) *height += LINX_READER_PARAGRAPH_GAP;
        if (section_heading(text, following, next_end, length)) {
            /* Consecutive short section titles stay with the first body row. */
            start = following;
            end = next_end;
            continue;
        }
        /* Check the first following row with its actual hanging indentation.
         * Long numbered markers may leave no room for even one body glyph. */
        list_prefix_t prefix = list_prefix(text, following, next_end);
        size_t indent;
        measure_context_t following_font = *font;
        following_font.kind = prefix.marker_length ? LINX_READER_LIST : LINX_READER_BODY;
        linx_text_line_t line;
        if (!list_indent(text, prefix, &following_font, &indent) || width <= indent ||
            !linx_wrap_span(text, prefix.content, next_end, (int32_t)(width - indent),
                            row_width, &following_font, &line) || line.end <= prefix.content ||
            row_width(text + prefix.content, line.end - prefix.content, &following_font) >
                (int32_t)(width - indent)) return false;
        /* At least the next text row accompanies every section heading. */
        (*rows)++;
        *height += LINX_READER_LINE_HEIGHT;
        return true;
    }
}

static bool new_page(linx_reader_layout_t *layout, size_t source, size_t *rows,
                      size_t *y, size_t target, linx_reader_page_t *out)
{
    if (layout->count >= LINX_PAGE_LIMIT) return false;
    if (out && layout->count - 1U == target) out->source_end = source;
    layout->offsets[layout->count++] = source;
    *rows = 0U;
    *y = 0U;
    if (out && layout->count - 1U == target) out->source_start = source;
    return true;
}

static bool build_layout(linx_reader_layout_t *layout, const char *text,
                          uint16_t width, uint16_t height,
                          linx_reader_measure_fn measure, void *context,
                          size_t target, linx_reader_page_t *out)
{
    memset(layout, 0, sizeof(*layout));
    if (out) memset(out, 0, sizeof(*out));
    if (!text || !measure || width == 0U || height < LINX_READER_LINE_HEIGHT) return false;
    size_t length = strlen(text);
    if (length > LINX_TEXT_LIMIT) return false;
    layout->width = width;
    layout->height = height;
    layout->count = 1U;
    size_t position = 0U;
    size_t rows = 0U;
    size_t y = 0U;
    size_t gap = 0U;
    measure_context_t font = {measure, context, LINX_READER_BODY};
    while (position < length) {
        size_t physical_start = position;
        size_t end = line_end(text, position, length);
        if (blank_line(text, position, end)) {
            position = end < length ? end + 1U : end;
            /* Consecutive blank source lines form one compact paragraph gap. */
            gap = LINX_READER_PARAGRAPH_GAP;
            continue;
        }
        list_prefix_t prefix = list_prefix(text, position, end);
        font.kind = prefix.marker_length ? LINX_READER_LIST :
                    section_heading(text, position, end, length) ? LINX_READER_HEADING : LINX_READER_BODY;
        size_t indent;
        if (!list_indent(text, prefix, &font, &indent) || width <= indent) return false;
        if (font.kind == LINX_READER_HEADING) {
            size_t needed_rows;
            size_t needed_height;
            if (!heading_reserve(text, position, end, length, width, &font,
                                 &needed_rows, &needed_height)) return false;
            if (needed_rows > LINX_READER_ROWS || needed_height > height) return false;
            if (rows && (rows + needed_rows > LINX_READER_ROWS ||
                         y + gap + needed_height > height)) {
                if (!new_page(layout, physical_start, &rows, &y, target, out)) return false;
            }
        }
        bool first = true;
        position = prefix.content;
        while (position < end) {
            linx_text_line_t line;
            if (!linx_wrap_span(text, position, end, (int32_t)(width - indent),
                                row_width, &font, &line) || line.end <= position ||
                line.next <= position || line.next > end ||
                row_width(text + position, line.end - position, &font) > (int32_t)(width - indent)) return false;
            size_t row_y = y + (first && rows ? gap : 0U);
            if (rows == LINX_READER_ROWS || row_y + LINX_READER_LINE_HEIGHT > height) {
                size_t boundary = first ? physical_start : position;
                if (!new_page(layout, boundary, &rows, &y, target, out)) return false;
                row_y = 0U;
            }
            if (out && layout->count - 1U == target) {
                out->rows[out->count++] = (linx_reader_row_t){
                    .start = position,
                    .length = line.end - position,
                    .marker_start = first ? prefix.marker : 0U,
                    .marker_length = first ? prefix.marker_length : 0U,
                    .x = (uint16_t)indent,
                    .y = (uint16_t)row_y,
                    .kind = font.kind,
                    .first_list_line = first && font.kind == LINX_READER_LIST
                };
            }
            rows++;
            y = row_y + LINX_READER_LINE_HEIGHT;
            position = line.next;
            first = false;
        }
        position = end < length ? end + 1U : end;
        gap = 0U;
    }
    layout->offsets[layout->count] = length;
    layout->complete = true;
    if (out && layout->count - 1U == target) out->source_end = length;
    return true;
}

bool linx_reader_paginate(linx_reader_layout_t *layout, const char *text,
                          uint16_t width, uint16_t height,
                          linx_reader_measure_fn measure, void *context)
{
    if (!layout) return false;
    return build_layout(layout, text, width, height, measure, context, 0U, NULL);
}

bool linx_reader_get_page(const linx_reader_layout_t *layout, size_t page,
                          const char *text, linx_reader_measure_fn measure,
                          void *context, linx_reader_page_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!layout || !layout->complete || layout->count == 0U ||
        layout->count > LINX_PAGE_LIMIT || page >= layout->count) return false;
    linx_reader_layout_t checked;
    if (!build_layout(&checked, text, layout->width, layout->height, measure, context,
                      page, out) || checked.count != layout->count ||
        memcmp(checked.offsets, layout->offsets,
               (layout->count + 1U) * sizeof(layout->offsets[0])) != 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}
