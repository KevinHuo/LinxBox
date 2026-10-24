#include "linx_ui.h"
#include "linx_assets.h"
#include "linx_navigation.h"
#include "linx_reader.h"
#include "linx_input.h"
#include "bsp_display_rounding.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define WIDTH 240
#define HEIGHT 320
static uint16_t s_draw_buffer[WIDTH * 40];
/* Emulates the LCD controller's retained pixels; not another device draw buffer. */
static uint16_t s_frame[HEIGHT][WIDTH];
static lv_display_t *s_display;
static uint64_t s_now;
static const char *s_screens;
static size_t s_key_count;
static size_t s_label_checks;
static size_t s_captures;
static size_t s_reader_line_checks;

static uint32_t decode(const char *text, size_t bytes, size_t *offset)
{
    if (*offset >= bytes) return 0U;
    unsigned char first = (unsigned char)text[(*offset)++];
    if (first < 0x80U) return first;
    unsigned remaining = (first & 0xe0U) == 0xc0U ? 1U : (first & 0xf0U) == 0xe0U ? 2U : 3U;
    uint32_t value = first & (remaining == 1U ? 0x1fU : remaining == 2U ? 0x0fU : 0x07U);
    while (remaining-- > 0U) {
        assert(*offset < bytes);
        unsigned char continuation = (unsigned char)text[(*offset)++];
        assert((continuation & 0xc0U) == 0x80U);
        value = (value << 6U) | (continuation & 0x3fU);
    }
    return value;
}

static bool covered(const lv_font_t *font, uint32_t codepoint)
{
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0U) && !glyph.is_placeholder;
}

static void check_string(const lv_font_t *font, const char *text)
{
    size_t length = strlen(text);
    size_t offset = 0U;
    while (offset < length) {
        uint32_t cp = decode(text, length, &offset);
        if (cp < 0x20U) continue;
        if (!covered(font, cp)) {
            fprintf(stderr, "Missing glyph U+%04X on page %d: %s\n", (unsigned)cp, linx_ui_page(), text);
            abort();
        }
    }
}

static void audit_objects(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *text = lv_label_get_text(object);
        const lv_font_t *font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
        assert(font == &linx_font_12 || font == &linx_font_14 || font == &linx_font_16 ||
               font == &linx_font_20 || font == &linx_font_16_bold ||
               font == &linx_font_20_bold || font == &linx_font_24_bold || font == &linx_font_28_bold);
        check_string(font, text);
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < WIDTH && area.y2 < HEIGHT);
        if (text[0] != '\0' && lv_label_get_long_mode(object) != LV_LABEL_LONG_DOT) {
            lv_point_t natural;
            lv_text_get_size(&natural, text, font,
                             lv_obj_get_style_text_letter_space(object, LV_PART_MAIN),
                             lv_obj_get_style_text_line_space(object, LV_PART_MAIN),
                             lv_obj_get_content_width(object), LV_TEXT_FLAG_NONE);
            if (natural.y > lv_obj_get_content_height(object)) {
                fprintf(stderr, "Text clipped on page %d: wanted %dpx, has %dpx: %s\n",
                        linx_ui_page(), (int)natural.y, (int)lv_obj_get_content_height(object), text);
                abort();
            }
        }
        ++s_label_checks;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        audit_objects(lv_obj_get_child(object, (int32_t)i));
}

static lv_obj_t *label_at(lv_obj_t *object, int x, int y, int width, int height)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && lv_obj_get_x(object) == x &&
        lv_obj_get_y(object) == y && lv_obj_get_width(object) == width &&
        lv_obj_get_height(object) == height) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = label_at(lv_obj_get_child(object, (int32_t)i), x, y, width, height);
        if (found) return found;
    }
    return NULL;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    lv_draw_buf_t *draw = lv_display_get_buf_active(display);
    assert(draw != NULL);
    assert(draw->header.cf == LV_COLOR_FORMAT_RGB565);
    assert(draw->data_size == sizeof(s_draw_buffer));
    assert((uint32_t)(area->y2 - area->y1 + 1) * draw->header.stride <= sizeof(s_draw_buffer));
    for (int y = area->y1; y <= area->y2; ++y) {
        int32_t x1;
        int32_t x2;
        assert(bsp_display_rounded_row_span(y, WIDTH, HEIGHT, 30, &x1, &x2));
        const uint16_t *row = (const uint16_t *)(pixels + (y - area->y1) * draw->header.stride);
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT);
            s_frame[y][x] = x >= x1 && x <= x2 ? row[x - area->x1] : 0U;
        }
    }
    lv_display_flush_ready(display);
}

static void settle(void)
{
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(s_display);
    assert(lv_mem_test() == LV_RESULT_OK);
    assert(!linx_ui_debug().render_overflow);
    assert(linx_ui_debug().labels_used <= 24U);
    audit_objects(lv_screen_active());
}

static bool visible_text_contains(lv_obj_t *object, const char *needle)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(object, &lv_label_class) && strstr(lv_label_get_text(object), needle)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        if (visible_text_contains(lv_obj_get_child(object, (int32_t)i), needle)) return true;
    return false;
}

static void capture(const char *name)
{
    settle();
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/%s.ppm", s_screens, name) > 0);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            uint16_t pixel = s_frame[y][x];
            unsigned char rgb[] = {
                (unsigned char)(((pixel >> 11) & 31U) * 255U / 31U),
                (unsigned char)(((pixel >> 5) & 63U) * 255U / 63U),
                (unsigned char)((pixel & 31U) * 255U / 31U)
            };
            assert(fwrite(rgb, 1, 3, file) == 3U);
        }
    }
    assert(fclose(file) == 0);
    ++s_captures;
}

static void advance(uint64_t elapsed)
{
    assert(elapsed <= UINT32_MAX);
    s_now += elapsed;
    lv_tick_inc((uint32_t)elapsed);
    linx_ui_tick(s_now);
    settle();
}

static void key(linx_ui_key_t value)
{
    linx_ui_handle_key(value);
    ++s_key_count;
    settle();
}

static void select_item(size_t selected)
{
    for (size_t attempt = 0; linx_ui_debug().selected != selected && attempt < 32U; ++attempt) {
        size_t before = linx_ui_debug().selected;
        key(before < selected ? LINX_UI_KEY_DOWN : LINX_UI_KEY_UP);
        assert(linx_ui_debug().selected != before);
    }
    assert(linx_ui_debug().selected == selected);
}

static void to_tasks(void)
{
    for (size_t i = 0; linx_ui_page() != LINX_UI_TASKS && i < 16U; ++i) key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_TASKS);
}

static void enter_task(uint32_t task_id)
{
    to_tasks();
    select_item(task_id - 1U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().task_id == task_id);
}

static void overview_action(size_t action)
{
    assert(linx_ui_page() == LINX_UI_OVERVIEW);
    select_item(action);
    key(LINX_UI_KEY_OK);
}

static void reset_scene(void)
{
    to_tasks();
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_SETTINGS);
    select_item(0U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_RESET && linx_ui_debug().selected == 0U);
    select_item(1U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_TASKS);
}

