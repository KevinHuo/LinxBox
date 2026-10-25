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
               font == &linx_font_20_bold || font == &linx_font_24_bold || font == &linx_font_28_bold ||
               font == &linx_font_remote_16);
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
    for (size_t i = 0; linx_ui_page() != LINX_UI_TASKS && i < 16U; ++i) key(LINX_UI_KEY_BACK);
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
    key(LINX_UI_KEY_BACK);
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
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_SETTINGS);
    select_item(index);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_BACK);
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

/* Mirrors main.c: reduce queued event, tick the UI, invalidate automatic
 * transitions, then resolve a pending single click only once the queue is idle.
 * Dispatch an action and invalidate any manual page transition afterward. */
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
    } else if (!has_event) action = linx_input_tick(input, now_ms);
    if (action != LINX_INPUT_NONE) {
        linx_ui_page_t previous = linx_ui_page();
        linx_ui_key_t mapped = action == LINX_INPUT_UP ? LINX_UI_KEY_UP :
                               action == LINX_INPUT_DOWN ? LINX_UI_KEY_DOWN :
                               action == LINX_INPUT_BACK ? LINX_UI_KEY_BACK : LINX_UI_KEY_OK;
        linx_ui_handle_key(mapped);
        ++s_key_count;
        if (linx_ui_page() != previous) linx_input_barrier(input, now_ms);
    }
    settle();
}

static void test_timeout_release_barriers(void)
{
    for (size_t example = 0U; example < 4U; ++example) {
        linx_back_mode_t mode = example < 2U ? LINX_BACK_LONG_PRESS : LINX_BACK_DOUBLE_CLICK;
        size_t scenario = example % 2U;
        reset_scene();
        linx_ui_set_back_mode(mode);
        enter_task(LINX_TASK_DESIGN);
        start_voice_from_overview();
        linx_input_t input;
        linx_input_init(&input, 300U);
        uint64_t start = s_now;
        linx_input_set_back_mode(&input, linx_ui_back_mode(), start);
        pump(&input, true, LINX_INPUT_PRESS, start + 7990U, start + 7990U);
        assert(linx_ui_page() == LINX_UI_VOICE);
        if (scenario == 0U) {
            /* A release on the timeout loop must not leave a pending confirmation. */
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
        assert(linx_ui_page() == (mode == LINX_BACK_LONG_PRESS ? LINX_UI_RECEIPT : LINX_UI_CONFIRM));
        pump(&input, false, LINX_INPUT_RELEASE, 0U, fresh + 341U);
        assert(linx_ui_page() == LINX_UI_RECEIPT);
        uint32_t id = linx_ui_debug().command_id;
        assert(id != 0U && linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->pending.id == id);
        /* The release cannot run a second time through the new receipt page. */
        pump(&input, true, LINX_INPUT_RELEASE, fresh + 40U, s_now);
        assert(linx_ui_page() == LINX_UI_RECEIPT && linx_ui_debug().command_id == id);
    }
    linx_ui_set_back_mode(LINX_BACK_LONG_PRESS);
    puts("Physical input regression: simultaneous timeout/release and release after timeout require a fresh gesture PASS");
}

/* 使用真实归约器验证返回手势不会附带确认、发送或结束上传。 */
static void back_gesture(void)
{
    linx_input_t input;
    linx_input_init(&input, 300U);
    uint64_t start = s_now;
    linx_ui_page_t original = linx_ui_page();
    linx_input_set_back_mode(&input, linx_ui_back_mode(), start);
    pump(&input, true, LINX_INPUT_PRESS, start + 10U, start + 10U);
    if (linx_ui_back_mode() == LINX_BACK_LONG_PRESS) {
        pump(&input, true, LINX_INPUT_LONG, start + 510U, start + 510U);
        linx_ui_page_t returned = linx_ui_page();
        pump(&input, true, LINX_INPUT_RELEASE, start + 550U, start + 550U);
        pump(&input, false, LINX_INPUT_RELEASE, 0U, start + 900U);
        assert(linx_ui_page() == returned);
        return;
    }
    pump(&input, true, LINX_INPUT_RELEASE, start + 50U, start + 50U);
    assert(linx_ui_page() == original);
    pump(&input, true, LINX_INPUT_PRESS, start + 100U, start + 100U);
    assert(linx_ui_page() == original);
    pump(&input, true, LINX_INPUT_RELEASE, start + 140U, start + 140U);
    linx_ui_page_t returned = linx_ui_page();
    pump(&input, false, LINX_INPUT_RELEASE, 0U, start + 500U);
    assert(linx_ui_page() == returned);
}

static void test_configured_back_cancels_demo_voice(void)
{
    for (unsigned mode = LINX_BACK_LONG_PRESS; mode <= LINX_BACK_DOUBLE_CLICK; ++mode) {
        reset_scene();
        linx_ui_set_back_mode((linx_back_mode_t)mode);
        assert(visible_text_contains(lv_screen_active(), mode == LINX_BACK_LONG_PRESS ? "长按选项" : "双击选项"));
        back_gesture();
        assert(linx_ui_page() == LINX_UI_SETTINGS);
        back_gesture();
        assert(linx_ui_page() == LINX_UI_TASKS);
        enter_task(LINX_TASK_DESIGN);
        start_voice_from_overview();
        linx_demo_t before = *linx_ui_demo();
        back_gesture();
        assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);
        linx_demo_tick(&before, s_now);
        assert(memcmp(&before, linx_ui_demo(), sizeof(before)) == 0);
    }
    linx_ui_set_back_mode(LINX_BACK_LONG_PRESS);
    puts("返回手势：默认长按或配置双击均可返回，长按松手/双击第一击不确认或发送，语音直接取消通过");
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

static void check_remote_readability(void)
{
    /* 在线字体须与已验证的 UI 粗体同源，避免退回细体或二值字形。 */
    const char *sample = "我的任务继续沟通确认指令完整消息连接状态未知校验边界";
    const lv_font_fmt_txt_dsc_t *remote = linx_font_remote_16.dsc;
    const lv_font_fmt_txt_dsc_t *bold = linx_font_16_bold.dsc;
    const lv_font_fmt_txt_dsc_t *regular = linx_font_16.dsc;
    assert(remote->bpp == 4U && linx_font_remote_16.line_height == 20);
    size_t offset = 0U, bytes = strlen(sample);
    unsigned remote_ink = 0U, regular_ink = 0U, shades = 0U;
    while (offset < bytes) {
        uint32_t cp = decode(sample, bytes, &offset);
        lv_font_glyph_dsc_t actual = {0}, expected = {0}, thin = {0};
        assert(lv_font_get_glyph_dsc(&linx_font_remote_16, &actual, cp, 0U));
        assert(lv_font_get_glyph_dsc(&linx_font_16_bold, &expected, cp, 0U));
        assert(lv_font_get_glyph_dsc(&linx_font_16, &thin, cp, 0U));
        assert(actual.format == LV_FONT_GLYPH_FORMAT_A4 && !actual.is_placeholder);
        assert(actual.box_w == expected.box_w && actual.box_h == expected.box_h &&
               actual.adv_w == expected.adv_w && actual.ofs_x == expected.ofs_x && actual.ofs_y == expected.ofs_y);
        const uint8_t *pixels = remote->glyph_bitmap + remote->glyph_dsc[actual.gid.index].bitmap_index;
        const uint8_t *expected_pixels = bold->glyph_bitmap + bold->glyph_dsc[expected.gid.index].bitmap_index;
        assert(memcmp(pixels, expected_pixels, ((size_t)actual.box_w * actual.box_h + 1U) / 2U) == 0);
        for (size_t i = 0U; i < (size_t)actual.box_w * actual.box_h; ++i) {
            unsigned alpha = (pixels[i / 2U] >> (i & 1U ? 0U : 4U)) & 15U;
            remote_ink += alpha;
            shades |= 1U << alpha;
        }
        pixels = regular->glyph_bitmap + regular->glyph_dsc[thin.gid.index].bitmap_index;
        for (size_t i = 0U; i < (size_t)thin.box_w * thin.box_h; ++i)
            regular_ink += (pixels[i / 2U] >> (i & 1U ? 0U : 4U)) & 15U;
    }
    assert(remote_ink > regular_ink * 13U / 10U);
    assert((shades & 0x7ffeU) != 0U);
    puts("在线文字：实际粗体像素匹配、灰度平滑和笔画覆盖回归通过");
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
    assert(pages.count > 1U);
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
    key(LINX_UI_KEY_BACK);
    overview_action(0U);
    assert(linx_ui_debug().read_page == pages.count - 1U);
    key(LINX_UI_KEY_BACK);
    overview_action(2U);
    capture("06-history");
    select_item(1U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER);
    capture("07-user-message");
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER);
    back_gesture();
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

static void test_failure_recovery(void)
{
    reset_scene();
    settings_toggle(2U);
    assert(linx_ui_demo()->fail_next_send);
    enter_task(LINX_TASK_DESIGN);
    uint32_t source_turn = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN)->id;
    start_voice_from_overview();
    advance(125U);
    capture("14-voice");
    advance(7875U);
    assert(linx_ui_page() == LINX_UI_CONFIRM);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 1U);
    capture("15-confirm");
    select_item(1U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_VOICE && linx_ui_debug().turn_id == source_turn &&
           linx_ui_debug().image_id == 0U);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);
    start_voice_from_overview();
    uint32_t id = finish_and_send();
    capture("16-sending");
    advance(500U);
    const linx_command_t *command = linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, id);
    assert(command && command->state == LINX_COMMAND_FAILED);
    assert(command->context.image_id == 0U && command->context.source_turn_id == source_turn);
    capture("17-failed");
    key(LINX_UI_KEY_BACK);
    settings_toggle(1U);
    assert(!linx_ui_demo()->online);
    capture("18-offline-snapshot");
    enter_task(LINX_TASK_DESIGN);
    overview_action(1U);
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
    assert(command->state == LINX_COMMAND_FINISHED && command->context.image_id == 0U &&
           command->context.source_turn_id == source_turn);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 2U);
    const linx_turn_t *new_turn = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN);
    assert(new_turn->unread);
    key(LINX_UI_KEY_OK);
    overview_action(2U);
    select_item(2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == source_turn);
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_DESIGN));
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().image_id == 0U);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == 2U);
    puts("概览继续沟通：超时、重新录入、取消、发送失败、离线恢复及原请求重试保留任务和来源轮次通过");
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
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().command_id == 0U);
    key(LINX_UI_KEY_BACK);
    overview_action(1U);
    expect_pending_receipt(queued);
    overview_action(2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER && visible_text_contains(lv_screen_active(), "你的指令"));
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER);
    key(LINX_UI_KEY_BACK);
    key(LINX_UI_KEY_BACK);
    overview_action(1U);
    expect_pending_receipt(queued);
    advance(3000U);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, first)->state == LINX_COMMAND_FINISHED);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, queued)->state == LINX_COMMAND_AGENT_ACCEPTED);
    advance(4000U);
    assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, queued)->state == LINX_COMMAND_FINISHED);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 3U);
    puts("队列门禁：阅读正文和历史指令不发送，返回概览继续沟通始终复用同一排队编号通过");
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
    enter_task(LINX_TASK_DESIGN);
    start_voice_from_overview();
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_CONFIRM);
    linx_ui_debug_t confirmation = linx_ui_debug();
    advance(8000U);
    after = linx_ui_debug();
    assert(after.page == LINX_UI_CONFIRM && after.task_id == confirmation.task_id &&
           after.turn_id == confirmation.turn_id && after.image_id == 0U);
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_LOGIN));
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->turn_count == 1U);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);
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
    key(LINX_UI_KEY_BACK);
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
    key(LINX_UI_KEY_BACK);
    overview_action(2U);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_BACK);
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
        key(scenario == 0U ? LINX_UI_KEY_OK : LINX_UI_KEY_BACK);
        assert(linx_ui_page() == LINX_UI_TASKS && linx_ui_debug().selected == 0U);
        linx_ui_deinit();
        assert(lv_obj_get_child_count(screen) == children + 1U);
        assert(lv_obj_is_valid(unrelated));
    }
    lv_obj_delete(unrelated);
    linx_ui_init(screen, s_now);
    advance(800U);
    assert(linx_ui_page() == LINX_UI_TASKS);
    puts("生命周期：空初始化、重复清理保留调用者对象；启动等待及短按/双击进入任务通过");
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
    assert(linx_ui_page() == LINX_UI_READER);
    back_gesture();
    assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == 0U);

    to_tasks();
    key(LINX_UI_KEY_BACK);
    select_item(0U);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_UP);
    assert(linx_ui_page() == LINX_UI_RESET && linx_ui_debug().selected == 0U);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_SETTINGS);
    select_item(4U);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 4U);
    key(LINX_UI_KEY_UP);
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

