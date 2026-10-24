#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "linx_navigation.h"

static int32_t width(const char *text, size_t bytes, void *context)
{
    (void)context;
    int32_t size = 0;
    for (size_t i = 0; i < bytes; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0xc0U) != 0x80U) size += c < 0x80U ? 5 : 10;
    }
    return size;
}

static int32_t display_width(const char *text, size_t bytes, void *context)
{
    /* Match the UI's 16px CJK advance and a conservative 8px Latin advance. */
    return width(text, bytes, context) * 8 / 5;
}

static bool list_prefix_only(const char *text, size_t bytes)
{
    size_t at = 0U;
    while (at < bytes && (text[at] == ' ' || text[at] == '\t')) at++;
    if (at == bytes) return false;
    if (text[at] == '-' || text[at] == '*') at++;
    else {
        size_t first = at;
        while (at < bytes && text[at] >= '0' && text[at] <= '9') at++;
        if (at == first || at == bytes || (text[at] != '.' && text[at] != ')')) return false;
        at++;
    }
    if (at == bytes || (text[at] != ' ' && text[at] != '\t')) return false;
    while (at < bytes && (text[at] == ' ' || text[at] == '\t')) at++;
    return at == bytes;
}

static void display_pagination(const char *source, int32_t line_width, size_t lines,
                               const char *first_line)
{
    linx_pagination_t pages;
    assert(linx_paginate(&pages, source, line_width, lines, display_width, NULL));
    char collected[1024];
    size_t used = 0U;
    assert(pages.offsets[0] == 0U && pages.offsets[pages.count] == strlen(source));
    for (size_t p = 0U; p < pages.count; p++) {
        char page[1024];
        assert(pages.offsets[p] < pages.offsets[p + 1U]);
        assert(((unsigned char)source[pages.offsets[p]] & 0xc0U) != 0x80U);
        assert(linx_page_text(&pages, p, source, line_width, display_width, NULL, page, sizeof(page)));
        const char *begin = page;
        size_t line_count = 0U;
        for (const char *end = page;; end++) {
            if (*end == '\n' || *end == '\0') {
                size_t bytes = (size_t)(end - begin);
                assert(display_width(begin, bytes, NULL) <= line_width);
                assert(!list_prefix_only(begin, bytes));
                if (p == 0U && line_count == 0U && first_line) {
                    if (bytes != strlen(first_line) || memcmp(begin, first_line, bytes) != 0) {
                        fprintf(stderr, "Unexpected first line at %dpx: [%.*s], expected [%s]\n",
                                (int)line_width, (int)bytes, begin, first_line);
                    }
                    assert(bytes == strlen(first_line));
                    assert(memcmp(begin, first_line, bytes) == 0);
                }
                line_count++;
                begin = end + 1;
            } else collected[used++] = *end;
            if (*end == '\0') break;
        }
        assert(line_count <= lines);
    }
    collected[used] = '\0';
    char original[1024];
    size_t original_used = 0U;
    for (size_t i = 0U; source[i] != '\0'; i++) {
        if (source[i] != '\n') original[original_used++] = source[i];
    }
    original[original_used] = '\0';
    /* Include every space and tab; only the existing newline boundaries differ. */
    assert(strcmp(original, collected) == 0);
}