static void settings_toggle(size_t index)
{
    to_tasks();
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_SETTINGS);
    select_item(index);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_TASKS);
}

static void start_voice_from_overview(void)
{
    overview_action(1U);
    assert(linx_ui_page() == LINX_UI_VOICE);
}

static uint32_t finish_and_send(void)
{
    assert(linx_ui_page() == LINX_UI_VOICE);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_CONFIRM);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_RECEIPT);
    assert(linx_ui_debug().command_id != 0U);
    return linx_ui_debug().command_id;
}

/* Mirrors main.c: reduce queued event, tick, invalidate automatic transition,
 * dispatch an action, then invalidate any manual page transition. The paired
 * source contract test prevents main.c silently changing this ordering. */
static void pump(linx_input_t *input, bool has_event, linx_input_event_t event,
                 uint64_t event_ms, uint64_t now_ms)
{
    linx_input_action_t action = has_event ? linx_input_event(input, 2U, event, event_ms) : LINX_INPUT_NONE;
    assert(now_ms >= s_now && now_ms - s_now <= UINT32_MAX);
    lv_tick_inc((uint32_t)(now_ms - s_now));
    s_now = now_ms;
    linx_ui_page_t before_tick = linx_ui_page();
    linx_ui_tick(now_ms);
    if (linx_ui_page() != before_tick) {
        linx_input_barrier(input, now_ms);
        action = LINX_INPUT_NONE;
    }
    if (action != LINX_INPUT_NONE) {
        linx_ui_page_t previous = linx_ui_page();
        linx_ui_key_t mapped = action == LINX_INPUT_UP ? LINX_UI_KEY_UP :
                               action == LINX_INPUT_DOWN ? LINX_UI_KEY_DOWN :
                               action == LINX_INPUT_BACK ? LINX_UI_KEY_OK_LONG : LINX_UI_KEY_OK;
        linx_ui_handle_key(mapped);
        ++s_key_count;
        if (linx_ui_page() != previous) linx_input_barrier(input, now_ms);
    }
    settle();
}

static void test_timeout_release_barriers(void)
{
    for (size_t scenario = 0U; scenario < 2U; ++scenario) {
        reset_scene();
        enter_task(LINX_TASK_DESIGN);
        start_voice_from_overview();
        linx_input_t input;
        linx_input_init(&input, 500U);
        uint64_t start = s_now;
        linx_input_barrier(&input, start);
        pump(&input, true, LINX_INPUT_PRESS, start + 7990U, start + 7990U);
        assert(linx_ui_page() == LINX_UI_VOICE);
        if (scenario == 0U) {
            /* Release produces OK in the same loop in which voice times out. */
            pump(&input, true, LINX_INPUT_RELEASE, start + 8000U, start + 8000U);
        } else {
            pump(&input, false, LINX_INPUT_RELEASE, 0U, start + 8000U);
            assert(linx_ui_page() == LINX_UI_CONFIRM);
            /* The held press began on VOICE; its later release cannot submit. */
            pump(&input, true, LINX_INPUT_RELEASE, start + 8040U, start + 8040U);
        }
        assert(linx_ui_page() == LINX_UI_CONFIRM && input.held_key == -1);
        assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->pending.id == 0U);
        assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 1U);
        /* A stale queued press/release is rejected after the transition too. */
        pump(&input, true, LINX_INPUT_PRESS, start + 7995U, s_now);
        pump(&input, true, LINX_INPUT_RELEASE, start + 8000U, s_now);
        assert(linx_ui_page() == LINX_UI_CONFIRM);
        uint64_t fresh = s_now + 10U;
        pump(&input, true, LINX_INPUT_PRESS, fresh, fresh);
        assert(linx_ui_page() == LINX_UI_CONFIRM);
        pump(&input, true, LINX_INPUT_RELEASE, fresh + 40U, fresh + 40U);
        assert(linx_ui_page() == LINX_UI_RECEIPT);
        uint32_t id = linx_ui_debug().command_id;
        assert(id != 0U && linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->pending.id == id);
        /* The release cannot run a second time through the new receipt page. */
        pump(&input, true, LINX_INPUT_RELEASE, fresh + 40U, fresh + 40U);
        assert(linx_ui_page() == LINX_UI_RECEIPT && linx_ui_debug().command_id == id);
    }
    puts("Physical input regression: simultaneous timeout/release and release after timeout require a fresh gesture PASS");
}

static void test_missing_image(void)
{
    assert(linx_asset_image(LINX_IMAGE_RESULT, 0U) == NULL);
    assert(linx_asset_image(LINX_IMAGE_TASK_LIST, 0U) != NULL);
    assert(linx_asset_image(LINX_IMAGE_VOICE, 0U) != NULL);
    enter_task(LINX_TASK_DESIGN);
    overview_action(3U);
    key(LINX_UI_KEY_DOWN);
    uint32_t source = linx_ui_debug().turn_id;
    assert(linx_ui_page() == LINX_UI_IMAGES && linx_ui_debug().image_id == LINX_IMAGE_RESULT);
    assert(visible_text_contains(lv_screen_active(), "图片暂不可用"));
    assert(visible_text_contains(lv_screen_active(), "结果阅读"));
    capture("missing-01-overview");
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_IMAGE_MENU);
    select_item(0U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL);
    assert(visible_text_contains(lv_screen_active(), "图片暂不可用"));
    capture("missing-02-detail");
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().image_view == 2U && visible_text_contains(lv_screen_active(), "图片暂不可用"));
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_VOICE && linx_ui_debug().image_id == LINX_IMAGE_RESULT &&
           linx_ui_debug().turn_id == source);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL);
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_IMAGES && linx_ui_debug().image_id == LINX_IMAGE_RESULT);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().image_id == LINX_IMAGE_TASK_LIST);
    assert(!visible_text_contains(lv_screen_active(), "图片暂不可用"));
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_OVERVIEW);
    overview_action(0U);
    assert(linx_ui_page() == LINX_UI_READER && label_at(lv_screen_active(), 16, 74, 208, 22) != NULL);
    to_tasks();
    puts("Missing image injection: readable placeholder, original metadata, reply, return and full-text fallback PASS");
}