static void test_text_only_navigation(void)
{
    for (uint32_t task_id = 1U; task_id <= LINX_TASK_COUNT; ++task_id) {
        reset_scene();
        enter_task(task_id);
        advance(8000U);
        select_item(0U);
        key(LINX_UI_KEY_UP);
        assert(linx_ui_debug().selected == 0U);
        key(LINX_UI_KEY_DOWN);
        assert(linx_ui_debug().selected == 1U);
        key(LINX_UI_KEY_DOWN);
        assert(linx_ui_debug().selected == 2U);
        key(LINX_UI_KEY_DOWN);
        assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 2U);
        assert(!visible_text_contains(lv_screen_active(), "查看图片") &&
               !visible_text_contains(lv_screen_active(), "图片附件"));
        if (task_id == LINX_TASK_DESIGN)
            assert(linx_demo_latest_turn(linx_ui_demo(), task_id)->image_count == 3U);
        overview_action(2U);
        const linx_turn_t *turn = linx_demo_latest_turn(linx_ui_demo(), task_id);
        assert(turn && turn->complete && turn->user_text);
        for (size_t entry = 0U; entry < 2U; ++entry) {
            select_item(entry);
            assert(!visible_text_contains(lv_screen_active(), "查看图片") &&
                   !visible_text_contains(lv_screen_active(), "图片附件"));
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == turn->id);
            assert(visible_text_contains(lv_screen_active(), entry ? "你的指令" : "完整回复"));
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_READER);
            back_gesture();
            assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == entry);
        }
        key(LINX_UI_KEY_DOWN);
        assert(linx_ui_debug().selected == 1U);
        key(LINX_UI_KEY_UP);
        key(LINX_UI_KEY_UP);
        assert(linx_ui_debug().selected == 0U);
    }
    puts("文本 MVP 导航：全部任务概览仅三项，历史仅回复和用户指令，已有图片元数据不产生入口或附件条目通过");
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
    key(LINX_UI_KEY_BACK);
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
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_page() == LINX_UI_VOICE && payment->pending.id == 0U);
    key(LINX_UI_KEY_BACK);
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
    assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == original_turn);
    back_gesture();
    start_voice_from_overview();
    assert(linx_ui_debug().turn_id == original_turn);
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
    puts("支付回复：返回概览继续沟通绑定原轮次，三条数字列表实际字形/位置与首尾分页通过");
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
    assert(linx_ui_page() == LINX_UI_READER);
    back_gesture();
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
    enter_task(LINX_TASK_DESIGN);
    start_voice_from_overview();
    linx_ui_debug_t target = linx_ui_debug();
    assert(target.page == LINX_UI_VOICE && target.image_id == 0U);
    advance(124U);
    assert(visible_text_contains(lv_screen_active(), "00:00"));
    advance(1U);
    advance(6875U);
    linx_ui_debug_t after = linx_ui_debug();
    assert(after.page == LINX_UI_VOICE && after.task_id == target.task_id &&
           after.turn_id == target.turn_id && after.image_id == 0U);
    assert(visible_text_contains(lv_screen_active(), "00:07"));
    assert(linx_demo_has_unread(linx_ui_demo(), LINX_TASK_LOGIN));
    advance(999U);
    assert(linx_ui_page() == LINX_UI_VOICE);
    advance(1U);
    assert(linx_ui_page() == LINX_UI_CONFIRM && linx_ui_debug().turn_id == target.turn_id);
    select_item(2U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->pending.id == 0U);
    to_tasks();
    puts("录入期间后台完成：保留任务、来源轮次及七秒计时，八秒确认后取消不发送通过");
}

static void test_background_history_eviction(void)
{
    for (size_t scenario = 0U; scenario < 2U; scenario++) {
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
        overview_action(2U);
        /* 运行中用户消息、六组已完成对话，随后是初始回复和用户指令。 */
        select_item(scenario == 0U ? 13U : 14U);
        if (scenario == 0U) {
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == oldest);
            size_t count = linx_ui_debug().read_page_count;
            for (size_t page = 1U; page < count; page++) key(LINX_UI_KEY_DOWN);
            assert(linx_ui_debug().read_page > 0U);
        }
        advance(3000U);
        assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, first)->state == LINX_COMMAND_FINISHED);
        assert(linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, queued)->state == LINX_COMMAND_AGENT_ACCEPTED);
        assert(linx_demo_turn(linx_ui_demo(), LINX_TASK_DESIGN, oldest) == NULL);
        if (scenario == 0U) {
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
    puts("后台历史淘汰：旧正文页码回落且单击不发送，历史失效选项回退通过");
}

/* 真实 LVGL 页面配合只记录请求的后端，不访问网络或真实音频。 */
typedef struct {
    linx_voice_request_t requests[64];
    size_t count;
    linx_voice_request_t background_requests[128];
    size_t background_count;
    linx_voice_request_t all_requests[192];
    size_t all_count;
    bool reject;
    bool reject_prepare;
} live_voice_mock_t;

static bool live_voice_request(const linx_voice_request_t *request, void *user)
{
    live_voice_mock_t *mock = user;
    assert(mock != NULL && request != NULL && request->generation != 0U);
    bool background = request->command == LINX_VOICE_PREPARE;
    if (request->command == LINX_VOICE_CANCEL || request->command == LINX_VOICE_RELEASE) {
        for (size_t i = mock->all_count; i > 0U; --i) {
            if (mock->all_requests[i - 1U].generation != request->generation) continue;
            background = mock->all_requests[i - 1U].command == LINX_VOICE_PREPARE;
            break;
        }
    }
    assert(mock->all_count < sizeof(mock->all_requests) / sizeof(mock->all_requests[0]));
    mock->all_requests[mock->all_count++] = *request;
    if (background) {
        assert(mock->background_count < sizeof(mock->background_requests) / sizeof(mock->background_requests[0]));
        mock->background_requests[mock->background_count++] = *request;
    } else {
        assert(mock->count < sizeof(mock->requests) / sizeof(mock->requests[0]));
        mock->requests[mock->count++] = *request;
    }
    return !mock->reject && !(mock->reject_prepare && request->command == LINX_VOICE_PREPARE);
}

static void live_reset(live_voice_mock_t *mock)
{
    linx_ui_init(lv_screen_active(), s_now);
    advance(800U);
    memset(mock, 0, sizeof(*mock));
    linx_ui_set_voice_handler(live_voice_request, mock);
    settle();
    enter_task(LINX_TASK_DESIGN);
}

static void assert_live_context(const linx_reply_context_t *expected,
                                const linx_reply_context_t *actual)
{
    assert(actual->scene_id == expected->scene_id && actual->task_id == expected->task_id &&
           actual->source_turn_id == expected->source_turn_id && actual->image_id == expected->image_id);
}

static void assert_no_live_demo_send(void)
{
    assert(linx_ui_page() == LINX_UI_VOICE);
    assert(linx_ui_debug().command_id == 0U);
    for (size_t i = 0U; i < LINX_TASK_COUNT; ++i) {
        const linx_task_t *target = &linx_ui_demo()->tasks[i];
        assert(target->pending.id == 0U && target->turn_count == 1U);
    }
}