static void cjk_lists_and_mixed_words(void)
{
    const char *login = "- 空密码提示改为：请输入密码。";
    display_pagination(login, 208, 7U, "- 空密码提示改为：请输入密");
    display_pagination(login, 200, 7U, "- 空密码提示改为：请输入");
    /* One-line pages exercise the same protection at an actual page edge. */
    display_pagination(login, 208, 1U, "- 空密码提示改为：请输入密");
    display_pagination("* 空密码提示改为：请输入密码。", 200, 1U, "* 空密码提示改为：请输入");
    display_pagination("1. 空密码提示改为：请输入密码。", 208, 1U, "1. 空密码提示改为：请输入");
    display_pagination("12) 空密码提示改为：请输入密码。", 200, 1U, "12) 空密码提示改为：请输");
    display_pagination("  -\t空密码提示改为：请输入密码。", 208, 2U, NULL);
    /* A long first English word must hard-wrap with its marker, not orphan it. */
    display_pagination("- ABCDEFGHIJKLMNOPQRSTUVWXYZ", 80, 1U, "- ABCDEFGH");
    display_pagination("* First word remains readable", 80, 2U, "* First ");
    display_pagination("Alpha beta gamma", 80, 1U, "Alpha ");
    display_pagination("前言 Alpha beta完成后继续检查。", 128, 2U, "前言 Alpha beta");
    display_pagination("先检查 ABCDEFGHIJK 的结果。", 128, 1U, "先检查 ");
    display_pagination("已按你的要求让异常提示更清楚，暂不发布。\n\n本轮修改\n"
                       "- 空密码提示改为：请输入密码。\n"
                       "1. 令牌过期提示改为：登录已过期，请重新登录。\n"
                       "* Codex 回答会保留  两个空格和\t制表符。", 200, 7U, NULL);
}

static void wrapping_bounded_spans(void)
{
    const char *source = "前缀\n- 空密码提示改为：请输入密码。\n后缀";
    size_t start = (size_t)(strchr(source, '\n') + 1 - source);
    size_t limit = (size_t)(strrchr(source, '\n') - source);
    linx_text_line_t line;
    assert(linx_wrap_span(source, start, limit, 208, display_width, NULL, &line));
    assert(line.end == start + strlen("- 空密码提示改为：请输入密"));
    assert(line.next == line.end);
    assert(linx_wrap_span(source, line.next, limit, 208, display_width, NULL, &line));
    assert(line.end == limit && line.next == limit);
    assert(linx_wrap_span(source, limit, strlen(source), 208, display_width, NULL, &line));
    assert(line.end == limit && line.next == limit + 1U);
    assert(linx_wrap_span(source, 0U, strlen(source), 208, display_width, NULL, &line));
    assert(line.end == start - 1U && line.next == start);
    assert(!linx_wrap_span(NULL, 0U, 1U, 208, display_width, NULL, &line));
    assert(!linx_wrap_span(source, 0U, strlen(source) + 1U, 208, display_width, NULL, &line));
    assert(!linx_wrap_span(source, 1U, limit, 208, display_width, NULL, &line));
    assert(!linx_wrap_span(source, 0U, 1U, 208, display_width, NULL, &line));
    assert(!linx_wrap_span(source, limit, limit, 208, display_width, NULL, &line));
    assert(!linx_wrap_span(source, limit, 0U, 208, display_width, NULL, &line));
    assert(!linx_wrap_span(source, 0U, limit, 0, display_width, NULL, &line));
    assert(!linx_wrap_span(source, 0U, limit, 208, NULL, NULL, &line));
    assert(!linx_wrap_span(source, 0U, limit, 208, display_width, NULL, NULL));
    assert(line.end == 0U && line.next == 0U);
}

static void punctuation_boundaries(const char *source, int32_t line_width, size_t lines)
{
    static const char *const closing[] = {
        "，", "。", "？", "！", "）", "】", "”", "’", "、", "：", "；", "…",
        "〉", "》", "」", "』", "〕", "〗", "〙", "〛", "］", "｝",
        ",", ".", "?", "!", ")", "]", "}", ":", ";"
    };
    static const char *const opening[] = {
        "（", "【", "“", "‘", "〈", "《", "「", "『", "〔", "〖", "〘", "〚", "［", "｛", "(", "[", "{"
    };
    linx_pagination_t pages;
    assert(linx_paginate(&pages, source, line_width, lines, width, NULL));
    char collected[1024];
    size_t used = 0U;
    for (size_t p = 0U; p < pages.count; p++) {
        char page[1024];
        assert(linx_page_text(&pages, p, source, line_width, width, NULL, page, sizeof(page)));
        const char *begin = page;
        for (const char *end = page;; end++) {
            if (*end == '\n' || *end == '\0') {
                assert(width(begin, (size_t)(end - begin), NULL) <= line_width);
                if (end > begin) {
                    for (size_t i = 0U; i < sizeof(closing) / sizeof(closing[0]); i++) {
                        assert(strncmp(begin, closing[i], strlen(closing[i])) != 0);
                    }
                    const char *last = end - 1;
                    while (last > begin && ((unsigned char)*last & 0xc0U) == 0x80U) last--;
                    for (size_t i = 0U; i < sizeof(opening) / sizeof(opening[0]); i++) {
                        size_t bytes = strlen(opening[i]);
                        assert((size_t)(end - last) != bytes || memcmp(last, opening[i], bytes) != 0);
                    }
                }
                begin = end + 1;
            } else collected[used++] = *end;
            if (*end == '\0') break;
        }
    }
    collected[used] = '\0';
    char original[1024];
    size_t original_used = 0U;
    for (size_t i = 0U; source[i] != '\0'; i++) {
        if (source[i] != '\n') original[original_used++] = source[i];
    }
    original[original_used] = '\0';
    assert(strcmp(original, collected) == 0);
}