static void check_inventory(void)
{
    const lv_font_t *fonts[] = {&linx_font_12, &linx_font_14, &linx_font_16,
                              &linx_font_20, &linx_font_16_bold, &linx_font_20_bold, &linx_font_24_bold};
    const size_t font_count = sizeof(fonts) / sizeof(fonts[0]);
    FILE *file = fopen(LINX_REPO_ROOT "/assets/fonts/codepoints.txt", "r");
    assert(file != NULL);
    unsigned codepoint;
    size_t count = 0U;
    while (fscanf(file, "%x", &codepoint) == 1) {
        for (size_t i = 0; i < font_count; ++i) assert(covered(fonts[i], codepoint));
        ++count;
    }
    fclose(file);
    assert(count > 400U);
    for (size_t i = 0; i < font_count; ++i) {
        assert(!covered(fonts[i], 0x9F98U));
        for (size_t t = 0; t < linx_content_text_count(); ++t)
            check_string(fonts[i], linx_content_text_at(t));
    }
    file = fopen(LINX_REPO_ROOT "/assets/fonts/codepoints-ascii.txt", "r");
    assert(file != NULL);
    size_t ascii_count = 0U;
    while (fscanf(file, "%x", &codepoint) == 1) {
        assert(codepoint == 32U + ascii_count && codepoint <= 126U);
        assert(covered(&linx_font_28_bold, codepoint));
        ++ascii_count;
    }
    fclose(file);
    assert(ascii_count == 95U);
    assert(!covered(&linx_font_28_bold, 0x4E2DU));
    assert(!covered(&linx_font_28_bold, 0x9F98U));
    printf("Fonts: %zu inventory codepoints × %zu CJK variants; %zu ASCII at28px bold; U+9F98 negative controls PASS\n",
           count, font_count, ascii_count);
}

static int32_t measure(const char *value, size_t bytes, void *context)
{
    const lv_font_t *font = context;
    size_t offset = 0U;
    uint32_t current = decode(value, bytes, &offset);
    int32_t width = 0;
    while (current != 0U) {
        uint32_t next = decode(value, bytes, &offset);
        width += lv_font_get_glyph_width(font, current, next);
        current = next;
    }
    return width;
}

static bool codepoint_in(uint32_t codepoint, const char *characters)
{
    size_t offset = 0U;
    size_t length = strlen(characters);
    while (offset < length)
        if (decode(characters, length, &offset) == codepoint) return true;
    return false;
}

static int32_t reader_measure(const char *value, size_t bytes, linx_reader_kind_t kind, void *context)
{
    (void)context;
    const lv_font_t *font = kind == LINX_READER_HEADING ? &linx_font_16_bold : &linx_font_16;
    return measure(value, bytes, (void *)font);
}

static size_t expected_list_indent(const char *source, const linx_reader_row_t *row)
{
    size_t start = row->start;
    while (start > 0U && source[start - 1U] != '\n') start--;
    while (source[start] == ' ' || source[start] == '\t' || source[start] == '\r') start++;
    if (source[start] == '-' || source[start] == '*') return LINX_READER_LIST_INDENT;
    size_t end = start;
    while (source[end] >= '0' && source[end] <= '9') end++;
    assert(end > start && (source[end] == '.' || source[end] == ')'));
    end++;
    size_t measured = (size_t)reader_measure(source + start, end - start, LINX_READER_BODY, NULL) + 2U;
    return measured > LINX_READER_LIST_INDENT ? measured : LINX_READER_LIST_INDENT;
}

static void check_reader_row(const char *source, const linx_reader_row_t *row)
{
    assert(row->length > 0U && row->start + row->length <= strlen(source));
    assert(((unsigned char)source[row->start] & 0xc0U) != 0x80U);
    assert(((unsigned char)source[row->start + row->length] & 0xc0U) != 0x80U);
    assert(row->x + reader_measure(source + row->start, row->length, row->kind, NULL) <= 208);
    assert(row->y + LINX_READER_LINE_HEIGHT <= 200U);
    assert(memchr(source + row->start, '\n', row->length) == NULL);
    size_t offset = 0U;
    uint32_t first = 0U;
    uint32_t last = 0U;
    while (offset < row->length) {
        uint32_t cp = decode(source + row->start, row->length, &offset);
        if (cp == ' ' || cp == '\t') continue;
        if (first == 0U) first = cp;
        last = cp;
    }
    const char *closing = "、。，．！？：；…’”〉》」』】〕〗〙〛）］｝";
    const char *opening = "‘“〈《「『【〔〖〘〚（［｛";
    if (codepoint_in(first, closing) || codepoint_in(last, opening)) {
        fprintf(stderr, "CJK punctuation at illegal line/page edge: %.*s\n",
                (int)row->length, source + row->start);
        abort();
    }
    assert(!(row->length == 1U && (first == '-' || first == '*')));
    if (row->kind == LINX_READER_LIST) {
        assert(row->x == expected_list_indent(source, row));
        assert(row->first_list_line == (row->marker_length > 0U));
        if (row->first_list_line && source[row->marker_start] != '-' && source[row->marker_start] != '*') {
            assert(measure(source + row->marker_start, row->marker_length, (void *)&linx_font_12) <= row->x);
            assert(reader_measure(source + row->marker_start, row->marker_length,
                                  LINX_READER_BODY, NULL) + 2 <= row->x);
        }
    } else assert(row->x == 0U && !row->first_list_line && row->marker_length == 0U);
    ++s_reader_line_checks;
}

static linx_reader_layout_t check_reader_layout(const char *source)
{
    linx_reader_layout_t layout;
    assert(linx_reader_paginate(&layout, source, 208U, 200U, reader_measure, NULL));
    assert(layout.complete && layout.count > 0U && layout.count <= LINX_PAGE_LIMIT);
    size_t length = strlen(source);
    assert(layout.offsets[0] == 0U && layout.offsets[layout.count] == length);
    unsigned char rendered[LINX_TEXT_LIMIT] = {0};
    char recovered[LINX_TEXT_LIMIT + 1U];
    size_t used = 0U;
    for (size_t p = 0U; p < layout.count; p++) {
        linx_reader_page_t page;
        assert(linx_reader_get_page(&layout, p, source, reader_measure, NULL, &page));
        assert(page.source_start == layout.offsets[p] && page.source_end == layout.offsets[p + 1U]);
        assert(page.source_start <= page.source_end && page.source_end <= length);
        if (length) assert(page.source_start < page.source_end);
        assert(page.count <= LINX_READER_ROWS);
        memcpy(recovered + used, source + page.source_start, page.source_end - page.source_start);
        used += page.source_end - page.source_start;
        for (size_t r = 0U; r < page.count; r++) {
            const linx_reader_row_t *row = &page.rows[r];
            check_reader_row(source, row);
            assert(row->start >= page.source_start && row->start + row->length <= page.source_end);
            if (r == 0U) assert(row->y == 0U);
            else assert(row->y >= page.rows[r - 1U].y + LINX_READER_LINE_HEIGHT);
            for (size_t i = row->start; i < row->start + row->length; i++) {
                assert(!rendered[i]);
                rendered[i] = 1U;
            }
            for (size_t i = row->marker_start; i < row->marker_start + row->marker_length; i++) {
                assert(i >= page.source_start && i < page.source_end && !rendered[i]);
                rendered[i] = 1U;
            }
            if (r + 1U == page.count) assert(row->kind != LINX_READER_HEADING);
        }
    }
    recovered[used] = '\0';
    assert(used == length && strcmp(recovered, source) == 0);
    for (size_t i = 0U; i < length; i++)
        assert(rendered[i] || source[i] == '\n' || source[i] == '\r' || source[i] == ' ' || source[i] == '\t');
    return layout;
}