static void live_event(linx_voice_state_t state, linx_voice_error_t error, const char *code)
{
    linx_voice_event_t event = { .generation = linx_ui_debug().voice_generation,
                                 .state = state, .error = error };
    if (code) {
        assert(strlen(code) == 6U);
        memcpy(event.activation_code, code, sizeof(event.activation_code));
    }
    assert(linx_ui_voice_event(&event));
    assert(linx_ui_debug().voice_state == state && linx_ui_debug().voice_error == error);
    settle();
    assert_no_live_demo_send();
}

static void assert_rejected_voice_event(const linx_voice_event_t *event)
{
    linx_ui_debug_t before = linx_ui_debug();
    const linx_demo_t model = *linx_ui_demo();
    assert(!linx_ui_voice_event(event));
    linx_ui_debug_t after = linx_ui_debug();
    assert(after.page == before.page && after.task_id == before.task_id &&
           after.turn_id == before.turn_id && after.image_id == before.image_id &&
           after.voice_generation == before.voice_generation && after.voice_state == before.voice_state &&
           after.voice_error == before.voice_error && after.wifi_mode == before.wifi_mode &&
           after.selected == before.selected);
    assert(memcmp(&model, linx_ui_demo(), sizeof(model)) == 0);
    settle();
}

static void finish_live_reply(void)
{
    const linx_demo_t model = *linx_ui_demo();
    linx_voice_event_t finished = { .generation = linx_ui_debug().voice_generation,
                                  .state = LINX_VOICE_FINISHED };
    assert(linx_ui_voice_event(&finished));
    settle();
    assert(linx_ui_debug().voice_generation == 0U);
    assert(linx_ui_page() != LINX_UI_VOICE && linx_ui_page() != LINX_UI_CONFIRM &&
           linx_ui_page() != LINX_UI_RECEIPT);
    assert(memcmp(&model, linx_ui_demo(), sizeof(model)) == 0);
    assert_rejected_voice_event(&finished);
}

static linx_voice_request_t last_live_prepare(const live_voice_mock_t *mock)
{
    assert(mock->all_count > 0U);
    linx_voice_request_t request = mock->all_requests[mock->all_count - 1U];
    assert(request.command == LINX_VOICE_PREPARE && request.generation != 0U && request.context.image_id == 0U);
    return request;
}

typedef struct {
    linx_back_mode_t requests[8];
    size_t count;
    bool reject;
} back_mode_save_mock_t;

static bool save_back_mode(linx_back_mode_t mode, void *user)
{
    back_mode_save_mock_t *mock = user;
    assert(mock && mock->count < sizeof(mock->requests) / sizeof(mock->requests[0]));
    mock->requests[mock->count++] = mode;
    return !mock->reject;
}

static void test_back_mode_settings(void)
{
    linx_ui_init(lv_screen_active(), s_now);
    advance(800U);
    assert(linx_ui_back_mode() == LINX_BACK_LONG_PRESS &&
           visible_text_contains(lv_screen_active(), "长按选项"));
    linx_ui_set_back_mode((linx_back_mode_t)-1);
    linx_ui_set_back_mode((linx_back_mode_t)(LINX_BACK_DOUBLE_CLICK + 1));
    assert(linx_ui_back_mode() == LINX_BACK_LONG_PRESS);
    key(LINX_UI_KEY_BACK);
    select_item(4U);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 4U && visible_text_contains(lv_screen_active(), "返回方式：长按"));
    capture("60-back-mode-default");
    key(LINX_UI_KEY_OK);
    assert(linx_ui_back_mode() == LINX_BACK_LONG_PRESS &&
           visible_text_contains(lv_screen_active(), "无法保存，请重试"));

    back_mode_save_mock_t saved = {0};
    linx_ui_set_back_mode_handler(save_back_mode, &saved);
    key(LINX_UI_KEY_OK);
    assert(saved.count == 1U && saved.requests[0] == LINX_BACK_DOUBLE_CLICK &&
           linx_ui_back_mode() == LINX_BACK_LONG_PRESS &&
           visible_text_contains(lv_screen_active(), "正在保存，请稍候") &&
           visible_text_contains(lv_screen_active(), "返回方式：长按") &&
           visible_text_contains(lv_screen_active(), "长按返回"));
    capture("61-back-mode-saving");
    key(LINX_UI_KEY_OK);
    linx_ui_back_mode_result(LINX_BACK_LONG_PRESS, true);
    linx_ui_back_mode_result((linx_back_mode_t)-1, true);
    assert(saved.count == 1U && linx_ui_back_mode() == LINX_BACK_LONG_PRESS &&
           visible_text_contains(lv_screen_active(), "正在保存，请稍候"));
    linx_ui_back_mode_result(LINX_BACK_DOUBLE_CLICK, false);
    assert(linx_ui_back_mode() == LINX_BACK_LONG_PRESS &&
           visible_text_contains(lv_screen_active(), "保存失败，请重试"));
    capture("62-back-mode-failed");
    key(LINX_UI_KEY_OK);
    assert(saved.count == 2U && saved.requests[1] == LINX_BACK_DOUBLE_CLICK);
    linx_ui_back_mode_result(LINX_BACK_DOUBLE_CLICK, true);
    assert(linx_ui_back_mode() == LINX_BACK_DOUBLE_CLICK &&
           visible_text_contains(lv_screen_active(), "返回方式：双击") &&
           visible_text_contains(lv_screen_active(), "双击返回"));
    capture("63-back-mode-double");
    linx_ui_back_mode_result(LINX_BACK_LONG_PRESS, true);
    assert(linx_ui_back_mode() == LINX_BACK_DOUBLE_CLICK);
    back_gesture();
    assert(linx_ui_page() == LINX_UI_TASKS && visible_text_contains(lv_screen_active(), "双击选项"));
    reset_scene();
    assert(linx_ui_back_mode() == LINX_BACK_DOUBLE_CLICK);

    key(LINX_UI_KEY_BACK);
    select_item(4U);
    saved.reject = true;
    key(LINX_UI_KEY_OK);
    assert(saved.count == 3U && saved.requests[2] == LINX_BACK_LONG_PRESS &&
           linx_ui_back_mode() == LINX_BACK_DOUBLE_CLICK &&
           visible_text_contains(lv_screen_active(), "保存失败，请重试"));
    saved.reject = false;
    key(LINX_UI_KEY_OK);
    assert(saved.count == 4U && saved.requests[3] == LINX_BACK_LONG_PRESS);
    key(LINX_UI_KEY_BACK);
    enter_task(LINX_TASK_DESIGN);
    overview_action(0U);
    linx_ui_debug_t reader = linx_ui_debug();
    linx_demo_t model = *linx_ui_demo();
    linx_ui_back_mode_result(LINX_BACK_DOUBLE_CLICK, true);
    assert(linx_ui_back_mode() == LINX_BACK_DOUBLE_CLICK && linx_ui_page() == LINX_UI_READER);
    linx_ui_back_mode_result(LINX_BACK_LONG_PRESS, true);
    assert(linx_ui_back_mode() == LINX_BACK_LONG_PRESS && linx_ui_page() == LINX_UI_READER &&
           linx_ui_debug().turn_id == reader.turn_id && linx_ui_debug().read_page == reader.read_page &&
           memcmp(&model, linx_ui_demo(), sizeof(model)) == 0 &&
           visible_text_contains(lv_screen_active(), "长按返回"));

    live_voice_mock_t voice;
    live_reset(&voice);
    to_tasks();
    key(LINX_UI_KEY_BACK);
    select_item(5U);
    key(LINX_UI_KEY_DOWN);
    assert(linx_ui_debug().selected == 5U && visible_text_contains(lv_screen_active(), "Wi-Fi网络") &&
           visible_text_contains(lv_screen_active(), "关于 LinX 演示") &&
           visible_text_contains(lv_screen_active(), "返回方式：长按"));
    capture("64-device-back-mode");
    linx_ui_set_voice_handler(NULL, NULL);
    assert(linx_ui_page() == LINX_UI_SETTINGS && linx_ui_debug().selected == 4U &&
           visible_text_contains(lv_screen_active(), "返回方式：长按"));
    linx_ui_set_voice_handler(live_voice_request, &voice);
    select_item(5U);
    memset(&saved, 0, sizeof(saved));
    linx_ui_set_back_mode_handler(save_back_mode, &saved);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_BACK);
    size_t voice_count = voice.all_count;
    linx_ui_back_mode_result(LINX_BACK_DOUBLE_CLICK, true);
    assert(linx_ui_page() == LINX_UI_TASKS && linx_ui_back_mode() == LINX_BACK_DOUBLE_CLICK &&
           voice.all_count == voice_count && voice.count == 0U && saved.count == 1U &&
           visible_text_contains(lv_screen_active(), "双击设置"));
    linx_ui_set_voice_handler(NULL, NULL);
    linx_ui_deinit();
    linx_ui_init(lv_screen_active(), s_now);
    advance(800U);
    linx_ui_back_mode_result(LINX_BACK_DOUBLE_CLICK, true);
    assert(linx_ui_back_mode() == LINX_BACK_LONG_PRESS);
    linx_ui_set_back_mode(LINX_BACK_DOUBLE_CLICK);
    assert(linx_ui_back_mode() == LINX_BACK_DOUBLE_CLICK && visible_text_contains(lv_screen_active(), "双击选项"));
    linx_ui_set_back_mode(LINX_BACK_LONG_PRESS);
    puts("返回方式设置：默认长按、两种布局、保存中防重复、失败重试、成功才生效、错误回执拒绝、离页回执不跳转、演示重置保留及启动加载通过");
}