static void pagination_preserves_utf8(void)
{
    const char *text = "暖白方向已经确定。\n\n整体风格\n背景采用暖白，正文使用墨黑。\nA very long path/to/a/source/file.c can wrap safely.\n最后一段。";
    linx_pagination_t pages;
    assert(linx_paginate(&pages, text, 50, 3, width, NULL));
    assert(pages.complete && pages.count > 3U);
    assert(pages.offsets[0] == 0U);
    assert(pages.offsets[pages.count] == strlen(text));
    char collected[1024];
    size_t collected_bytes = 0U;
    for (size_t i = 0; i < pages.count; i++) {
        assert(pages.offsets[i] < pages.offsets[i + 1U]);
        assert(((unsigned char)text[pages.offsets[i]] & 0xc0U) != 0x80U);
        char out[256];
        assert(linx_page_text(&pages, i, text, 50, width, NULL, out, sizeof(out)));
        for (size_t p = 0U; out[p] != '\0'; p++) {
            if (out[p] != '\n') collected[collected_bytes++] = out[p];
        }
        size_t lines = 1U;
        const char *begin = out;
        for (const char *p = out;; p++) {
            if (*p == '\n' || *p == '\0') {
                assert(width(begin, (size_t)(p - begin), NULL) <= 50);
                begin = p + 1;
                if (*p == '\n') lines++;
            }
            if (*p == '\0') break;
        }
        assert(lines <= 3U);
    }
    collected[collected_bytes] = '\0';
    char original[1024];
    size_t original_bytes = 0U;
    for (size_t i = 0U; text[i] != '\0'; i++) {
        if (text[i] != '\n') original[original_bytes++] = text[i];
    }
    original[original_bytes] = '\0';
    assert(strcmp(original, collected) == 0);
    char small[4];
    assert(!linx_page_text(&pages, 0, text, 50, width, NULL, small, sizeof(small)));
    assert(small[0] == '\0');
}

static void all_punctuation_and_unicode_ranges(void)
{
    static const char *const closing[] = {
        ",", ".", "!", "?", ":", ";", ")", "]", "}", "、", "。", "，", "．", "！", "？",
        "：", "；", "…", "’", "”", "〉", "》", "」", "』", "】", "〕", "〗", "〙", "〛", "）", "］", "｝"
    };
    static const char *const opening[] = {
        "(", "[", "{", "‘", "“", "〈", "《", "「", "『", "【", "〔", "〖", "〘", "〚", "（", "［", "｛"
    };
    char source[128];
    for (size_t i = 0U; i < sizeof(closing) / sizeof(closing[0]); i++) {
        snprintf(source, sizeof(source), "甲乙丙%s丁戊己。", closing[i]);
        punctuation_boundaries(source, 30, 1U);
    }
    for (size_t i = 0U; i < sizeof(opening) / sizeof(opening[0]); i++) {
        snprintf(source, sizeof(source), "甲乙%s丙丁戊。", opening[i]);
        punctuation_boundaries(source, 30, 1U);
    }
    /* 两字节拉丁字母、韩文、兼容汉字、四字节汉字和表情都不能被拆开。 */
    display_pagination("éΩ甲한豈𠀀😀继续。", 64, 1U, NULL);
    display_pagination("甲乙丙 \t。丁戊己。", 48, 1U, NULL);
    display_pagination("甲乙（ \t丙丁戊。", 48, 1U, NULL);
    const char *narrow = "甲。";
    linx_text_line_t line;
    assert(linx_wrap_span(narrow, 0U, strlen(narrow), 10, width, NULL, &line));
    assert(line.end == strlen("甲") && line.next == line.end);
}