static size_t visible_reader_rows(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return 0U;
    size_t count = lv_obj_check_type(object, &lv_label_class) && lv_obj_get_height(object) == 22 &&
                   lv_obj_get_y(object) >= 74 && lv_obj_get_y(object) + 22 <= 274 ? 1U : 0U;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        count += visible_reader_rows(lv_obj_get_child(object, (int32_t)i));
    return count;
}

static void check_visible_reader(const linx_reader_layout_t *layout, size_t index, const char *source)
{
    linx_reader_page_t page;
    assert(linx_reader_get_page(layout, index, source, reader_measure, NULL, &page));
    assert(visible_reader_rows(lv_screen_active()) == page.count);
    for (size_t r = 0U; r < page.count; r++) {
        const linx_reader_row_t *row = &page.rows[r];
        check_reader_row(source, row);
        lv_obj_t *label = label_at(lv_screen_active(), 16 + row->x, 74 + row->y, 208 - row->x, 22);
        assert(label != NULL);
        const char *actual = lv_label_get_text(label);
        assert(strlen(actual) == row->length && memcmp(actual, source + row->start, row->length) == 0);
        assert(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
               (row->kind == LINX_READER_HEADING ? &linx_font_16_bold : &linx_font_16));
        if (row->first_list_line && source[row->marker_start] != '-' && source[row->marker_start] != '*') {
            label = label_at(lv_screen_active(), 16, 76 + row->y, row->x, 18);
            assert(label && lv_obj_get_style_text_font(label, LV_PART_MAIN) == &linx_font_12);
            actual = lv_label_get_text(label);
            assert(strlen(actual) == row->marker_length &&
                   memcmp(actual, source + row->marker_start, row->marker_length) == 0);
            assert(measure(actual, strlen(actual), (void *)&linx_font_12) <= row->x);
            assert(reader_measure(actual, strlen(actual), LINX_READER_BODY, NULL) + 2 <= row->x);
        }
    }
}

static void test_template_reader_layouts(void)
{
    for (unsigned id = 0U; id < LINX_CONTENT_COUNT; id++) {
        const linx_content_t *content = linx_content_get((linx_content_id_t)id);
        assert(content != NULL);
        check_reader_layout(content->body);
        check_reader_layout(content->summary);
        check_reader_layout(content->question);
    }
    const char *numbered = "本轮修改\n12) 空密码提示改为：请输入密码。\n"
                           "1234. 令牌过期提示改为：登录已过期，请重新登录。";
    linx_reader_layout_t layout = check_reader_layout(numbered);
    size_t numeric_markers = 0U;
    size_t numeric_continuations = 0U;
    for (size_t p = 0U; p < layout.count; p++) {
        linx_reader_page_t page;
        assert(linx_reader_get_page(&layout, p, numbered, reader_measure, NULL, &page));
        for (size_t r = 0U; r < page.count; r++) {
            const linx_reader_row_t *row = &page.rows[r];
            if (row->kind != LINX_READER_LIST) continue;
            assert(row->x > LINX_READER_LIST_INDENT);
            if (row->first_list_line) {
                assert(row->marker_length == 3U || row->marker_length == 5U);
                numeric_markers++;
            } else numeric_continuations++;
        }
    }
    assert(numeric_markers == 2U && numeric_continuations >= 2U);
    const char *dense = "12) 请输入密码。\n13) 请输入密码。\n14) 请输入密码。\n"
                        "15) 请输入密码。\n16) 请输入密码。\n17) 请输入密码。\n"
                        "18) 请输入密码。\n19) 请输入密码。\n20) 请输入密码。";
    layout = check_reader_layout(dense);
    linx_reader_page_t page;
    assert(layout.count == 1U && linx_reader_get_page(&layout, 0U, dense, reader_measure, NULL, &page));
    assert(page.count == 9U);
    for (size_t r = 0U; r < page.count; r++) {
        assert(page.rows[r].first_list_line && page.rows[r].marker_length == 3U);
        assert(page.rows[r].y == r * LINX_READER_LINE_HEIGHT);
    }
    puts("Structured reader: all7 templates including LOGIN_FOLLOWUP, actual regular/bold fonts,208x200 viewport,22px rows; UTF-8/source coverage, punctuation, heading/list constraints PASS");
    puts("Numeric marker regression: 12)/1234. actual12px marker width,16px measured reserve, matching continuation indents PASS");
    puts("Dense actual-font layout: 9numbered rows,198px high,18content/marker labels before UI chrome PASS");
}

static void test_full_reader(void)
{
    enter_task(LINX_TASK_DESIGN);
    capture("02-design-overview");
    const linx_turn_t *initial = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN);
    uint32_t turn_id = initial->id;
    const char *source = linx_content_get(initial->content_id)->body;
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_DESIGN));
    overview_action(0U);
    assert(linx_ui_page() == LINX_UI_READER);
    assert(!linx_demo_has_unread(linx_ui_demo(), LINX_TASK_DESIGN));
    linx_reader_layout_t pages = check_reader_layout(source);
    assert(pages.count > 8U);
    assert(linx_ui_debug().read_page_count == pages.count);
    for (size_t page = 0U; page < pages.count; ++page) {
        assert(linx_ui_debug().read_page == page && linx_ui_debug().turn_id == turn_id);
        check_visible_reader(&pages, page, source);
        assert(pages.offsets[page + 1U] > pages.offsets[page]);
        if (page == 0U) capture("03-reply-first");
        if (page == pages.count / 2U) capture("04-reply-middle");
        if (page + 1U == pages.count) capture("05-reply-last");
        key(LINX_UI_KEY_DOWN);
    }
    assert(linx_ui_debug().read_page == pages.count - 1U);
    key(LINX_UI_KEY_OK_LONG);
    overview_action(0U);
    assert(linx_ui_debug().read_page == pages.count - 1U);
    key(LINX_UI_KEY_OK_LONG);
    overview_action(2U);
    capture("06-history");
    select_item(2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER);
    capture("07-user-message");
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_HISTORY);
    printf("Complete reply: %zu pages, %zu original UTF-8 bytes, all visible structured rows/coordinates/fonts matched; %zu actual-font lines satisfy width and CJK punctuation rules\n", pages.count, strlen(source), s_reader_line_checks);
}