static void test_background_voice_preparation(void)
{
    live_voice_mock_t mock;
    live_reset(&mock);
    linx_voice_request_t initial = last_live_prepare(&mock);
    assert(mock.all_count == 1U && mock.count == 0U && linx_ui_debug().voice_generation == 0U);
    const linx_reply_context_t expected = {linx_ui_demo()->scene_id, LINX_TASK_DESIGN,
        linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN)->id, 0U};
    assert_live_context(&expected, &initial.context);
    linx_voice_event_t background = {.generation=initial.generation, .state=LINX_VOICE_READY};
    assert_rejected_voice_event(&background);
    background.state = LINX_VOICE_PREPARING;
    assert_rejected_voice_event(&background);
    background.state = LINX_VOICE_DRAINING;
    assert_rejected_voice_event(&background);
    background.state = LINX_VOICE_ERROR;
    background.error = LINX_VOICE_NETWORK_FAILED;
    assert_rejected_voice_event(&background);
    size_t count = mock.all_count;
    for (size_t i = 0U; i < 4U; ++i) {
        key(LINX_UI_KEY_DOWN);
        key(LINX_UI_KEY_UP);
        advance(250U);
    }
    assert(mock.all_count == count && linx_ui_page() == LINX_UI_OVERVIEW);

    overview_action(0U);
    count = mock.all_count;
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_DOWN);
    advance(500U);
    assert(linx_ui_page() == LINX_UI_READER && mock.all_count == count && mock.count == 0U);
    back_gesture();
    linx_voice_request_t reread = last_live_prepare(&mock);
    assert(mock.all_count == count + 1U && reread.generation != initial.generation);
    assert_live_context(&initial.context, &reread.context);
    overview_action(2U);
    count = mock.all_count;
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER);
    key(LINX_UI_KEY_OK);
    back_gesture();
    assert(linx_ui_page() == LINX_UI_HISTORY && mock.all_count == count && mock.count == 0U);
    back_gesture();
    linx_voice_request_t history = last_live_prepare(&mock);
    assert(mock.all_count == count + 1U && history.generation != reread.generation);
    assert_live_context(&initial.context, &history.context);

    start_voice_from_overview();
    assert(mock.count == 1U && mock.requests[0].command == LINX_VOICE_START &&
           mock.requests[0].generation != history.generation && mock.all_count == count + 2U);
    background.generation = history.generation;
    assert_rejected_voice_event(&background);
    background.state = LINX_VOICE_READY;
    background.error = LINX_VOICE_OK;
    assert_rejected_voice_event(&background);
    background.state = LINX_VOICE_PREPARING;
    assert_rejected_voice_event(&background);
    background.generation = mock.requests[0].generation;
    background.state = LINX_VOICE_READY;
    assert_rejected_voice_event(&background);
    back_gesture();
    assert(mock.count == 2U && mock.requests[1].command == LINX_VOICE_CANCEL &&
           mock.requests[1].generation == mock.requests[0].generation);
    linx_voice_request_t returned = last_live_prepare(&mock);
    assert(returned.generation != mock.requests[0].generation && returned.generation != history.generation);
    assert_live_context(&initial.context, &returned.context);
    count = mock.all_count;
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_TASKS && mock.all_count == count && mock.count == 2U);
    advance(1000U);
    assert(mock.all_count == count);
    enter_task(LINX_TASK_DESIGN);
    linx_voice_request_t reentered = last_live_prepare(&mock);
    assert(mock.all_count == count + 1U && reentered.generation != returned.generation && mock.count == 2U);
    assert_live_context(&returned.context, &reentered.context);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_TASKS && mock.all_count == count + 1U);
    enter_task(LINX_TASK_PAYMENT);
    linx_voice_request_t switched = last_live_prepare(&mock);
    assert(mock.all_count == count + 2U && switched.context.task_id == LINX_TASK_PAYMENT &&
           switched.generation != reentered.generation && mock.count == 2U);
    for (size_t i = count; i < mock.all_count; ++i)
        assert(mock.all_requests[i].command == LINX_VOICE_PREPARE);
    linx_ui_set_voice_handler(NULL, NULL);
    puts("语音后台准备：概览准备且不录音、不重复投递，阅读/历史/普通返回保留连接，同任务重进和换目标只更新准备代号，旧准备事件不污染录音通过");
}

static void test_background_voice_context_refresh(void)
{
    live_voice_mock_t mock;
    live_reset(&mock);
    linx_ui_set_voice_handler(NULL, NULL);
    start_voice_from_overview();
    uint32_t command = finish_and_send();
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW);
    memset(&mock, 0, sizeof(mock));
    linx_ui_set_voice_handler(live_voice_request, &mock);
    linx_voice_request_t original = last_live_prepare(&mock);
    advance(1000U);
    const linx_turn_t *latest = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN);
    assert(latest->id != original.context.source_turn_id &&
           linx_demo_command(linx_ui_demo(), LINX_TASK_DESIGN, command)->state >= LINX_COMMAND_AGENT_ACCEPTED);
    linx_voice_request_t refreshed = last_live_prepare(&mock);
    assert(refreshed.generation != original.generation && refreshed.context.source_turn_id == latest->id &&
           refreshed.context.scene_id == original.context.scene_id &&
           refreshed.context.task_id == original.context.task_id && mock.count == 0U);
    size_t count = mock.all_count;
    advance(8000U);
    assert(mock.all_count == count && linx_ui_page() == LINX_UI_OVERVIEW);
    linx_ui_set_voice_handler(NULL, NULL);
    puts("语音准备目标：概览后台收到新轮次时重新准备，原轮次完成和重复刷新不重复连接通过");
}

static void test_background_voice_owner_lifecycle(void)
{
    live_voice_mock_t mock;
    live_reset(&mock);
    linx_voice_request_t original = last_live_prepare(&mock);
    live_voice_mock_t replacement = {0};
    linx_ui_set_voice_handler(live_voice_request, &replacement);
    assert(mock.count == 0U && mock.all_count == 2U && mock.all_requests[1].command == LINX_VOICE_RELEASE &&
           mock.all_requests[1].generation == original.generation);
    linx_voice_request_t current = last_live_prepare(&replacement);
    assert(replacement.all_count == 1U && replacement.count == 0U && current.generation != original.generation);
    linx_ui_deinit();
    assert(replacement.all_count == 2U && replacement.all_requests[1].command == LINX_VOICE_RELEASE &&
           replacement.all_requests[1].generation == current.generation && replacement.count == 0U);
    linx_ui_deinit();
    assert(replacement.all_count == 2U);
    linx_ui_init(lv_screen_active(), s_now);
    advance(800U);

    live_reset(&mock);
    original = last_live_prepare(&mock);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_TASKS && mock.all_count == 1U);
    memset(&replacement, 0, sizeof(replacement));
    linx_ui_set_voice_handler(live_voice_request, &replacement);
    assert(mock.all_count == 2U && mock.all_requests[1].command == LINX_VOICE_RELEASE &&
           mock.all_requests[1].generation == original.generation && replacement.all_count == 0U);
    linx_ui_set_voice_handler(NULL, NULL);

    live_reset(&mock);
    original = last_live_prepare(&mock);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_TASKS && mock.all_count == 1U);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_SETTINGS && mock.all_count == 1U);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_WIFI && mock.count == 1U && mock.requests[0].command == LINX_VOICE_WIFI_SETUP &&
           mock.requests[0].generation != original.generation && mock.all_count == 3U &&
           mock.all_requests[1].command == LINX_VOICE_RELEASE && mock.all_requests[1].generation == original.generation &&
           mock.all_requests[2].command == LINX_VOICE_WIFI_SETUP);
    linx_voice_event_t late = {.generation=original.generation, .state=LINX_VOICE_READY};
    assert_rejected_voice_event(&late);
    linx_ui_set_voice_handler(NULL, NULL);

    /* START 未入队时，已接受的准备仍归原代号所有，清理不可遗漏。 */
    live_reset(&mock);
    original = last_live_prepare(&mock);
    mock.reject = true;
    start_voice_from_overview();
    uint32_t rejected = linx_ui_debug().voice_generation;
    assert(linx_ui_debug().voice_state == LINX_VOICE_ERROR && rejected != original.generation);
    linx_ui_deinit();
    assert(mock.all_count == 4U && mock.all_requests[2].command == LINX_VOICE_RELEASE &&
           mock.all_requests[2].generation == rejected && mock.all_requests[3].command == LINX_VOICE_RELEASE &&
           mock.all_requests[3].generation == original.generation && mock.count == 2U);
    linx_ui_init(lv_screen_active(), s_now);
    advance(800U);
    memset(&mock, 0, sizeof(mock));
    mock.reject = true;
    linx_ui_set_voice_handler(live_voice_request, &mock);
    enter_task(LINX_TASK_DESIGN);
    original = last_live_prepare(&mock);
    assert(mock.all_count == 1U && mock.count == 0U && linx_ui_page() == LINX_UI_OVERVIEW);
    advance(1000U);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_UP);
    assert(mock.all_count == 1U && mock.count == 0U);
    mock.reject = false;
    start_voice_from_overview();
    assert(mock.count == 1U && mock.requests[0].generation != original.generation);
    linx_ui_set_voice_handler(NULL, NULL);
    puts("语音准备生命周期：替换后端、重复清理和进入手机配网均取消原准备所有者，准备状态不串入配网页通过");
}

static void test_soft_cancel_retains_accepted_connection_owner(void)
{
    for (unsigned rebind = 0U; rebind < 2U; ++rebind) {
        live_voice_mock_t mock;
        live_reset(&mock);
        start_voice_from_overview();
        uint32_t owner = mock.requests[0].generation;
        assert(mock.requests[0].command == LINX_VOICE_START && owner == linx_ui_debug().voice_generation);
        mock.reject_prepare = true;
        back_gesture();
        linx_voice_request_t rejected = last_live_prepare(&mock);
        assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().voice_generation == 0U &&
               mock.count == 2U && mock.requests[1].command == LINX_VOICE_CANCEL &&
               mock.requests[1].generation == owner && rejected.generation != owner && mock.all_count == 4U);
        mock.reject_prepare = false;
        live_voice_mock_t replacement = {0};
        if (rebind) linx_ui_set_voice_handler(live_voice_request, &replacement);
        else linx_ui_deinit();
        assert(mock.count == 3U && mock.requests[2].command == LINX_VOICE_RELEASE &&
               mock.requests[2].generation == owner && mock.all_count == 5U);
        if (rebind) {
            assert(replacement.count == 0U && replacement.all_count == 1U &&
                   last_live_prepare(&replacement).generation != owner);
            linx_ui_set_voice_handler(NULL, NULL);
        }
    }
    puts("语音连接所有权：成功开始后软取消，返回准备被拒绝仍在销毁或替换时释放原已接受的录音代号通过");
}

