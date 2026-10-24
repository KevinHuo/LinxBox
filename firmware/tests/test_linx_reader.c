#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "linx_content.h"
#include "linx_reader.h"

typedef struct {
    size_t headings;
    size_t body;
    size_t lists;
} measurements_t;

static int32_t measure(const char *text, size_t bytes, linx_reader_kind_t kind, void *context)
{
    measurements_t *calls = context;
    if (calls) {
        if (kind == LINX_READER_HEADING) calls->headings++;
        else if (kind == LINX_READER_LIST) calls->lists++;
        else calls->body++;
    }
    int32_t width = 0;
    for (size_t i = 0U; i < bytes; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0xc0U) != 0x80U) width += c < 0x80U ? 8 : 16;
    }
    /* Distinct metrics make measuring headings with the body font observable. */
    return width + (kind == LINX_READER_HEADING && bytes ? 1 : 0);
}

static void assert_not_closing(const char *text, size_t start)
{
    static const char *const closing[] = {
        "。", "，", "：", "；", "？", "！", "、", "）", "】", "”", ",", ".", ")", "]", ":", ";", "?", "!"
    };
    while (text[start] == ' ' || text[start] == '\t') start++;
    for (size_t i = 0U; i < sizeof(closing) / sizeof(closing[0]); i++) {
        assert(strncmp(text + start, closing[i], strlen(closing[i])) != 0);
    }
}

static linx_reader_layout_t audit(const char *source, uint16_t width, uint16_t height)
{
    linx_reader_layout_t layout;
    measurements_t calls = {0};
    assert(linx_reader_paginate(&layout, source, width, height, measure, &calls));
    assert(layout.complete && layout.count >= 1U && layout.count <= LINX_PAGE_LIMIT);
    size_t length = strlen(source);
    assert(layout.offsets[0] == 0U && layout.offsets[layout.count] == length);
    unsigned char rendered[LINX_TEXT_LIMIT] = {0};
    char recovered[LINX_TEXT_LIMIT + 1U];
    size_t used = 0U;
    for (size_t p = 0U; p < layout.count; p++) {
        linx_reader_page_t page;
        assert(linx_reader_get_page(&layout, p, source, measure, &calls, &page));
        assert(page.source_start == layout.offsets[p]);
        assert(page.source_end == layout.offsets[p + 1U]);
        assert(page.source_start <= page.source_end && page.source_end <= length);
        assert(page.count <= LINX_READER_ROWS);
        if (length) assert(page.source_start < page.source_end);
        memcpy(recovered + used, source + page.source_start, page.source_end - page.source_start);
        used += page.source_end - page.source_start;
        for (size_t r = 0U; r < page.count; r++) {
            const linx_reader_row_t *row = &page.rows[r];
            assert(row->length > 0U && row->start >= page.source_start);
            assert(row->start + row->length <= page.source_end);
            assert(((unsigned char)source[row->start] & 0xc0U) != 0x80U);
            assert(((unsigned char)source[row->start + row->length] & 0xc0U) != 0x80U);
            assert(row->y + LINX_READER_LINE_HEIGHT <= height);
            assert(row->x + measure(source + row->start, row->length, row->kind, NULL) <= width);
            assert(memchr(source + row->start, '\n', row->length) == NULL);
            assert_not_closing(source, row->start);
            if (r == 0U) assert(row->y == 0U);
            else assert(row->y >= page.rows[r - 1U].y + LINX_READER_LINE_HEIGHT);
            if (row->kind == LINX_READER_LIST) {
                size_t marker = row->start;
                while (marker > 0U && source[marker - 1U] != '\n') marker--;
                while (source[marker] == ' ' || source[marker] == '\t') marker++;
                size_t indent = LINX_READER_LIST_INDENT;
                if (source[marker] >= '0' && source[marker] <= '9') {
                    size_t end = marker;
                    while (source[end] >= '0' && source[end] <= '9') end++;
                    end++; /* Period or right parenthesis. */
                    size_t measured = (size_t)measure(source + marker, end - marker, LINX_READER_BODY, NULL) + 2U;
                    if (measured > indent) indent = measured;
                }
                assert(row->x == indent);
                assert(row->first_list_line == (row->marker_length > 0U));
            } else {
                assert(row->x == 0U && !row->first_list_line && row->marker_length == 0U);
            }
            for (size_t i = row->start; i < row->start + row->length; i++) {
                assert(!rendered[i]);
                rendered[i] = 1U;
            }
            for (size_t i = row->marker_start; i < row->marker_start + row->marker_length; i++) {
                assert(i >= page.source_start && i < page.source_end && !rendered[i]);
                rendered[i] = 1U;
            }
            /* A section title never ends a page without following content. */
            if (r + 1U == page.count) assert(row->kind != LINX_READER_HEADING);
        }
    }
    recovered[used] = '\0';
    assert(used == length && strcmp(recovered, source) == 0);
    /* Every non-whitespace byte is drawn exactly once or an explicit marker. */
    for (size_t i = 0U; i < length; i++) {
        assert(rendered[i] || source[i] == '\n' || source[i] == '\r' || source[i] == ' ' || source[i] == '\t');
    }
    return layout;
}