static void test_login_list_reader(void)
{
    reset_scene();
    enter_task(LINX_TASK_LOGIN);
    advance(8000U);
    const linx_turn_t *initial = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_LOGIN);
    assert(initial && initial->complete && initial->content_id == LINX_CONTENT_LOGIN_INITIAL);
    start_voice_from_overview();
    uint32_t command_id = finish_and_send();
    advance(5000U);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_LOGIN, command_id)->state == LINX_COMMAND_FINISHED);
    const linx_turn_t *reply = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_LOGIN);
    assert(reply && reply->complete && reply->content_id == LINX_CONTENT_LOGIN_FOLLOWUP);
    const char *source = linx_content_get(reply->content_id)->body;
    uint32_t turn_id = reply->id;
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW);
    overview_action(0U);
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == turn_id);
    linx_reader_layout_t pages = check_reader_layout(source);
    assert(linx_ui_debug().read_page_count == pages.count);
    bool captured = false;
    size_t markers = 0U;
    for (size_t p = 0U; p < pages.count; p++) {
        assert(linx_ui_debug().read_page == p && linx_ui_debug().turn_id == turn_id);
        check_visible_reader(&pages, p, source);
        linx_reader_page_t page;
        assert(linx_reader_get_page(&pages, p, source, reader_measure, NULL, &page));
        for (size_t r = 0U; r < page.count; r++) {
            if (page.rows[r].first_list_line) {
                markers++;
                if (!captured) {
                    capture("29-login-list");
                    captured = true;
                }
            }
        }
        key(LINX_UI_KEY_DOWN);
    }
    assert(captured && markers == 2U);
    to_tasks();
    puts("Login list regression: actual initial-complete → voice → confirm → reply flow; all rows matched and screenshot29-login-list captured; no marker-only row PASS");
}

static void open_second_image(bool detail)
{
    enter_task(LINX_TASK_DESIGN);
    overview_action(3U);
    assert(linx_ui_page() == LINX_UI_IMAGES);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().image_id == LINX_IMAGE_TASK_LIST);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().image_id == LINX_IMAGE_RESULT);
    if (detail) {
        key(LINX_UI_KEY_OK);
        assert(linx_ui_page() == LINX_UI_IMAGE_MENU && linx_ui_debug().selected == 1U);
        select_item(0U);
        key(LINX_UI_KEY_OK);
        assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL && linx_ui_debug().image_view == 1U);
    }
}

static lv_obj_t *find_visible_box(lv_obj_t *object, bool selected)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (!lv_obj_check_type(object, &lv_label_class) && !lv_obj_check_type(object, &lv_image_class)) {
        if (selected && lv_color_eq(lv_obj_get_style_bg_color(object, LV_PART_MAIN), lv_color_hex(0xF8E0CFU)))
            return object;
        if (!selected && lv_obj_get_x(object) == 15 && lv_obj_get_y(object) == 99 &&
            lv_obj_get_width(object) == 210 && lv_obj_get_height(object) == 170)
            return object;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_visible_box(lv_obj_get_child(object, (int32_t)i), selected);
        if (found) return found;
    }
    return NULL;
}

static void test_box_style_reset(void)
{
    reset_scene();
    open_second_image(true);
    lv_obj_t *viewport = find_visible_box(lv_screen_active(), false);
    assert(viewport != NULL && lv_obj_get_style_border_width(viewport, LV_PART_MAIN) == 1);
    to_tasks();
    lv_obj_t *selected = find_visible_box(lv_screen_active(), true);
    assert(selected != NULL);
    int border = lv_obj_get_style_border_width(selected, LV_PART_MAIN);
    if (border != 0) {
        fprintf(stderr, "Selected task row retained image-detail border: width=%d\n", border);
        abort();
    }
    puts("Reused box regression: image detail border does not leak into selected task row PASS");
}

static void test_picture_failure_recovery(void)
{
    reset_scene();
    settings_toggle(2U);
    assert(linx_ui_demo()->fail_next_send);
    enter_task(LINX_TASK_DESIGN);
    overview_action(3U);
    capture("08-image-task-list");
    key(LINX_UI_KEY_DOWN);
    capture("09-image-result");
    uint32_t image_turn = linx_ui_debug().turn_id;
    key(LINX_UI_KEY_DOWN);
    capture("10-image-voice");
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_OK);
    capture("11-image-menu");
    select_item(0U);
    key(LINX_UI_KEY_OK);
    capture("12-image-upper");
    key(LINX_UI_KEY_DOWN);
    capture("13-image-lower");
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_VOICE);
    assert(linx_ui_debug().image_id == LINX_IMAGE_RESULT && linx_ui_debug().turn_id == image_turn);
    advance(125U);
    capture("14-voice");
    advance(7875U);
    assert(linx_ui_page() == LINX_UI_CONFIRM);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 1U);
    capture("15-confirm");
    select_item(1U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_VOICE && linx_ui_debug().image_id == LINX_IMAGE_RESULT);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL && linx_ui_debug().image_view == 2U);
    key(LINX_UI_KEY_OK);
    uint32_t id = finish_and_send();
    capture("16-sending");
    advance(500U);
    const linx_command_t *command = linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, id);
    assert(command && command->state == LINX_COMMAND_FAILED);
    assert(command->context.image_id == LINX_IMAGE_RESULT && command->context.source_turn_id == image_turn);
    capture("17-failed");
    key(LINX_UI_KEY_OK_LONG);
    settings_toggle(1U);
    assert(!linx_ui_demo()->online);
    capture("18-offline-snapshot");
    open_second_image(false);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_RECEIPT && linx_ui_debug().command_id == id);
    key(LINX_UI_KEY_OK);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, id)->state == LINX_COMMAND_FAILED);
    settings_toggle(1U);
    assert(linx_ui_demo()->online);
    advance(1000U);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, id)->state == LINX_COMMAND_FAILED);
    enter_task(LINX_TASK_DESIGN);
    overview_action(1U);
    assert(linx_ui_page() == LINX_UI_RECEIPT && linx_ui_debug().command_id == id);
    key(LINX_UI_KEY_OK);
    advance(500U);
    capture("19-computer-received");
    advance(500U);
    capture("20-agent-accepted");
    advance(250U);
    capture("21-agent-running");
    advance(3750U);
    capture("22-reply-received");
    command = linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, id);
    assert(command->state == LINX_COMMAND_FINISHED && command->context.image_id == LINX_IMAGE_RESULT);
    assert(command->context.source_turn_id == image_turn);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 2U);
    const linx_turn_t *new_turn = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN);
    assert(new_turn->image_count == 0U && new_turn->unread);
    key(LINX_UI_KEY_OK);
    overview_action(3U);
    assert(linx_ui_debug().image_id == LINX_IMAGE_RESULT && linx_ui_debug().turn_id == image_turn);
    key(LINX_UI_KEY_OK_LONG);
    overview_action(2U);
    select_item(2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == image_turn);
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_DESIGN));
    key(LINX_UI_KEY_OK_LONG);
    select_item(3U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_IMAGES && linx_ui_debug().turn_id == image_turn);
    assert(linx_demo_images(linx_ui_demo(), LINX_TASK_DESIGN, new_turn->id, NULL, 0U) == 0U);
    assert(linx_demo_images(linx_ui_demo(), LINX_TASK_DESIGN, image_turn, NULL, 0U) == 3U);
    puts("Picture reply: same source/image across timeout, cancel, failure, offline restore, retry, history PASS");
}

static void expect_pending_receipt(uint32_t id)
{
    assert(linx_ui_page() == LINX_UI_RECEIPT && linx_ui_debug().command_id == id);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->pending.id == id);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW);
}