static void test_read_only_reader_pages(void)
{
    for (unsigned live = 0U; live < 2U; ++live) {
        live_voice_mock_t mock;
        live_reset(&mock);
        if (!live) linx_ui_set_voice_handler(NULL, NULL);
        overview_action(0U);
        uint32_t turn = linx_ui_debug().turn_id;
        size_t pages = linx_ui_debug().read_page_count;
        assert(pages > 1U);
        for (size_t page = 0U; page < pages; ++page) {
            linx_demo_t before = *linx_ui_demo();
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == turn &&
                   linx_ui_debug().read_page == page && linx_ui_debug().voice_generation == 0U);
            assert(mock.count == 0U && memcmp(&before, linx_ui_demo(), sizeof(before)) == 0);
            assert(!visible_text_contains(lv_screen_active(), "OK回复") &&
                   !visible_text_contains(lv_screen_active(), "OK返回"));
            key(LINX_UI_KEY_DOWN);
        }
        back_gesture();
        assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 0U);
        assert(mock.count == 0U);
        start_voice_from_overview();
        if (live) {
            assert(mock.count == 1U && mock.requests[0].command == LINX_VOICE_START &&
                   mock.requests[0].context.source_turn_id == turn);
            live_event(LINX_VOICE_LISTENING, LINX_VOICE_OK, NULL);
            key(LINX_UI_KEY_OK);
            assert(mock.count == 2U && mock.requests[1].command == LINX_VOICE_FINISH);
            live_event(LINX_VOICE_SPEAKING, LINX_VOICE_OK, NULL);
            finish_live_reply();
        } else back_gesture();
        assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);
        overview_action(0U);
        assert(linx_ui_debug().turn_id == turn && linx_ui_debug().read_page == pages - 1U);
        back_gesture();
        overview_action(2U);
        const size_t entries[] = {0U, 1U}; /* 初始 agent 回复和用户指令。 */
        for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
            select_item(entries[i]);
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_READER);
            size_t count = mock.count;
            linx_ui_debug_t before = linx_ui_debug();
            key(LINX_UI_KEY_OK);
            assert(linx_ui_page() == LINX_UI_READER && linx_ui_debug().turn_id == before.turn_id &&
                   linx_ui_debug().read_page == before.read_page && mock.count == count);
            assert(!visible_text_contains(lv_screen_active(), "OK回复") &&
                   !visible_text_contains(lv_screen_active(), "OK返回"));
            back_gesture();
            assert(linx_ui_page() == LINX_UI_HISTORY && linx_ui_debug().selected == entries[i]);
        }
        linx_ui_set_voice_handler(NULL, NULL);
    }
    puts("阅读页只读：每页单击不录音、不发送、不返回，双击回概览后从继续沟通录入；播完回概览、重开保留页码，历史回复与用户正文同样只读通过");
}

static void test_live_voice_conversation(void)
{
    live_voice_mock_t mock;
    live_reset(&mock);
    uint32_t turn = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN)->id;
    start_voice_from_overview();
    assert(mock.count == 1U && mock.requests[0].command == LINX_VOICE_START);
    const linx_reply_context_t context = {linx_ui_demo()->scene_id, LINX_TASK_DESIGN, turn, 0U};
    assert_live_context(&context, &mock.requests[0].context);
    uint32_t first = mock.requests[0].generation;
    assert(first == linx_ui_debug().voice_generation);
    assert(linx_ui_debug().voice_state == LINX_VOICE_PREPARING);
    assert(visible_text_contains(lv_screen_active(), "正在准备录音") &&
           !visible_text_contains(lv_screen_active(), "正在连接语音服务"));
    capture("39-live-preparing");
    live_event(LINX_VOICE_CONNECTING, LINX_VOICE_OK, NULL);
    assert(visible_text_contains(lv_screen_active(), "正在连接语音服务"));
    capture("40-live-connecting");
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_UP);
    assert(mock.count == 1U);
    advance(8100U);
    assert_no_live_demo_send();
    live_event(LINX_VOICE_BINDING, LINX_VOICE_OK, "012345");
    assert(visible_text_contains(lv_screen_active(), "012345"));
    capture("41-live-binding");
    key(LINX_UI_KEY_OK);
    assert(mock.count == 2U && mock.requests[1].command == LINX_VOICE_START &&
           mock.requests[1].generation != first);
    assert_live_context(&context, &mock.requests[1].context);
    linx_voice_event_t stale = { .generation = first, .state = LINX_VOICE_LISTENING };
    assert_rejected_voice_event(&stale);
    live_event(LINX_VOICE_CONNECTING, LINX_VOICE_OK, NULL);
    live_event(LINX_VOICE_LISTENING, LINX_VOICE_OK, NULL);
    capture("42-live-listening");
    assert(visible_text_contains(lv_screen_active(), "00:00"));
    advance(8125U);
    assert(visible_text_contains(lv_screen_active(), "00:08"));
    assert_no_live_demo_send();
    live_event(LINX_VOICE_LISTENING, LINX_VOICE_OK, NULL);
    assert(visible_text_contains(lv_screen_active(), "00:08"));
    key(LINX_UI_KEY_OK);
    assert(mock.count == 3U && mock.requests[2].command == LINX_VOICE_FINISH &&
           mock.requests[2].generation == mock.requests[1].generation);
    assert_live_context(&context, &mock.requests[2].context);
    assert(linx_ui_debug().voice_state == LINX_VOICE_WAITING);
    assert_no_live_demo_send();
    capture("43-live-waiting");
    key(LINX_UI_KEY_OK);
    advance(10000U);
    assert(mock.count == 3U);
    assert_no_live_demo_send();
    live_event(LINX_VOICE_WAITING, LINX_VOICE_OK, NULL);
    live_event(LINX_VOICE_SPEAKING, LINX_VOICE_OK, NULL);
    capture("44-live-speaking");
    advance(250U);
    uint32_t speaking = linx_ui_debug().voice_generation;
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_OK);
    assert(mock.count == 3U && linx_ui_debug().voice_generation == speaking);
    assert(linx_ui_debug().voice_state == LINX_VOICE_SPEAKING);
    finish_live_reply();
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);
    assert(linx_ui_debug().voice_generation == 0U);
    assert(mock.count == 3U && mock.all_requests[mock.all_count - 1U].command == LINX_VOICE_PREPARE);
    stale.generation = speaking;
    stale.state = LINX_VOICE_FINISHED;
    assert_rejected_voice_event(&stale);
    start_voice_from_overview();
    assert(mock.count == 4U && mock.requests[3].command == LINX_VOICE_START);
    assert_live_context(&context, &mock.requests[3].context);
    assert_rejected_voice_event(&stale);
    stale.state = LINX_VOICE_PREPARING;
    assert_rejected_voice_event(&stale);
    live_event(LINX_VOICE_PREPARING, LINX_VOICE_OK, NULL);
    assert(visible_text_contains(lv_screen_active(), "正在准备录音") &&
           !visible_text_contains(lv_screen_active(), "正在连接语音服务"));
    live_event(LINX_VOICE_ERROR, LINX_VOICE_NETWORK_FAILED, NULL);
    capture("45-live-error");
    key(LINX_UI_KEY_OK);
    assert(mock.count == 5U && mock.requests[4].command == LINX_VOICE_START);
    live_event(LINX_VOICE_CLOSED, LINX_VOICE_OK, NULL);
    capture("46-live-closed");
    key(LINX_UI_KEY_OK);
    assert(mock.count == 6U && mock.requests[5].command == LINX_VOICE_START);
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_DOWN);
    assert(mock.count == 6U && linx_ui_page() == LINX_UI_VOICE);
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);
    assert(mock.count == 7U && mock.requests[6].command == LINX_VOICE_CANCEL &&
           mock.requests[6].generation == mock.requests[5].generation);
    assert_live_context(&context, &mock.requests[6].context);
    stale.generation = mock.requests[5].generation;
    assert_rejected_voice_event(&stale);
    linx_ui_set_voice_handler(NULL, NULL);
    puts("单条语音指令：绑定、结束等待、播完返回、失败重录与取消通过；播放按OK不再开启下一轮，无模拟发送回执");
}