static void actual_templates(void)
{
    for (unsigned id = 0U; id < LINX_CONTENT_COUNT; id++) {
        const linx_content_t *content = linx_content_get((linx_content_id_t)id);
        audit(content->body, 208U, 200U);
        audit(content->summary, 208U, 200U);
        audit(content->question, 208U, 200U);
    }
}

static void login_list_regression(void)
{
    const char *source = linx_content_get(LINX_CONTENT_LOGIN_FOLLOWUP)->body;
    linx_reader_layout_t layout = audit(source, 208U, 200U);
    size_t markers = 0U;
    size_t continuations = 0U;
    size_t headings = 0U;
    for (size_t p = 0U; p < layout.count; p++) {
        linx_reader_page_t page;
        assert(linx_reader_get_page(&layout, p, source, measure, NULL, &page));
        for (size_t r = 0U; r < page.count; r++) {
            const linx_reader_row_t *row = &page.rows[r];
            assert(!(row->length == 1U && source[row->start] == '-'));
            if (row->kind == LINX_READER_HEADING) headings++;
            if (row->kind != LINX_READER_LIST) continue;
            if (row->first_list_line) {
                assert(source[row->start] != '-' && source[row->start] != ' ');
                assert(row->marker_length == 1U && source[row->marker_start] == '-');
                markers++;
            } else continuations++;
        }
    }
    assert(markers == 2U && continuations >= 2U && headings == 2U);
}

static void paragraph_spacing_and_heading_pairing(void)
{
    const char *source = "首段。\n\n整体风格\n\n正文内容。\n第二段内容。";
    linx_reader_layout_t layout = audit(source, 208U, 200U);
    linx_reader_page_t page;
    assert(layout.count == 1U);
    assert(linx_reader_get_page(&layout, 0U, source, measure, NULL, &page));
    assert(page.count == 4U);
    assert(page.rows[0].y == 0U && page.rows[1].y == 30U);
    assert(page.rows[2].y == 60U && page.rows[3].y == 82U);
    assert(page.rows[1].kind == LINX_READER_HEADING);
    assert(page.rows[2].kind == LINX_READER_BODY);
    /* 66px fits three rows, but the last title must move with its next row. */
    source = "第一句。\n第二句。\n本轮修改\n- 调整提示文案。";
    layout = audit(source, 208U, 66U);
    assert(layout.count == 2U);
    assert(linx_reader_get_page(&layout, 1U, source, measure, NULL, &page));
    assert(page.rows[0].kind == LINX_READER_HEADING && page.rows[0].y == 0U);
    assert(page.rows[1].kind == LINX_READER_LIST);
    source = "前段。\n大标题\n小标题\n正文。";
    layout = audit(source, 208U, 66U);
    assert(layout.count == 2U);
    assert(linx_reader_get_page(&layout, 1U, source, measure, NULL, &page));
    assert(page.count == 3U && page.rows[0].kind == LINX_READER_HEADING &&
           page.rows[1].kind == LINX_READER_HEADING && page.rows[2].kind == LINX_READER_BODY);
    assert(!linx_reader_paginate(&layout, source, 208U, 44U, measure, NULL));
    /* The heading's own metric causes a 13-character title to wrap at 208px. */
    source = "一二三四五六七八九十一二三\n正文。";
    layout = audit(source, 208U, 66U);
    assert(linx_reader_get_page(&layout, 0U, source, measure, NULL, &page));
    assert(page.count == 3U && page.rows[0].kind == LINX_READER_HEADING &&
           page.rows[1].kind == LINX_READER_HEADING);
    /* Leading/trailing blank source never consumes a displayed row or margin. */
    source = "\n\n正文。\n\n\n下一段。\n\n";
    layout = audit(source, 208U, 44U);
    assert(layout.count == 2U);
    assert(linx_reader_get_page(&layout, 1U, source, measure, NULL, &page));
    assert(page.count == 1U && page.rows[0].y == 0U);
}