static void test_queue_gates(void)
{
    reset_scene();
    enter_task(LINX_TASK_DESIGN);
    start_voice_from_overview();
    uint32_t first = finish_and_send();
    advance(1000U);
    key(LINX_UI_KEY_OK);
    start_voice_from_overview();
    uint32_t queued = finish_and_send();
    advance(1000U);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, queued)->state == LINX_COMMAND_QUEUED);
    capture("23-queued");
    key(LINX_UI_KEY_OK);
    overview_action(1U);
    expect_pending_receipt(queued);
    overview_action(0U);
    assert(linx_ui_page() == LINX_UI_READER);
    key(LINX_UI_KEY_OK);
    expect_pending_receipt(queued);
    overview_action(3U);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_OK);
    expect_pending_receipt(queued);
    overview_action(3U);
    key(LINX_UI_KEY_OK);
    select_item(0U);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_OK);
    expect_pending_receipt(queued);
    advance(3000U);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, first)->state == LINX_COMMAND_FINISHED);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, queued)->state == LINX_COMMAND_AGENT_ACCEPTED);
    advance(4000U);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, queued)->state == LINX_COMMAND_FINISHED);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 3U);
    puts("Queue gates: overview, full reply, image overview/menu, detail all preserve one queued ID PASS");
}

static void test_background_does_not_steal_focus(void)
{
    reset_scene();
    enter_task(LINX_TASK_LOGIN);
    enter_task(LINX_TASK_DESIGN);
    overview_action(0U);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_DOWN);
    linx_ui_debug_t reading = linx_ui_debug();
    advance(8000U);
    linx_ui_debug_t after = linx_ui_debug();
    assert(after.page == LINX_UI_READER && after.task_id == reading.task_id &&
           after.turn_id == reading.turn_id && after.read_page == reading.read_page);
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_LOGIN));
    reset_scene();
    enter_task(LINX_TASK_LOGIN);
    open_second_image(true);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_CONFIRM);
    linx_ui_debug_t confirmation = linx_ui_debug();
    advance(8000U);
    after = linx_ui_debug();
    assert(after.page == LINX_UI_CONFIRM && after.task_id == confirmation.task_id &&
           after.turn_id == confirmation.turn_id && after.image_id == LINX_IMAGE_RESULT);
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_LOGIN));
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 1U);
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL && linx_ui_debug().image_id == LINX_IMAGE_RESULT);
    puts("Background completion: reader position and confirmation target remain unchanged PASS");
}

static void test_reset_and_other_screens(void)
{
    reset_scene();
    enter_task(LINX_TASK_LOGIN);
    capture("24-login-running");
    start_voice_from_overview();
    uint32_t pending = finish_and_send();
    advance(1000U);
    uint32_t scene = linx_ui_demo()->scene_id;
    reset_scene();
    assert(linx_ui_demo()->scene_id != scene);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_LOGIN, pending) == NULL);
    advance(12000U);
    assert(!linx_demo_task(linx_ui_demo(), LINX_TASK_LOGIN)->initial_started);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_LOGIN)->turn_count == 1U);
    enter_task(LINX_TASK_PAYMENT);
    capture("25-payment-overview");
    to_tasks();
    key(LINX_UI_KEY_OK_LONG);
    capture("26-settings");
    key(LINX_UI_KEY_OK);
    capture("27-reset-confirm");
    assert(linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_SETTINGS);
    select_item(3U);
    key(LINX_UI_KEY_OK);
    capture("28-about");
    to_tasks();
    puts("Reset: old scene command/timing invalidated; cancellation default, settings, about PASS");
}

static void repeat_reply(void)
{
    enter_task(LINX_TASK_DESIGN);
    start_voice_from_overview();
    finish_and_send();
    advance(5000U);
    key(LINX_UI_KEY_OK);
    overview_action(0U);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_OK_LONG);
    overview_action(2U);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_OK_LONG);
    to_tasks();
    advance(1000U);
}

static void test_repeat_memory(void)
{
    reset_scene();
    for (size_t i = 0; i < 5U; ++i) repeat_reply();
    lv_mem_monitor_t baseline;
    lv_mem_monitor(&baseline);
    size_t navigation_before = s_key_count;
    lv_mem_monitor_t final = baseline;
    for (size_t i = 0; i < 25U; ++i) {
        repeat_reply();
        if (i == 4U || i == 14U || i == 24U) {
            lv_mem_monitor(&final);
            assert(final.free_size + 2048U >= baseline.free_size);
            assert(final.free_biggest_size + 2048U >= baseline.free_biggest_size);
            printf("LVGL pool sample %zu: free=%zu largest=%zu max-used=%zu\n", i + 1U,
                   final.free_size, final.free_biggest_size, final.max_used);
        }
    }
    assert(s_key_count - navigation_before >= 100U);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == LINX_HISTORY_CAPACITY);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->history_trimmed);
    assert(lv_mem_test() == LV_RESULT_OK);
    printf("Repeat test: 5 warmups + 25 replies, %zu navigation events; free delta=%lld largest delta=%lld PASS\n",
           s_key_count - navigation_before,
           (long long)final.free_size - (long long)baseline.free_size,
           (long long)final.free_biggest_size - (long long)baseline.free_biggest_size);
    /* Fast-forward model time; this does not claim a 30-minute physical soak. */
    advance(30U * 60U * 1000U);
    assert(linx_ui_page() == LINX_UI_TASKS);
}

static void test_lifecycle_and_boot_keys(void)
{
    lv_obj_t *screen = lv_screen_active();
    linx_ui_deinit();
    size_t children = lv_obj_get_child_count(screen);
    linx_ui_deinit();
    linx_ui_init(NULL, s_now);
    linx_ui_tick(s_now);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(lv_obj_get_child_count(screen) == children);
    assert(linx_ui_debug().labels_used == 0U && linx_ui_demo()->scene_id == 0U);

    lv_obj_t *unrelated = lv_obj_create(screen);
    for (size_t scenario = 0U; scenario < 2U; scenario++) {
        linx_ui_init(screen, s_now);
        assert(lv_obj_get_child_count(screen) == children + 2U);
        assert(linx_ui_page() == LINX_UI_BOOT);
        advance(799U);
        assert(linx_ui_page() == LINX_UI_BOOT);
        key(LINX_UI_KEY_UP);
        key(LINX_UI_KEY_DOWN);
        assert(linx_ui_page() == LINX_UI_BOOT);
        key(scenario == 0U ? LINX_UI_KEY_OK : LINX_UI_KEY_OK_LONG);
        assert(linx_ui_page() == LINX_UI_TASKS && linx_ui_debug().selected == 0U);
        linx_ui_deinit();
        assert(lv_obj_get_child_count(screen) == children + 1U);
        assert(lv_obj_is_valid(unrelated));
    }
    lv_obj_delete(unrelated);
    linx_ui_init(screen, s_now);
    advance(800U);
    assert(linx_ui_page() == LINX_UI_TASKS);
    puts("生命周期：空初始化、重复清理保留调用者对象；启动等待及短按/长按进入任务通过");
}