static void test_live_voice_back_and_direction_keys(void)
{
    const linx_voice_state_t states[] = {
        LINX_VOICE_PREPARING, LINX_VOICE_DRAINING, LINX_VOICE_CONNECTING, LINX_VOICE_BINDING, LINX_VOICE_LISTENING,
        LINX_VOICE_WAITING, LINX_VOICE_SPEAKING, LINX_VOICE_ERROR, LINX_VOICE_CLOSED
    };
    size_t state_count = sizeof(states) / sizeof(states[0]);
    for (size_t example = 0U; example < state_count * 2U; ++example) {
        size_t i = example % state_count;
        linx_back_mode_t mode = example < state_count ? LINX_BACK_LONG_PRESS : LINX_BACK_DOUBLE_CLICK;
        live_voice_mock_t mock;
        live_reset(&mock);
        linx_ui_set_back_mode(mode);
        start_voice_from_overview();
        live_event(states[i], states[i] == LINX_VOICE_ERROR ? LINX_VOICE_NETWORK_FAILED : LINX_VOICE_OK,
                   states[i] == LINX_VOICE_BINDING ? "012345" : NULL);
        linx_ui_debug_t before = linx_ui_debug();
        linx_demo_t model = *linx_ui_demo();
        key(LINX_UI_KEY_UP);
        key(LINX_UI_KEY_DOWN);
        linx_ui_debug_t after = linx_ui_debug();
        assert(after.page == before.page && after.voice_state == before.voice_state &&
               after.voice_generation == before.voice_generation && mock.count == 1U);
        assert(memcmp(&model, linx_ui_demo(), sizeof(model)) == 0);
        assert(!visible_text_contains(lv_screen_active(), "↓返回"));
        assert(!visible_text_contains(lv_screen_active(), "↓取消"));
        assert(visible_text_contains(lv_screen_active(), mode == LINX_BACK_LONG_PRESS ? "长按返回" : "双击返回"));
        back_gesture();
        assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().voice_generation == 0U);
        assert(mock.count == 2U && mock.requests[1].command == LINX_VOICE_CANCEL &&
               mock.requests[1].generation == before.voice_generation);
        linx_demo_tick(&model, s_now);
        assert(memcmp(&model, linx_ui_demo(), sizeof(model)) == 0);
        key(LINX_UI_KEY_BACK);
        assert(linx_ui_page() == LINX_UI_TASKS && mock.count == 2U);
        linx_ui_set_voice_handler(NULL, NULL);
        assert(mock.count == 2U);
    }
    linx_ui_set_back_mode(LINX_BACK_LONG_PRESS);
    puts("语音返回：两种模式各状态上下键不取消，返回手势只软取消一次且不结束上传、不确认、不发送通过");
}

static void test_live_voice_draining_waits_for_backend(void)
{
    live_voice_mock_t mock;
    live_reset(&mock);
    start_voice_from_overview();
    uint32_t first = linx_ui_debug().voice_generation;
    live_event(LINX_VOICE_DRAINING, LINX_VOICE_OK, NULL);
    assert(visible_text_contains(lv_screen_active(), "正在结束上一条指令") &&
           !visible_text_contains(lv_screen_active(), "正在连接语音服务"));
    capture("47-live-draining");
    size_t all_count = mock.all_count;
    for (size_t i = 0U; i < 3U; ++i) {
        key(LINX_UI_KEY_OK);
        key(LINX_UI_KEY_UP);
        key(LINX_UI_KEY_DOWN);
        advance(4000U);
    }
    assert(mock.count == 1U && mock.all_count == all_count && linx_ui_page() == LINX_UI_VOICE &&
           linx_ui_debug().voice_generation == first && linx_ui_debug().voice_state == LINX_VOICE_DRAINING);
    assert_no_live_demo_send();
    linx_voice_event_t premature = {.generation=first, .state=LINX_VOICE_FINISHED};
    assert_rejected_voice_event(&premature);
    back_gesture();
    assert(mock.count == 2U && mock.requests[1].command == LINX_VOICE_CANCEL &&
           mock.requests[1].generation == first && linx_ui_page() == LINX_UI_OVERVIEW);
    linx_voice_request_t prepared = last_live_prepare(&mock);
    linx_voice_event_t late = {.generation=first, .state=LINX_VOICE_DRAINING};
    assert_rejected_voice_event(&late);
    start_voice_from_overview();
    uint32_t second = linx_ui_debug().voice_generation;
    assert(mock.count == 3U && mock.requests[2].command == LINX_VOICE_START && second != first &&
           second != prepared.generation);
    assert_rejected_voice_event(&late);
    live_event(LINX_VOICE_DRAINING, LINX_VOICE_OK, NULL);
    advance(31000U);
    key(LINX_UI_KEY_OK);
    assert(mock.count == 3U && linx_ui_debug().voice_generation == second &&
           linx_ui_debug().voice_state == LINX_VOICE_DRAINING);
    linx_voice_event_t ready = {.generation=second, .state=LINX_VOICE_READY};
    assert_rejected_voice_event(&ready);
    live_event(LINX_VOICE_PREPARING, LINX_VOICE_OK, NULL);
    live_event(LINX_VOICE_LISTENING, LINX_VOICE_OK, NULL);
    assert(mock.count == 3U && visible_text_contains(lv_screen_active(), "00:00"));
    key(LINX_UI_KEY_OK);
    assert(mock.count == 4U && mock.requests[3].command == LINX_VOICE_FINISH &&
           mock.requests[3].generation == second);
    live_event(LINX_VOICE_SPEAKING, LINX_VOICE_OK, NULL);
    finish_live_reply();
    assert(mock.count == 4U && linx_ui_page() == LINX_UI_OVERVIEW);
    prepared = last_live_prepare(&mock);
    linx_ui_set_voice_handler(NULL, NULL);
    assert(mock.count == 4U && mock.all_requests[mock.all_count - 1U].command == LINX_VOICE_RELEASE &&
           mock.all_requests[mock.all_count - 1U].generation == prepared.generation);
    puts("上一条指令排空：等待期间单击和计时不重录、不发送，双击软取消；旧状态拒绝，后端明确就绪后才进入监听通过");
}

static void test_live_voice_finished_returns_to_origin(void)
{
    for (unsigned speaking = 0U; speaking < 2U; ++speaking) {
        live_voice_mock_t mock;
        live_reset(&mock);
        select_item(1U);
        linx_ui_debug_t origin = linx_ui_debug();
        key(LINX_UI_KEY_OK);
        assert(linx_ui_page() == LINX_UI_VOICE && mock.count == 1U);
        linx_voice_event_t finished = { .generation = linx_ui_debug().voice_generation,
                                      .state = LINX_VOICE_FINISHED };
        assert_rejected_voice_event(&finished);
        live_event(LINX_VOICE_LISTENING, LINX_VOICE_OK, NULL);
        assert_rejected_voice_event(&finished);
        key(LINX_UI_KEY_OK);
        assert(mock.count == 2U && mock.requests[1].command == LINX_VOICE_FINISH);
        finished.error = LINX_VOICE_TIMEOUT;
        assert_rejected_voice_event(&finished);
        /* 状态快照队列可能覆盖 SPEAKING，WAITING 也必须能接收终态。 */
        if (speaking) live_event(LINX_VOICE_SPEAKING, LINX_VOICE_OK, NULL);
        finish_live_reply();
        linx_ui_debug_t restored = linx_ui_debug();
        assert(restored.page == LINX_UI_OVERVIEW && restored.selected == origin.selected &&
               restored.task_id == origin.task_id && restored.turn_id == origin.turn_id &&
               restored.image_id == 0U && restored.read_page == origin.read_page);
        assert(mock.count == 2U && mock.all_requests[mock.all_count - 1U].command == LINX_VOICE_PREPARE);
        key(LINX_UI_KEY_OK);
        assert(linx_ui_page() == LINX_UI_VOICE && mock.count == 3U &&
               mock.requests[2].generation != mock.requests[0].generation);
        assert_live_context(&mock.requests[0].context, &mock.requests[2].context);
        linx_ui_set_voice_handler(NULL, NULL);
    }
    puts("单轮完成返回概览继续沟通：等待或播报后恢复原任务，再次录入沿用上下文且代号更新；提前/错误/重复终态被拒绝通过");
}

static void test_live_voice_validation_and_failures(void)
{
    live_voice_mock_t mock;
    live_reset(&mock);
    linx_voice_event_t event = { .generation = 1U, .state = LINX_VOICE_LISTENING };
    assert_rejected_voice_event(NULL);
    assert_rejected_voice_event(&event);
    mock.reject = true;
    start_voice_from_overview();
    assert(mock.count == 1U && linx_ui_debug().voice_state == LINX_VOICE_ERROR &&
           linx_ui_debug().voice_error == LINX_VOICE_NO_MEMORY);
    assert(visible_text_contains(lv_screen_active(), "内存不足，请重试"));
    assert_no_live_demo_send();
    mock.reject = false;
    key(LINX_UI_KEY_OK);
    assert(mock.count == 2U && linx_ui_debug().voice_state == LINX_VOICE_PREPARING);
    event.generation = linx_ui_debug().voice_generation;
    event.state = (linx_voice_state_t)-1;
    assert_rejected_voice_event(&event);
    event.state = (linx_voice_state_t)(LINX_VOICE_DRAINING + 1);
    assert_rejected_voice_event(&event);
    event.state = LINX_VOICE_LISTENING;
    event.error = (linx_voice_error_t)-1;
    assert_rejected_voice_event(&event);
    event.error = (linx_voice_error_t)(LINX_VOICE_TIMEOUT + 1);
    assert_rejected_voice_event(&event);
    event.error = LINX_VOICE_OK;
    event.generation = 0U;
    assert_rejected_voice_event(&event);
    event.generation = linx_ui_debug().voice_generation;
    event.state = LINX_VOICE_BINDING;
    memcpy(event.activation_code, "123456x", sizeof(event.activation_code));
    assert_rejected_voice_event(&event);
    event.activation_code[6] = '\0';
    for (size_t i = 0U; i < 6U; ++i) {
        event.activation_code[i] = i % 2U ? ':' : '/';
        assert_rejected_voice_event(&event);
        event.activation_code[i] = (char)('1' + i);
    }
    static const char *const messages[] = {
        "请检查网络后重试", "请先设置Wi-Fi", "Wi-Fi连接失败", "网络时间同步失败",
        "灵矽连接已断开", "语音协议不兼容", "音频设备无法使用", "内存不足，请重试", "灵矽回复超时"
    };
    for (size_t i = 0U; i <= LINX_VOICE_TIMEOUT; ++i) {
        live_event(LINX_VOICE_ERROR, (linx_voice_error_t)i, NULL);
        assert(visible_text_contains(lv_screen_active(), messages[i]));
        if (i == LINX_VOICE_NOT_CONFIGURED) continue; /* 此错误由专门的配网用例验证。 */
        uint32_t failed = linx_ui_debug().voice_generation;
        size_t count = mock.count;
        key(LINX_UI_KEY_OK);
        assert(mock.count == count + 1U && mock.requests[count].command == LINX_VOICE_START &&
               linx_ui_debug().voice_generation != failed);
    }
    live_event(LINX_VOICE_LISTENING, LINX_VOICE_OK, NULL);
    mock.reject = true;
    size_t count = mock.count;
    key(LINX_UI_KEY_OK);
    assert(mock.count == count + 1U && mock.requests[count].command == LINX_VOICE_FINISH);
    assert(linx_ui_debug().voice_state == LINX_VOICE_ERROR &&
           linx_ui_debug().voice_error == LINX_VOICE_NO_MEMORY);
    assert_no_live_demo_send();
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && mock.requests[mock.count - 1U].command == LINX_VOICE_CANCEL);
    assert(linx_ui_debug().voice_generation == 0U);
    linx_ui_set_voice_handler(NULL, NULL);
    event.state = LINX_VOICE_LISTENING;
    assert_rejected_voice_event(&event);
    puts("灵矽事件输入：空值、越界状态、错误、失效编号及非法激活码均拒绝；全部错误文案和后端拒绝重试通过");
}