static void mixed_and_numbered_lists(void)
{
    const char *source = "内容说明\n\n- ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 连续英文正确换行。\n"
                         "  * Codex 与 Claude Code 保持原任务上下文。\n"
                         "12) First item has two spaces  and\ttabs.\n"
                         "3. 中文和 English 混合，标点不能出现在错误的位置。\n\n"
                         "收尾内容。";
    linx_reader_layout_t layout = audit(source, 208U, 200U);
    size_t firsts = 0U;
    for (size_t p = 0U; p < layout.count; p++) {
        linx_reader_page_t page;
        assert(linx_reader_get_page(&layout, p, source, measure, NULL, &page));
        for (size_t r = 0U; r < page.count; r++) if (page.rows[r].first_list_line) firsts++;
    }
    assert(firsts == 4U);
    audit("这是第一段，内容很多（括号里的补充说明）后面继续，最后结束。", 96U, 44U);
}

static void numbered_marker_width_regression(void)
{
    const char *source = "本轮修改\n12) ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 中文内容继续。\n"
                         "1234. Keep the complete original numbered marker visible.\n- 项目符号仍使用原来的缩进。";
    linx_reader_layout_t layout = audit(source, 208U, 66U);
    size_t seen12 = 0U;
    size_t seen1234 = 0U;
    size_t bullet = 0U;
    for (size_t p = 0U; p < layout.count; p++) {
        linx_reader_page_t page;
        assert(linx_reader_get_page(&layout, p, source, measure, NULL, &page));
        for (size_t r = 0U; r < page.count; r++) {
            const linx_reader_row_t *row = &page.rows[r];
            if (row->kind != LINX_READER_LIST) continue;
            if (row->x == 26U) seen12++;
            else if (row->x == 42U) seen1234++;
            else { assert(row->x == 12U); bullet++; }
            if (row->first_list_line && source[row->marker_start] != '-') {
                assert(measure(source + row->marker_start, row->marker_length, LINX_READER_BODY, NULL) + 2 <= row->x);
            }
        }
    }
    assert(seen12 >= 2U && seen1234 >= 2U && bullet >= 1U);
    /* Insufficient remaining body width must fail, including heading lookahead. */
    assert(!linx_reader_paginate(&layout, "12) 字", 41U, 66U, measure, NULL));
    assert(!linx_reader_paginate(&layout, "本轮修改\n12) 字", 41U, 66U, measure, NULL));
    audit("12) 字", 42U, 66U);
}