static void test_menu_boundaries(void)
{
    reset_scene();
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().selected == 0U);
    select_item(LINX_TASK_COUNT - 1U);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == LINX_TASK_COUNT - 1U);
    enter_task(LINX_TASK_LOGIN);
    assert(linx_ui_debug().selected == 1U);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().selected == 1U);
    assert(visible_text_contains(lv_screen_active(), "阅读回复  暂无"));
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER);
    assert(visible_text_contains(lv_screen_active(), "你的指令"));
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().read_page == 0U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == 0U);

    to_tasks();
    key(LINX_UI_KEY_OK_LONG);
    select_item(0U);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_page() == LINX_UI_RESET && linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_SETTINGS);
    select_item(3U);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 3U);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_page() == LINX_UI_ABOUT);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_SETTINGS && linx_ui_debug().selected == 3U);
    to_tasks();
    puts("菜单边界：任务/概览/历史/用户正文/设置/恢复/关于；禁用选项不可选通过");
}

static void test_image_menu_and_filtered_navigation(void)
{
    reset_scene();
    enter_task(LINX_TASK_DESIGN);
    overview_action(2U);
    select_item(1U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_IMAGES && linx_ui_debug().image_id == LINX_IMAGE_TASK_LIST);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().image_id == LINX_IMAGE_TASK_LIST);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().image_id == LINX_IMAGE_VOICE);
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == 1U);
    key(LINX_UI_KEY_OK_LONG);
    overview_action(3U);
    assert(linx_ui_debug().image_id == LINX_IMAGE_TASK_LIST);
    key(LINX_UI_KEY_OK);
    select_item(2U);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_IMAGES && linx_ui_debug().image_id == LINX_IMAGE_TASK_LIST);
    key(LINX_UI_KEY_OK);
    select_item(0U);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().image_view == 2U);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().image_view == 1U);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().image_view == 1U);
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_IMAGES && linx_ui_debug().image_id == LINX_IMAGE_TASK_LIST);
    to_tasks();
    puts("图片导航：过滤相册不覆盖任务相册记忆，菜单返回和局部首尾切换通过");
}

static void test_explicit_cancel_and_failed_receipt(void)
{
    reset_scene();
    settings_toggle(2U);
    enter_task(LINX_TASK_PAYMENT);
    start_voice_from_overview();
    linx_ui_debug_t target = linx_ui_debug();
    key(LINX_UI_KEY_UP);
    advance(1U);
    assert(linx_ui_page() == LINX_UI_VOICE && linx_ui_debug().turn_id == target.turn_id);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().selected == 0U);
    select_item(2U);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);
    const linx_task_t *payment = linx_demo_task(linx_ui_demo(), LINX_TASK_PAYMENT);
    assert(payment->turn_count == 1U && payment->pending.id == 0U);
    assert(linx_ui_demo()->fail_next_send);
    start_voice_from_overview();
    key(LINX_UI_KEY_OK_LONG);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && payment->pending.id == 0U);
    start_voice_from_overview();
    uint32_t command = finish_and_send();
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_page() == LINX_UI_RECEIPT && linx_ui_debug().selected == 0U);
    advance(500U);
    assert(payment->pending.id == command && payment->pending.state == LINX_COMMAND_FAILED);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 1U);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && payment->pending.id == 0U);
    assert(payment->turn_count == 1U && linx_demo_command(linx_ui_demo(), LINX_TASK_PAYMENT, command) == NULL);
    assert(linx_demo_reply_gate(linx_ui_demo(), LINX_TASK_PAYMENT) == LINX_REPLY_ALLOWED);
    start_voice_from_overview();
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && payment->pending.id == 0U);
    to_tasks();
    puts("取消流程：确认取消不发送，失败回执取消释放名额，回执方向键边界通过");
}

static void test_payment_numeric_reply(void)
{
    reset_scene();
    enter_task(LINX_TASK_PAYMENT);
    overview_action(0U);
    uint32_t original_turn = linx_ui_debug().turn_id;
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_VOICE && linx_ui_debug().turn_id == original_turn);
    uint32_t command = finish_and_send();
    advance(5000U);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_PAYMENT, command)->state == LINX_COMMAND_FINISHED);
    key(LINX_UI_KEY_OK);
    overview_action(0U);
    const linx_turn_t *turn = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_PAYMENT);
    assert(turn->content_id == LINX_CONTENT_PAYMENT_FOLLOWUP && !turn->unread);
    const char *source = linx_content_get(turn->content_id)->body;
    linx_reader_layout_t layout = check_reader_layout(source);
    size_t markers = 0U;
    for (size_t p = 0U; p < layout.count; p++) {
        assert(linx_ui_debug().read_page == p && linx_ui_debug().turn_id == turn->id);
        check_visible_reader(&layout, p, source);
        linx_reader_page_t page;
        assert(linx_reader_get_page(&layout, p, source, reader_measure, NULL, &page));
        for (size_t r = 0U; r < page.count; r++) {
            const linx_reader_row_t *row = &page.rows[r];
            if (row->first_list_line) {
                assert(row->marker_length == 2U);
                lv_obj_t *label = label_at(lv_screen_active(), 16, 74 + row->y + 2, row->x, 18);
                assert(label && lv_obj_get_style_text_font(label, LV_PART_MAIN) == &linx_font_12);
                assert(strlen(lv_label_get_text(label)) == row->marker_length);
                assert(memcmp(lv_label_get_text(label), source + row->marker_start, row->marker_length) == 0);
                markers++;
            }
        }
        key(LINX_UI_KEY_DOWN);
    }
    assert(markers == 3U && linx_ui_debug().read_page == layout.count - 1U);
    for (size_t p = layout.count - 1U; p > 0U; p--) key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().read_page == 0U);
    to_tasks();
    puts("支付回复：正文发起回复绑定原轮次，三条数字列表实际字形/位置与首尾分页通过");
}

static void test_incomplete_history_user(void)
{
    reset_scene();
    enter_task(LINX_TASK_DESIGN);
    start_voice_from_overview();
    uint32_t command = finish_and_send();
    advance(1000U);
    const linx_turn_t *running = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN);
    assert(!running->complete && running->command.id == command);
    uint32_t turn_id = running->id;
    key(LINX_UI_KEY_OK);
    overview_action(2U);
    assert(linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == turn_id);
    assert(visible_text_contains(lv_screen_active(), "你的指令"));
    advance(4000U);
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == turn_id);
    assert(visible_text_contains(lv_screen_active(), "你的指令"));
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_DESIGN));
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == 1U);
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == turn_id);
    assert(!linx_demo_turn(linx_ui_demo(), LINX_TASK_DESIGN, turn_id)->unread);
    assert(linx_demo_history(linx_ui_demo(), LINX_TASK_DESIGN, 1U)->unread);
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_DESIGN));
    to_tasks();
    puts("运行中历史：可读已接收用户指令；完成后保持阅读目标，返回仍选原条目且回复未读通过");
}