static void test_live_voice_context_and_lifecycle(void)
{
    live_voice_mock_t mock;
    live_reset(&mock);
    select_item(1U);
    linx_ui_debug_t origin = linx_ui_debug();
    uint32_t source = linx_demo_latest_turn(linx_ui_demo(), LINX_TASK_DESIGN)->id;
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_VOICE && mock.count == 1U);
    assert(mock.requests[0].context.task_id == origin.task_id &&
           mock.requests[0].context.source_turn_id == source && mock.requests[0].context.image_id == 0U);
    key(LINX_UI_KEY_BACK);
    assert(mock.count == 2U && mock.requests[1].command == LINX_VOICE_CANCEL);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == origin.selected &&
           linx_ui_debug().turn_id == origin.turn_id && linx_ui_debug().image_id == 0U);
    key(LINX_UI_KEY_OK);
    assert(mock.count == 3U);
    live_voice_mock_t replacement = {0};
    linx_ui_set_voice_handler(live_voice_request, &replacement);
    settle();
    assert(linx_ui_page() == LINX_UI_OVERVIEW && mock.count == 4U &&
           mock.requests[3].command == LINX_VOICE_RELEASE && replacement.count == 0U);
    key(LINX_UI_KEY_OK);
    assert(replacement.count == 1U && replacement.requests[0].command == LINX_VOICE_START);
    uint32_t generation = replacement.requests[0].generation;
    linx_ui_deinit();
    assert(replacement.count == 2U && replacement.requests[1].command == LINX_VOICE_RELEASE &&
           replacement.requests[1].generation == generation);
    linx_ui_deinit();
    assert(replacement.count == 2U);
    linx_ui_init(lv_screen_active(), s_now);
    advance(800U);
    linx_voice_event_t late = { .generation = generation, .state = LINX_VOICE_SPEAKING };
    assert_rejected_voice_event(&late);

    /* 切换离线确认页的后端只放弃草稿，不将它提交或触发开始。 */
    enter_task(LINX_TASK_DESIGN);
    start_voice_from_overview();
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_CONFIRM);
    linx_ui_set_voice_handler(live_voice_request, &replacement);
    settle();
    assert(linx_ui_page() == LINX_UI_OVERVIEW && replacement.count == 2U);
    assert(linx_demo_task(linx_ui_demo(), LINX_TASK_DESIGN)->pending.id == 0U);
    linx_ui_set_voice_handler(NULL, NULL);
    puts("灵矽生命周期：概览任务和轮次上下文、原位返回、替换后端和重复清理均正确取消；确认页切换放弃草稿通过");
}

static void assert_wifi_mode(linx_ui_wifi_mode_t mode)
{
    assert(linx_ui_page() == LINX_UI_WIFI && linx_ui_debug().wifi_mode == mode);
    assert(linx_ui_debug().command_id == 0U);
}

static void phone_result(linx_voice_state_t state, linx_voice_error_t error,
                         const char *ssid, const char *password)
{
    linx_voice_event_t event = { .generation = linx_ui_debug().voice_generation,
                                .state = state, .error = error };
    if (ssid) {
        assert(strlen(ssid) < sizeof(event.wifi_ap_ssid));
        memcpy(event.wifi_ap_ssid, ssid, strlen(ssid) + 1U);
    }
    if (password) {
        assert(strlen(password) == 8U);
        memcpy(event.wifi_ap_password, password, sizeof(event.wifi_ap_password));
    }
    assert(linx_ui_voice_event(&event));
    settle();
}

static void phone_from_settings(live_voice_mock_t *mock)
{
    live_reset(mock);
    to_tasks();
    assert(visible_text_contains(lv_screen_active(), "长按设置"));
    back_gesture();
    assert(linx_ui_page() == LINX_UI_SETTINGS && linx_ui_debug().selected == 0U);
    assert(visible_text_contains(lv_screen_active(), "设备设置"));
    key(LINX_UI_KEY_OK);
    assert_wifi_mode(LINX_UI_WIFI_SCANNING);
    assert(mock->count == 1U && mock->requests[0].command == LINX_VOICE_WIFI_SETUP);
}

static void assert_phone_keys_idle(live_voice_mock_t *mock)
{
    size_t count = mock->count;
    linx_ui_debug_t before = linx_ui_debug();
    key(LINX_UI_KEY_UP);
    key(LINX_UI_KEY_DOWN);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == before.page && linx_ui_debug().wifi_mode == before.wifi_mode &&
           linx_ui_debug().voice_generation == before.voice_generation && mock->count == count);
}

static void test_phone_wifi_pages_and_retry(void)
{
    live_voice_mock_t mock;
    phone_from_settings(&mock);
    capture("50-wifi-phone-preparing");
    assert_phone_keys_idle(&mock);
    phone_result(LINX_VOICE_WIFI_SCANNING, LINX_VOICE_OK, NULL, NULL);
    phone_result(LINX_VOICE_WIFI_PORTAL, LINX_VOICE_OK, "LinxBox-Setup-1234", "01234567");
    assert_wifi_mode(LINX_UI_WIFI_PHONE);
    assert(visible_text_contains(lv_screen_active(), "LinxBox-Setup-1234"));
    assert(visible_text_contains(lv_screen_active(), "01234567"));
    assert(visible_text_contains(lv_screen_active(), "192.168.4.1"));
    capture("51-wifi-phone-portal");
    assert_phone_keys_idle(&mock);
    phone_result(LINX_VOICE_WIFI_PORTAL, LINX_VOICE_WIFI_FAILED, "LinxBox-Setup-1234", "01234567");
    assert_wifi_mode(LINX_UI_WIFI_PHONE);
    assert(visible_text_contains(lv_screen_active(), "重试"));
    capture("52-wifi-phone-retry");
    assert_phone_keys_idle(&mock);
    phone_result(LINX_VOICE_WIFI_CONNECTING, LINX_VOICE_OK, NULL, NULL);
    assert_wifi_mode(LINX_UI_WIFI_CONNECTING);
    assert(visible_text_contains(lv_screen_active(), "LinxBox-Setup-1234"));
    assert(visible_text_contains(lv_screen_active(), "01234567"));
    capture("53-wifi-phone-connecting");
    assert_phone_keys_idle(&mock);
    phone_result(LINX_VOICE_ERROR, LINX_VOICE_TIMEOUT, NULL, NULL);
    assert_wifi_mode(LINX_UI_WIFI_ERROR);
    assert(!visible_text_contains(lv_screen_active(), "01234567"));
    capture("54-wifi-phone-error");
    uint32_t failed = linx_ui_debug().voice_generation;
    key(LINX_UI_KEY_OK);
    assert_wifi_mode(LINX_UI_WIFI_SCANNING);
    assert(mock.count == 2U && mock.requests[1].command == LINX_VOICE_WIFI_SETUP &&
           mock.requests[1].generation != failed);
    assert(!visible_text_contains(lv_screen_active(), "01234567"));
    linx_voice_event_t old = {.generation=failed, .state=LINX_VOICE_WIFI_SAVED};
    assert_rejected_voice_event(&old);
    phone_result(LINX_VOICE_WIFI_PORTAL, LINX_VOICE_OK, "LinxBox-Setup-5678", "76543210");
    phone_result(LINX_VOICE_WIFI_CONNECTING, LINX_VOICE_OK, NULL, NULL);
    phone_result(LINX_VOICE_WIFI_SAVED, LINX_VOICE_OK, NULL, NULL);
    assert_wifi_mode(LINX_UI_WIFI_CONNECTED);
    assert(!visible_text_contains(lv_screen_active(), "76543210"));
    capture("55-wifi-phone-connected");
    key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_SETTINGS && mock.requests[mock.count - 1U].command == LINX_VOICE_RELEASE);
    /* 新网络项后的原设置仍按原语义执行。 */
    select_item(1U); key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_RESET);
    key(LINX_UI_KEY_BACK);
    select_item(2U); key(LINX_UI_KEY_OK);
    assert(!linx_ui_demo()->online);
    key(LINX_UI_KEY_OK);
    assert(linx_ui_demo()->online);
    select_item(3U); key(LINX_UI_KEY_OK);
    assert(linx_ui_demo()->fail_next_send);
    select_item(4U); key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_ABOUT);
    key(LINX_UI_KEY_OK);
    linx_ui_set_voice_handler(NULL, NULL);
    puts("手机配网页面：准备、热点说明、手机重试、连接保留热点信息、致命错误重开与成功返回通过");
}