static void incomplete_list_markers_are_preserved(void)
{
    static const char *const sources[] = {
        " ", "\t", "-", "*", "123", "123x", "1.", "2)", "2)text", "-text", "*text",
        "\t123x text", "(plain text)", "abc\n\nlast."
    };
    for (size_t i = 0U; i < sizeof(sources) / sizeof(sources[0]); i++) {
        display_pagination(sources[i], 208, 2U, NULL);
    }
    linx_pagination_t pages;
    char out[8];
    assert(linx_paginate(&pages, "- \t", 208, 2U, width, NULL));
    assert(linx_page_text(&pages, 0U, "- \t", 208, width, NULL, out, sizeof(out)));
    assert(strcmp(out, "- \t") == 0);
}

static void pagination_limits_and_invalid_outputs(void)
{
    linx_pagination_t pages;
    memset(&pages, 0xa5, sizeof(pages));
    assert(!linx_paginate(NULL, "abc", 10, 1U, width, NULL));
    assert(!linx_paginate(&pages, NULL, 10, 1U, width, NULL));
    assert(!pages.complete && pages.count == 0U && pages.offsets[0] == 0U);
    assert(!linx_paginate(&pages, "abc", 10, 1U, NULL, NULL));
    assert(!linx_paginate(&pages, "abc", -1, 1U, width, NULL));
    assert(!linx_paginate(&pages, "abc", 10, 0U, width, NULL));
    char oversized[LINX_TEXT_LIMIT + 2U];
    memset(oversized, 'a', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    assert(!linx_paginate(&pages, oversized, 1000, 100U, width, NULL));
    oversized[LINX_TEXT_LIMIT] = '\0';
    assert(linx_paginate(&pages, oversized, 1000, 100U, width, NULL));
    assert(pages.complete && pages.offsets[pages.count] == LINX_TEXT_LIMIT);

    char source[(LINX_PAGE_LIMIT + 1U) * 2U + 1U];
    for (size_t i = 0U; i < LINX_PAGE_LIMIT; i++) {
        source[i * 2U] = 'a'; source[i * 2U + 1U] = '\n';
    }
    source[LINX_PAGE_LIMIT * 2U] = '\0';
    assert(linx_paginate(&pages, source, 10, 1U, width, NULL));
    assert(pages.count == LINX_PAGE_LIMIT && pages.complete);
    source[LINX_PAGE_LIMIT * 2U] = 'a';
    source[LINX_PAGE_LIMIT * 2U + 1U] = '\n';
    source[LINX_PAGE_LIMIT * 2U + 2U] = '\0';
    assert(!linx_paginate(&pages, source, 10, 1U, width, NULL));
    assert(!pages.complete && pages.count == LINX_PAGE_LIMIT);

    char out[16] = "unchanged";
    assert(!linx_page_text(&pages, 0U, source, 10, width, NULL, out, sizeof(out)));
    assert(out[0] == '\0');
    assert(linx_paginate(&pages, "abcd", 10, 2U, width, NULL));
    assert(linx_page_text(&pages, 0U, "abcd", 10, width, NULL, out, 6U));
    assert(strcmp(out, "ab\ncd") == 0);
    assert(!linx_page_text(&pages, 0U, "abcd", 10, width, NULL, out, 5U));
    assert(out[0] == '\0');
    assert(!linx_page_text(NULL, 0U, "abcd", 10, width, NULL, out, sizeof(out)));
    assert(!linx_page_text(&pages, pages.count, "abcd", 10, width, NULL, out, sizeof(out)));
    assert(!linx_page_text(&pages, 0U, NULL, 10, width, NULL, out, sizeof(out)));
    assert(!linx_page_text(&pages, 0U, "abcd", 10, NULL, NULL, out, sizeof(out)));
    assert(!linx_page_text(&pages, 0U, "abcd", 10, width, NULL, NULL, sizeof(out)));
    out[0] = 'x';
    assert(!linx_page_text(&pages, 0U, "abcd", 10, width, NULL, out, 0U));
    assert(out[0] == 'x');
}

static void headings_and_menu_bounds(void)
{
    const char *source = "正文。\n- 列表\n\n* 列表\n\n1. 列表\n\n"
                         "句号。\n\n冒号：\n\n感叹！\n\n问号？\n\n"
                         "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\n\n结束。";
    display_pagination(source, 208, 2U, NULL);
    char headings[2048] = "";
    for (size_t i = 0U; i < 100U; i++) strcat(headings, "a.\nb.\nTitle\n\n");
    linx_pagination_t pages;
    assert(!linx_paginate(&pages, headings, 100, 3U, width, NULL));
    assert(!pages.complete && pages.count == LINX_PAGE_LIMIT);
    bool enabled[] = {false, false, false};
    assert(linx_menu_step(99U, 1, enabled, 0U) == 0U);
    assert(linx_menu_step(99U, 0, enabled, 3U) == 2U);
    assert(linx_menu_step(0U, 0, enabled, 3U) == 0U);
    assert(linx_menu_step(2U, -1, enabled, 3U) == 2U);
    assert(linx_menu_step(99U, -1, NULL, 3U) == 1U);
    assert(linx_menu_step(0U, -1, NULL, 3U) == 0U);
}

int main(void)
{
    cjk_lists_and_mixed_words();
    wrapping_bounded_spans();
    pagination_preserves_utf8();
    all_punctuation_and_unicode_ranges();
    incomplete_list_markers_are_preserved();
    pagination_limits_and_invalid_outputs();
    headings_and_menu_bounds();
    /* Each punctuation would land exactly at a wrap or a one-line page start
     * without the backtracking rule. Every original byte must survive. */
    punctuation_boundaries("甲乙丙。丁戊己，庚辛壬？癸甲乙！丙丁戊）己庚辛】壬癸甲”乙丙丁", 30, 1);
    punctuation_boundaries("甲乙（丙丁）戊己“庚辛”壬癸【甲乙】丙丁", 30, 2);
    punctuation_boundaries("甲乙丙。丁戊己，庚辛壬？癸甲乙！丙丁戊。", 40, 3);
    punctuation_boundaries("甲乙丙。）丁戊己。】庚辛壬。”癸甲乙。", 50, 1);
    linx_pagination_t pages;
    char out[8];
    assert(linx_paginate(&pages, "", 10, 1, width, NULL));
    assert(pages.count == 1U);
    assert(linx_page_text(&pages, 0, "", 10, width, NULL, out, sizeof(out)));
    assert(out[0] == '\0');
    assert(!linx_paginate(&pages, "abc", 0, 1, width, NULL));
    const char *heading = "第一行。\n第二行。\n整体风格\n\n这里是内容。";
    assert(linx_paginate(&pages, heading, 100, 3, width, NULL));
    assert(pages.offsets[1] == (size_t)(strstr(heading, "整体风格") - heading));
    char many[200];
    memset(many, 'a', sizeof(many) - 1U);
    many[sizeof(many) - 1U] = '\0';
    assert(!linx_paginate(&pages, many, 5, 1, width, NULL));
    assert(!pages.complete);
    bool enabled[] = {true, false, true, false};
    assert(linx_menu_step(0, -1, enabled, 4) == 0U);
    assert(linx_menu_step(0, 1, enabled, 4) == 2U);
    assert(linx_menu_step(2, 1, enabled, 4) == 2U);
    assert(linx_menu_step(2, -1, enabled, 4) == 0U);
    assert(linx_menu_step(0, 1, NULL, 3) == 1U);
    puts("LinX pagination and bounded navigation: PASS");
    return 0;
}