static void bounds_and_failures(void)
{
    linx_reader_layout_t layout;
    linx_reader_page_t page;
    assert(!linx_reader_paginate(NULL, "text", 208U, 200U, measure, NULL));
    assert(!linx_reader_paginate(&layout, NULL, 208U, 200U, measure, NULL));
    assert(!linx_reader_paginate(&layout, "text", 208U, 200U, NULL, NULL));
    assert(!linx_reader_paginate(&layout, "text", 0U, 200U, measure, NULL));
    assert(!linx_reader_paginate(&layout, "text", 208U, 21U, measure, NULL));
    assert(!linx_reader_paginate(&layout, "字", 8U, 200U, measure, NULL));
    assert(!linx_reader_paginate(&layout, "- text", 12U, 200U, measure, NULL));
    char oversized[LINX_TEXT_LIMIT + 2U];
    memset(oversized, 'a', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    assert(!linx_reader_paginate(&layout, oversized, 208U, 200U, measure, NULL));
    char many[140];
    for (size_t i = 0U; i < 69U; i++) { many[i * 2U] = 'a'; many[i * 2U + 1U] = '.'; }
    many[138] = '\0';
    assert(!linx_reader_paginate(&layout, many, 16U, 22U, measure, NULL));
    assert(!layout.complete && layout.count == LINX_PAGE_LIMIT);
    layout = audit("", 208U, 200U);
    assert(linx_reader_get_page(&layout, 0U, "", measure, NULL, &page));
    assert(page.count == 0U && page.source_start == 0U && page.source_end == 0U);
    assert(!linx_reader_get_page(&layout, 1U, "", measure, NULL, &page));
    assert(!linx_reader_get_page(&layout, 0U, "changed.", measure, NULL, &page));
    assert(!linx_reader_get_page(NULL, 0U, "", measure, NULL, &page));
    assert(!linx_reader_get_page(&layout, 0U, "", measure, NULL, NULL));
    layout.count = LINX_PAGE_LIMIT + 1U;
    assert(!linx_reader_get_page(&layout, 0U, "", measure, NULL, &page));
    char exact[LINX_TEXT_LIMIT + 1U];
    memset(exact, 'a', LINX_TEXT_LIMIT);
    exact[LINX_TEXT_LIMIT] = '\0';
    audit(exact, 208U, 200U);
    /* Even a very tall viewport never exceeds the fixed row storage. */
    audit("一。\n二。\n三。\n四。\n五。\n六。\n七。\n八。\n九。\n十。\n一。\n二。\n三。", 208U, 1000U);
}

static void plain_markers_and_heading_boundaries(void)
{
    const char *source = "-\n*\n1.\n12)\n123\n123x\n-abc\n*abc\n- \t\n正文。";
    linx_reader_layout_t layout = audit(source, 208U, 1000U);
    linx_reader_page_t page;
    assert(linx_reader_get_page(&layout, 0U, source, measure, NULL, &page));
    assert(page.count == 10U);
    for (size_t r = 0U; r < page.count; r++) {
        assert(page.rows[r].kind == LINX_READER_BODY);
        assert(page.rows[r].marker_length == 0U && page.rows[r].x == 0U);
    }
    source = " \t标题 \t\r\n\r\t \n正文。";
    layout = audit(source, 208U, 200U);
    assert(linx_reader_get_page(&layout, 0U, source, measure, NULL, &page));
    assert(page.count == 2U && page.rows[0].kind == LINX_READER_HEADING);
    assert(page.rows[1].kind == LINX_READER_BODY && page.rows[1].y == 30U);
    source = "正文。\n看似标题\n \t\r\n";
    layout = audit(source, 208U, 200U);
    assert(linx_reader_get_page(&layout, 0U, source, measure, NULL, &page));
    assert(page.count == 2U && page.rows[1].kind == LINX_READER_BODY);
    audit("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\n正文。", 208U, 200U);
    audit("数字1\n正文。\n1正文\n尾句。", 208U, 200U);
    /* 单数字有序标记同样保留原始标记及悬挂缩进。 */
    audit("1. a", 208U, 200U);
}

static int32_t invalid_marker_measure(const char *text, size_t bytes,
                                       linx_reader_kind_t kind, void *context)
{
    if (kind == LINX_READER_BODY && bytes == 3U && memcmp(text, "12)", 3U) == 0) return -1;
    return measure(text, bytes, kind, context);
}

static int32_t larger_measure(const char *text, size_t bytes,
                               linx_reader_kind_t kind, void *context)
{
    return measure(text, bytes, kind, context) * 2;
}

static void rejected_layouts_clear_output(void)
{
    linx_reader_layout_t layout;
    assert(!linx_reader_paginate(&layout, "12) 字", 208U, 200U, invalid_marker_measure, NULL));
    assert(!layout.complete);
    assert(!linx_reader_paginate(&layout, "标题\n12) 字", 208U, 200U, invalid_marker_measure, NULL));
    assert(!linx_reader_paginate(&layout, "标题\n正文。", 8U, 200U, measure, NULL));
    char headings[256] = "";
    for (size_t i = 0U; i < LINX_READER_ROWS; i++) strcat(headings, "标题\n");
    strcat(headings, "正文。");
    assert(!linx_reader_paginate(&layout, headings, 208U, 1000U, measure, NULL));
    assert(!layout.complete);

    linx_reader_page_t page;
    const linx_reader_page_t empty = {0};
    memset(&page, 0xa5, sizeof(page));
    assert(!linx_reader_get_page(&layout, 0U, headings, measure, NULL, &page));
    assert(memcmp(&page, &empty, sizeof(page)) == 0);
    layout = audit("abcdef", 16U, 22U);
    assert(layout.count == 3U);
    memset(&page, 0xa5, sizeof(page));
    assert(!linx_reader_get_page(&layout, 0U, "abcdef", larger_measure, NULL, &page));
    assert(memcmp(&page, &empty, sizeof(page)) == 0);
    assert(!linx_reader_get_page(&layout, 0U, NULL, measure, NULL, &page));
    assert(!linx_reader_get_page(&layout, 0U, "abcdef", NULL, NULL, &page));
    assert(memcmp(&page, &empty, sizeof(page)) == 0);
    layout.count = 0U;
    assert(!linx_reader_get_page(&layout, 0U, "abcdef", measure, NULL, &page));
    assert(memcmp(&page, &empty, sizeof(page)) == 0);
}

int main(void)
{
    actual_templates();
    login_list_regression();
    paragraph_spacing_and_heading_pairing();
    mixed_and_numbered_lists();
    numbered_marker_width_regression();
    bounds_and_failures();
    plain_markers_and_heading_boundaries();
    rejected_layouts_clear_output();
    puts("LinX structured reader: PASS (templates, byte spans, headings, hanging lists, pixel bounds)");
    return 0;
}