static void test_phone_wifi_event_validation(void)
{
    live_voice_mock_t mock;
    phone_from_settings(&mock);
    linx_voice_event_t event = { .generation = linx_ui_debug().voice_generation,
                                .state = LINX_VOICE_LISTENING };
    assert_rejected_voice_event(&event);
    event.state = (linx_voice_state_t)-1;
    assert_rejected_voice_event(&event);
    event.state = (linx_voice_state_t)(LINX_VOICE_WIFI_PORTAL + 1);
    assert_rejected_voice_event(&event);
    event.state = LINX_VOICE_PREPARING;
    assert_rejected_voice_event(&event);
    event.state = LINX_VOICE_DRAINING;
    assert_rejected_voice_event(&event);
    event.state = LINX_VOICE_WIFI_PORTAL;
    memcpy(event.wifi_ap_password, "01234567", sizeof(event.wifi_ap_password));
    assert_rejected_voice_event(&event); /* SSID 不能为空。 */
    memset(event.wifi_ap_ssid, 'x', sizeof(event.wifi_ap_ssid));
    assert_rejected_voice_event(&event); /* SSID 必须终止。 */
    memcpy(event.wifi_ap_ssid, "LinxBox", sizeof("LinxBox"));
    const unsigned char forbidden[] = {0x1fU, 0x7fU, 0x80U, 0xffU};
    for (size_t i = 0U; i < sizeof(forbidden); ++i) {
        event.wifi_ap_ssid[3] = (char)forbidden[i];
        assert_rejected_voice_event(&event);
    }
    event.wifi_ap_ssid[3] = 'x';
    event.wifi_ap_password[8] = '9';
    assert_rejected_voice_event(&event);
    event.wifi_ap_password[8] = '\0';
    for (size_t i = 0U; i < 8U; ++i) {
        event.wifi_ap_password[i] = i % 2U ? ':' : '/';
        assert_rejected_voice_event(&event);
        event.wifi_ap_password[i] = (char)('0' + i);
    }
    event.wifi_ap_password[7] = '\0';
    assert_rejected_voice_event(&event);
    event.wifi_ap_password[7] = '7';
    memset(event.wifi_ap_ssid, 'x', sizeof(event.wifi_ap_ssid) - 1U);
    event.wifi_ap_ssid[sizeof(event.wifi_ap_ssid) - 1U] = '\0';
    assert(linx_ui_voice_event(&event)); /* 最长 32 字节可打印 SSID。 */
    settle();
    assert_wifi_mode(LINX_UI_WIFI_PHONE);
    phone_result(LINX_VOICE_WIFI_PORTAL, LINX_VOICE_OK, " ~", "99999999");
    assert_wifi_mode(LINX_UI_WIFI_PHONE);
    assert(visible_text_contains(lv_screen_active(), "99999999"));
    key(LINX_UI_KEY_BACK);
    assert(linx_ui_page() == LINX_UI_SETTINGS && mock.requests[mock.count - 1U].command == LINX_VOICE_RELEASE);
    assert_rejected_voice_event(&event);
    linx_ui_set_voice_handler(NULL, NULL);
    puts("手机配网事件：状态范围、过期代号、空/未终止/不可打印SSID、八位数字热点密码边界均验证通过");
}

static void test_phone_wifi_lifecycle_and_voice_resume(void)
{
    live_voice_mock_t mock;
    for (size_t mode = 0U; mode < 5U; ++mode) {
        phone_from_settings(&mock);
        if (mode > 0U) phone_result(LINX_VOICE_WIFI_PORTAL, LINX_VOICE_OK, "LinxBox-Setup", "12345678");
        if (mode == 2U) phone_result(LINX_VOICE_WIFI_CONNECTING, LINX_VOICE_OK, NULL, NULL);
        if (mode == 3U) phone_result(LINX_VOICE_ERROR, LINX_VOICE_NETWORK_FAILED, NULL, NULL);
        if (mode == 4U) phone_result(LINX_VOICE_WIFI_SAVED, LINX_VOICE_OK, NULL, NULL);
        uint32_t generation = linx_ui_debug().voice_generation;
        size_t count = mock.count;
        back_gesture();
        assert(linx_ui_page() == LINX_UI_SETTINGS && mock.count == count + 1U &&
               mock.requests[count].command == LINX_VOICE_RELEASE && mock.requests[count].generation == generation);
        assert(!visible_text_contains(lv_screen_active(), "12345678"));
        linx_ui_set_voice_handler(NULL, NULL);
        assert(mock.count == count + 1U);
    }
    phone_from_settings(&mock);
    mock.reject = true;
    phone_result(LINX_VOICE_ERROR, LINX_VOICE_WIFI_FAILED, NULL, NULL);
    key(LINX_UI_KEY_OK);
    assert_wifi_mode(LINX_UI_WIFI_ERROR);
    assert(linx_ui_debug().voice_error == LINX_VOICE_NO_MEMORY);
    mock.reject = false;
    key(LINX_UI_KEY_OK);
    assert_wifi_mode(LINX_UI_WIFI_SCANNING);
    phone_result(LINX_VOICE_WIFI_PORTAL, LINX_VOICE_OK, "LinxBox-Setup", "12345678");
    size_t count = mock.count;
    live_voice_mock_t replacement = {0};
    linx_ui_set_voice_handler(live_voice_request, &replacement);
    settle();
    assert(linx_ui_page() == LINX_UI_SETTINGS && mock.count == count + 1U &&
           mock.requests[count].command == LINX_VOICE_RELEASE && replacement.count == 0U);
    assert(!visible_text_contains(lv_screen_active(), "12345678"));
    key(LINX_UI_KEY_OK);
    phone_result(LINX_VOICE_WIFI_PORTAL, LINX_VOICE_OK, "LinxBox-Setup", "12345678");
    count = replacement.count;
    linx_ui_deinit();
    assert(replacement.count == count + 1U && replacement.requests[count].command == LINX_VOICE_RELEASE);
    linx_ui_init(lv_screen_active(), s_now);
    advance(800U);

    live_reset(&mock);
    start_voice_from_overview();
    linx_reply_context_t context = mock.requests[0].context;
    live_event(LINX_VOICE_ERROR, LINX_VOICE_NOT_CONFIGURED, NULL);
    uint32_t voice_generation = linx_ui_debug().voice_generation;
    key(LINX_UI_KEY_OK);
    assert_wifi_mode(LINX_UI_WIFI_SCANNING);
    assert(mock.count == 3U && mock.requests[1].command == LINX_VOICE_RELEASE &&
           mock.requests[1].generation == voice_generation && mock.requests[2].command == LINX_VOICE_WIFI_SETUP);
    linx_voice_event_t late = {.generation=voice_generation, .state=LINX_VOICE_LISTENING};
    assert_rejected_voice_event(&late);
    phone_result(LINX_VOICE_WIFI_PORTAL, LINX_VOICE_OK, "LinxBox-Setup", "12345678");
    phone_result(LINX_VOICE_WIFI_CONNECTING, LINX_VOICE_OK, NULL, NULL);
    uint32_t setup_generation = linx_ui_debug().voice_generation;
    phone_result(LINX_VOICE_WIFI_SAVED, LINX_VOICE_OK, NULL, NULL);
    assert_no_live_demo_send();
    assert(linx_ui_debug().voice_state == LINX_VOICE_PREPARING);
    assert(mock.requests[mock.count - 1U].command == LINX_VOICE_START &&
           mock.requests[mock.count - 1U].generation != setup_generation);
    assert_live_context(&context, &mock.requests[mock.count - 1U].context);
    assert(!visible_text_contains(lv_screen_active(), "12345678"));
    late.generation = setup_generation; late.state = LINX_VOICE_WIFI_SAVED;
    assert_rejected_voice_event(&late);
    live_event(LINX_VOICE_ERROR, LINX_VOICE_NOT_CONFIGURED, NULL);
    key(LINX_UI_KEY_OK);
    key(LINX_UI_KEY_BACK);
    assert_no_live_demo_send();
    assert(linx_ui_debug().voice_error == LINX_VOICE_NOT_CONFIGURED && linx_ui_debug().voice_generation == 0U);
    key(LINX_UI_KEY_BACK);
    linx_ui_set_voice_handler(NULL, NULL);
    puts("手机配网生命周期：各模式取消、后端拒绝/替换、清理、原语音上下文恢复及迟到结果拒绝通过");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    s_screens = argv[1];
    assert(LV_MEM_SIZE == 40U * 1024U);
    lv_init();
    check_inventory();
    check_remote_readability();
    test_template_reader_layouts();
    s_display = lv_display_create(WIDTH, HEIGHT);
    assert(s_display != NULL);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL, sizeof(s_draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, flush);
    linx_ui_init(lv_screen_active(), s_now);
    capture("00-boot");
    advance(800U);
    assert(linx_ui_page() == LINX_UI_TASKS);
    capture("01-tasks");
    test_timeout_release_barriers();
    test_configured_back_cancels_demo_voice();
    reset_scene();
    test_full_reader();
    test_login_list_reader();
    test_failure_recovery();
    test_queue_gates();
    test_background_does_not_steal_focus();
    test_reset_and_other_screens();
    test_repeat_memory();
    test_lifecycle_and_boot_keys();
    test_menu_boundaries();
    test_text_only_navigation();
    test_explicit_cancel_and_failed_receipt();
    test_payment_numeric_reply();
    test_incomplete_history_user();
    test_background_completion_during_voice();
    test_background_history_eviction();
    test_back_mode_settings();
    test_background_voice_preparation();
    test_background_voice_context_refresh();
    test_background_voice_owner_lifecycle();
    test_soft_cancel_retains_accepted_connection_owner();
    test_read_only_reader_pages();
    test_live_voice_conversation();
    test_live_voice_back_and_direction_keys();
    test_live_voice_draining_waits_for_backend();
    test_live_voice_finished_returns_to_origin();
    test_live_voice_validation_and_failures();
    test_live_voice_context_and_lifecycle();
    test_phone_wifi_pages_and_retry();
    test_phone_wifi_event_validation();
    test_phone_wifi_lifecycle_and_voice_resume();
    linx_ui_deinit();
    lv_display_delete(s_display);
    lv_deinit();
    printf("Real LVGL host PASS: %zu screenshots, %zu key events, %zu actual-label audits\n",
           s_captures, s_key_count, s_label_checks);
    return 0;
}