static void test_background_completion_during_voice(void)
{
    reset_scene();
    enter_task(LINX_TASK_LOGIN);
    advance(1000U);
    open_second_image(true);
    key(LINX_UI_KEY_OK);
    linx_ui_debug_t target = linx_ui_debug();
    assert(target.page == LINX_UI_VOICE && target.image_id == LINX_IMAGE_RESULT);
    advance(124U);
    assert(visible_text_contains(lv_screen_active(), "00:00"));
    advance(1U);
    advance(6875U);
    linx_ui_debug_t after = linx_ui_debug();
    assert(after.page == LINX_UI_VOICE && after.task_id == target.task_id &&
           after.turn_id == target.turn_id && after.image_id == target.image_id);
    assert(visible_text_contains(lv_screen_active(), "00:07"));
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_LOGIN));
    advance(999U);
    assert(linx_ui_page() == LINX_UI_VOICE);
    advance(1U);
    assert(linx_ui_page() == LINX_UI_CONFIRM && linx_ui_debug().image_id == target.image_id);
    select_item(2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL && linx_ui_debug().image_view == 1U);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->pending.id == 0U);
    to_tasks();
    puts("录入期间后台完成：保留图片目标及七秒计时，八秒确认后取消不发送通过");
}

static void test_background_history_eviction(void)
{
    for (size_t scenario = 0U; scenario < 4U; scenario++) {
        reset_scene();
        for (size_t turn = 0U; turn < 6U; turn++) repeat_reply();
        const linx_task_t *design = linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN);
        assert(design->turn_count == LINX_HISTORY_CAPACITY - 1U);
        uint32_t oldest = linx_demo_history(linx_ui_demo(), LINX_TASK_DESIGN, 6U)->id;
        enter_task(LINX_TASK_DESIGN);
        start_voice_from_overview();
        uint32_t first = finish_and_send();
        advance(1000U);
        key(LINX_UI_KEY_OK);
        start_voice_from_overview();
        uint32_t queued = finish_and_send();
        advance(1000U);
        assert(design->turn_count == LINX_HISTORY_CAPACITY);
        assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, queued)->state == LINX_COMMAND_QUEUED);
        key(LINX_UI_KEY_OK);
        if (scenario < 2U) {
            overview_action(3U);
            assert(linx_ui_debug().turn_id == oldest && linx_ui_debug().image_id == LINX_IMAGE_TASK_LIST);
            if (scenario == 1U) key(LINX_UI_KEY_OK);
        } else {
            overview_action(2U);
            /* 运行中的用户消息、六组已完成对话，随后是初始的三条记录。 */
            select_item(scenario == 2U ? 13U : 15U);
            if (scenario == 2U) {
                key(LINX_UI_KEY_OK);
                assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == oldest);
                size_t count = linx_ui_debug().read_page_count;
                for (size_t page = 1U; page < count; page++) key(LINX_UI_KEY_DOWN);
                assert(linx_ui_debug().read_page > 0U);
            }
        }
        advance(3000U);
        assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, first)->state == LINX_COMMAND_FINISHED);
        assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, queued)->state == LINX_COMMAND_AGENT_ACCEPTED);
        assert(linx_demo_turn(linx_ui_demo(), LINX_TASK_DESIGN, oldest) == NULL);
        assert(linx_demo_images(linx_ui_demo(), LINX_TASK_DESIGN, 0U, NULL, 0U) == 0U);
        if (scenario == 0U) {
            assert(linx_ui_page() == LINX_UI_IMAGES && linx_ui_debug().image_id == 0U);
            assert(visible_text_contains(lv_screen_active(), "图片暂不可用"));
            key(LINX_UI_KEY_UP);
            key(LINX_UI_KEY_DOWN);
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_IMAGES && linx_ui_debug().image_id == 0U);
            key(LINX_UI_KEY_OK_LONG);
            assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 3U);
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_OVERVIEW);
            key(LINX_UI_KEY_UP);
            assert(linx_ui_debug().selected == 2U);
        } else if (scenario == 1U) {
            assert(linx_ui_page() == LINX_UI_IMAGE_MENU && linx_ui_debug().image_id == 0U);
            assert(visible_text_contains(lv_screen_active(), "图片暂不可用"));
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_IMAGE_MENU);
            select_item(0U);
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL && linx_ui_debug().image_id == 0U);
            assert(visible_text_contains(lv_screen_active(), "图片暂不可用"));
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL);
            key(LINX_UI_KEY_OK_LONG);
            assert(linx_ui_page() == LINX_UI_IMAGES);
        } else if (scenario == 2U) {
            assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == oldest);
            assert(linx_ui_debug().read_page == 0U && linx_ui_debug().read_page_count == 1U);
            assert(visible_text_contains(lv_screen_active(), "这轮记录已移出保留范围"));
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_READER && design->pending.id == 0U);
            assert(design->turn_count == LINX_HISTORY_CAPACITY);
        } else {
            assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == 0U);
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_READER && visible_text_contains(lv_screen_active(), "你的指令"));
            assert(linx_ui_debug().turn_id == linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN)->id);
        }
        to_tasks();
    }
    puts("后台历史淘汰：图片/菜单/局部降级，旧正文页码回落且禁止失效回复，历史失效选项回退通过");
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    bool missing_image = argc == 3 && strcmp(argv[2], "--missing-image") == 0;
    assert(argc == 2 || missing_image);
    s_screens = argv[1];
    assert(LV_MEM_SIZE == 40U * 1024U);
    lv_init();
    check_inventory();
    test_template_reader_layouts();
    s_display = lv_display_create(WIDTH, HEIGHT);
    assert(s_display != NULL);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL, sizeof(s_draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush);
    linx_ui_init(lv_screen_active(), s_now);
    if (!missing_image) capture("00-boot");
    advance(800U);
    assert(linx_ui_page() == LINX_UI_TASKS);
    if (missing_image) {
        test_missing_image();
        linx_ui_deinit();
        lv_display_delete(s_display);
        lv_deinit();
        printf("Missing-image real LVGL host PASS: %zu screenshots, %zu actual-label audits\n", s_captures, s_label_checks);
        return 0;
    }
    capture("01-tasks");
    test_timeout_release_barriers();
    test_box_style_reset();
    reset_scene();
    test_full_reader();
    test_login_list_reader();
    test_picture_failure_recovery();
    test_queue_gates();
    test_background_does_not_steal_focus();
    test_reset_and_other_screens();
    test_repeat_memory();
    test_lifecycle_and_boot_keys();
    test_menu_boundaries();
    test_image_menu_and_filtered_navigation();
    test_explicit_cancel_and_failed_receipt();
    test_payment_numeric_reply();
    test_incomplete_history_user();
    test_background_completion_during_voice();
    test_background_history_eviction();
    linx_ui_deinit();
    lv_display_delete(s_display);
    lv_deinit();
    printf("Real LVGL host PASS: %zu screenshots, %zu key events, %zu actual-label audits\n",
           s_captures, s_key_count, s_label_checks);
    return 0;
}
