/* 私有绘制器的容量与输入边界；使用真实实现和 LVGL，不改写演示模型。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../main/linx_ui.c"

enum { WIDTH = 240, HEIGHT = 320 };
static uint16_t draw_buffer[WIDTH * 40];
static uint16_t frame[HEIGHT][WIDTH];
static lv_display_t *display;
static unsigned flush_count;

static void flush(lv_display_t *target, const lv_area_t *area, uint8_t *pixels)
{
    lv_draw_buf_t *buffer = lv_display_get_buf_active(target);
    assert(buffer != NULL && buffer->header.cf == LV_COLOR_FORMAT_RGB565);
    assert((uint32_t)(area->y2 - area->y1 + 1) * buffer->header.stride <= sizeof(draw_buffer));
    for (int y = area->y1; y <= area->y2; ++y) {
        assert(y >= 0 && y < HEIGHT);
        const uint16_t *row = (const uint16_t *)(pixels + (y - area->y1) * buffer->header.stride);
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < WIDTH);
            frame[y][x] = row[x - area->x1];
        }
    }
    ++flush_count;
    lv_display_flush_ready(target);
}

static void check_reset(void)
{
    reset_objects();
    assert(ui.labels_used == 0 && ui.boxes_used == 0 && ui.decoration_count == 0);
    assert(!ui.render_overflow && ui.voice_counter == NULL);
    for (size_t i = 0; i < LABEL_COUNT; ++i) assert(lv_obj_has_flag(ui.labels[i], LV_OBJ_FLAG_HIDDEN));
    for (size_t i = 0; i < BOX_COUNT; ++i) assert(lv_obj_has_flag(ui.boxes[i], LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(ui.image, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_child_count(ui.root) == LABEL_COUNT + BOX_COUNT + 2);
    assert(lv_mem_test() == LV_RESULT_OK);
}

static void fill_labels(void)
{
    check_reset();
    for (size_t i = 0; i < LABEL_COUNT; ++i) {
        assert(text("x", 4, 5, 30, 20, &linx_font_12, COLOR_INK) == ui.labels[i]);
    }
    assert(ui.labels_used == LABEL_COUNT && !ui.render_overflow);
}

static void check_full_labels(void)
{
    assert(ui.labels_used == LABEL_COUNT && ui.render_overflow);
    for (size_t i = 0; i < LABEL_COUNT; ++i) assert(strcmp(lv_label_get_text(ui.labels[i]), "x") == 0);
    assert(lv_obj_get_child_count(ui.root) == LABEL_COUNT + BOX_COUNT + 2);
    assert(lv_mem_test() == LV_RESULT_OK);
}

static void test_label_capacity(void)
{
    fill_labels();
    const linx_demo_t model = ui.demo;
    assert(text("不能覆盖旧对象", 0, 0, 20, 20, &linx_font_12, COLOR_ACCENT) == NULL);
    assert(text(NULL, 0, 0, 20, 20, &linx_font_12, COLOR_ACCENT) == NULL);
    check_full_labels();
    assert(memcmp(&model, &ui.demo, sizeof(model)) == 0);

    check_reset();
    lv_obj_t *label = text(NULL, 4, 5, 30, 20, &linx_font_12, COLOR_MUTED);
    assert(label == ui.labels[0] && strcmp(lv_label_get_text(label), "") == 0);
    assert(ui.labels_used == 1 && !ui.render_overflow);
    assert(!lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    lv_obj_update_layout(ui.root);
    assert(lv_obj_get_x(label) == 4 && lv_obj_get_y(label) == 5);
    assert(lv_obj_get_width(label) == 30 && lv_obj_get_height(label) == 20);
    assert(lv_obj_get_style_text_font(label, LV_PART_MAIN) == &linx_font_12);
    assert(lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), lv_color_hex(COLOR_MUTED)));
}

static void test_box_capacity(void)
{
    check_reset();
    lv_obj_t *image = ui.image;
    lv_obj_t *decor = ui.decor;
    for (size_t i = 0; i < BOX_COUNT; ++i) {
        lv_obj_t *object = box(12, 13, 40, 20, COLOR_SELECTED, 5);
        assert(object == ui.boxes[i] && !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_get_style_radius(object, LV_PART_MAIN) == 5);
        assert(lv_obj_get_style_border_width(object, LV_PART_MAIN) == 0);
    }
    assert(!ui.render_overflow && ui.boxes_used == BOX_COUNT);
    assert(box(0, 0, 1, 1, COLOR_ACCENT, 0) == NULL);
    assert(ui.render_overflow && ui.boxes_used == BOX_COUNT);
    assert(ui.image == image && ui.decor == decor && lv_mem_test() == LV_RESULT_OK);
    check_reset();
    assert(box(1, 2, 3, 4, COLOR_LINE, 0) == ui.boxes[0]);
    assert(ui.boxes_used == 1 && !ui.render_overflow);
}

static void test_decoration_capacity(void)
{
    decoration_t saved[DECOR_COUNT];
    check_reset();
    for (size_t i = 0; i < DECOR_COUNT; ++i) stroke(1, 2, 3, 4, COLOR_INK, 2);
    assert(ui.decoration_count == DECOR_COUNT && !ui.render_overflow);
    memcpy(saved, ui.decorations, sizeof(saved));
    stroke(20, 30, 40, 50, COLOR_ACCENT, 3);
    assert(ui.render_overflow && ui.decoration_count == DECOR_COUNT);
    assert(memcmp(saved, ui.decorations, sizeof(saved)) == 0);

    check_reset();
    for (size_t i = 0; i < DECOR_COUNT; ++i) shape(5, 6, 20, 10, COLOR_INK, 1, 2);
    assert(ui.decoration_count == DECOR_COUNT && !ui.render_overflow);
    memcpy(saved, ui.decorations, sizeof(saved));
    shape(20, 30, 40, 50, COLOR_ACCENT, 0, 0);
    assert(ui.render_overflow && ui.decoration_count == DECOR_COUNT);
    assert(memcmp(saved, ui.decorations, sizeof(saved)) == 0);
    assert(saved[DECOR_COUNT - 1].rectangle && saved[DECOR_COUNT - 1].x2 == 24);
    assert(saved[DECOR_COUNT - 1].y2 == 15 && saved[DECOR_COUNT - 1].radius == 2);

    check_reset();
    stroke(10, 10, 20, 10, COLOR_INK, 1);
    shape(40, 40, 10, 10, COLOR_ACCENT, 0, 0);
    shape(70, 40, 10, 10, COLOR_INK, 1, 0);
    assert(ui.decoration_count == 3 && !ui.render_overflow);
    assert(!ui.decorations[0].rectangle && ui.decorations[0].width == 1);
    assert(ui.decorations[1].rectangle && ui.decorations[1].width == 0);
    assert(ui.decorations[2].rectangle && ui.decorations[2].width == 1);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_invalidate(ui.decor);
    lv_refr_now(display);
    assert(flush_count > 0 && lv_mem_test() == LV_RESULT_OK);
    assert(frame[45][45] == lv_color_to_u16(lv_color_hex(COLOR_ACCENT)));
    assert(frame[40][70] == lv_color_to_u16(lv_color_hex(COLOR_INK)));
    assert(frame[45][75] == lv_color_to_u16(lv_color_hex(COLOR_PAPER)));
}

static void test_utf8_and_width_bounds(void)
{
    const char sample[] = "A\xc2\xb7\xe6\xb1\x89\xf0\x9f\x99\x82";
    const uint32_t codepoints[] = { 0x41, 0xb7, 0x6c49, 0x1f642 };
    const size_t offsets[] = { 1, 3, 6, 10 };
    size_t offset = 0;
    int32_t expected_width = 0;
    for (size_t i = 0; i < 4; ++i) {
        assert(next_codepoint(sample, sizeof(sample) - 1, &offset) == codepoints[i]);
        assert(offset == offsets[i]);
        expected_width += lv_font_get_glyph_width(&linx_font_16, codepoints[i], i == 3 ? 0 : codepoints[i + 1]);
    }
    assert(next_codepoint(sample, sizeof(sample) - 1, &offset) == 0 && offset == 10);
    assert(measure(sample, sizeof(sample) - 1, (void *)&linx_font_16) == expected_width);
    assert(measure(NULL, 0, (void *)&linx_font_16) == 0);

    /* 此内部解码器不承担 UTF-8 校验；截断时只消费明确给定的字节窗口。 */
    const char truncated[] = "\xe4\xb8\xad";
    offset = 0;
    assert(next_codepoint(truncated, 2, &offset) == 0x138 && offset == 2);
    assert(next_codepoint(truncated, 2, &offset) == 0 && offset == 2);
    offset = 0;
    assert(next_codepoint("\xc2", 1, &offset) == 2 && offset == 1);
}

static void test_footer_truncation(void)
{
    check_reset();
    char long_line[160];
    memset(long_line, 'x', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = '\0';
    footer(long_line);
    assert(ui.labels_used == 2 && !ui.render_overflow);
    assert(strlen(lv_label_get_text(ui.labels[0])) == 79);
    assert(strspn(lv_label_get_text(ui.labels[0]), "x") == 79);
    assert(strcmp(lv_label_get_text(ui.labels[1]), "长按返回") == 0);
    check_reset();
    footer("OK确认\n长按OK返回");
    assert(strcmp(lv_label_get_text(ui.labels[0]), "OK确认") == 0);
}

static void test_stack_capacity(void)
{
    linx_ui_init(lv_screen_active(), 0);
    for (size_t i = 0; i < STACK_LIMIT; ++i) {
        push(LINX_UI_OVERVIEW);
        assert(ui.depth == i + 1);
    }
    location_t saved[STACK_LIMIT];
    memcpy(saved, ui.stack, sizeof(saved));
    push(LINX_UI_HISTORY);
    assert(ui.depth == STACK_LIMIT && ui.at.page == LINX_UI_HISTORY);
    assert(memcmp(saved, ui.stack, sizeof(saved)) == 0);
    for (size_t i = STACK_LIMIT; i > 0; --i) {
        pop();
        assert(ui.depth == i - 1);
    }
    assert(ui.at.page == LINX_UI_BOOT);
    pop();
    assert(ui.depth == 0 && ui.at.page == LINX_UI_TASKS);
}

static void test_renderers_handle_exhausted_pools(void)
{
    linx_ui_init(lv_screen_active(), 0);
    linx_ui_tick(800);
    assert(linx_ui_page() == LINX_UI_TASKS);
    fill_labels();
    render_tasks();
    check_full_labels();

    /* 通过真实公开按键进入已有的设计任务，不伪造 task/turn 或演示内容。 */
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && ui.at.task == LINX_TASK_DESIGN);
    fill_labels();
    header("标题", "上下文");
    check_full_labels();
    fill_labels();
    render_overview();
    check_full_labels();
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_READER);
    fill_labels();
    render_reader();
    check_full_labels();

    linx_ui_handle_key(LINX_UI_KEY_OK_LONG);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_HISTORY && ui.history_count > 0);
    fill_labels();
    render_history();
    check_full_labels();

    linx_ui_handle_key(LINX_UI_KEY_OK_LONG);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_IMAGES);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    linx_ui_handle_key(LINX_UI_KEY_UP);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_IMAGE_DETAIL);
    check_reset();
    for (size_t i = 0; i < BOX_COUNT; ++i) assert(box(1, 2, 3, 4, COLOR_PAPER, 0) != NULL);
    render_image_detail();
    assert(ui.boxes_used == BOX_COUNT && ui.render_overflow);
    assert(!lv_obj_has_flag(ui.image, LV_OBJ_FLAG_HIDDEN));
    assert(lv_mem_test() == LV_RESULT_OK);
}

static linx_voice_request_t boundary_voice_requests[8];
static size_t boundary_voice_count;

static bool boundary_voice_handler(const linx_voice_request_t *request, void *user)
{
    assert(user == &boundary_voice_count && request != NULL);
    assert(boundary_voice_count < sizeof(boundary_voice_requests) / sizeof(boundary_voice_requests[0]));
    boundary_voice_requests[boundary_voice_count++] = *request;
    return true;
}

static void test_voice_sequence_and_private_guards(void)
{
    linx_ui_init(lv_screen_active(), 0U);
    linx_ui_tick(800U);
    assert(!voice_request(LINX_VOICE_START));
    boundary_voice_count = 0U;
    linx_ui_set_voice_handler(boundary_voice_handler, &boundary_voice_count);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && linx_ui_debug().selected == 1U);

    /* 只把编号计数器放到实际溢出边界；任务、上下文和页面均由真实按键建立。 */
    voice_sequence = UINT32_MAX - 1U;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(boundary_voice_count == 1U && ui.voice_generation == UINT32_MAX);
    assert(boundary_voice_requests[0].command == LINX_VOICE_START);
    linx_reply_context_t original = boundary_voice_requests[0].context;
    linx_ui_handle_key(LINX_UI_KEY_OK_LONG);
    assert(boundary_voice_count == 2U && boundary_voice_requests[1].command == LINX_VOICE_CANCEL);
    assert(boundary_voice_requests[1].generation == UINT32_MAX);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(boundary_voice_count == 3U && boundary_voice_requests[2].command == LINX_VOICE_START);
    assert(ui.voice_generation == 1U && boundary_voice_requests[2].generation == 1U);
    assert(memcmp(&original, &boundary_voice_requests[2].context, sizeof(original)) == 0);
    linx_voice_event_t stale = { .generation = UINT32_MAX, .state = LINX_VOICE_LISTENING };
    assert(!linx_ui_voice_event(&stale));

    /* 无法从公开事件进入的非法枚举仍有稳定默认文案。 */
    ui.voice_state = (linx_voice_state_t)-1;
    assert(strcmp(voice_status(), "正在连接灵矽") == 0);
    ui.voice_state = LINX_VOICE_CONNECTING;
    cancel_live_voice();
    assert(boundary_voice_count == 4U && ui.voice_generation == 0U);
    linx_voice_event_t zero = { .generation = 0U, .state = LINX_VOICE_LISTENING };
    assert(!linx_ui_voice_event(&zero));
    cancel_live_voice();
    assert(boundary_voice_count == 4U);
    linx_ui_set_voice_handler(NULL, NULL);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && lv_mem_test() == LV_RESULT_OK);
}

static void assert_phone_credentials_erased(void)
{
    for (size_t i = 0U; i < sizeof(ui.wifi_ap_password); ++i) assert(ui.wifi_ap_password[i] == '\0');
    for (size_t i = 0U; i < sizeof(ui.wifi_ap_ssid); ++i) assert(ui.wifi_ap_ssid[i] == '\0');
}

static void boundary_enter_phone_portal(void)
{
    linx_ui_init(lv_screen_active(), 0U);
    linx_ui_tick(800U);
    boundary_voice_count = 0U;
    linx_ui_set_voice_handler(boundary_voice_handler, &boundary_voice_count);
    linx_ui_handle_key(LINX_UI_KEY_OK_LONG);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_WIFI && ui.wifi_mode == LINX_UI_WIFI_SCANNING);
    linx_voice_event_t portal = { .generation = ui.voice_generation, .state = LINX_VOICE_WIFI_PORTAL,
                                 .wifi_ap_ssid = "LinxBox-Setup", .wifi_ap_password = "01234567" };
    assert(linx_ui_voice_event(&portal));
    assert(ui.wifi_mode == LINX_UI_WIFI_PHONE && strcmp(ui.wifi_ap_password, "01234567") == 0);
}

static void test_phone_secret_and_render_boundaries(void)
{
    /* 致命错误与重开、退出、保存、替换后端和清理均擦除整个凭据缓冲。 */
    boundary_enter_phone_portal();
    linx_voice_event_t failure = { .generation = ui.voice_generation, .state = LINX_VOICE_ERROR,
                                  .error = LINX_VOICE_NETWORK_FAILED };
    assert(linx_ui_voice_event(&failure));
    assert_phone_credentials_erased();
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.wifi_mode == LINX_UI_WIFI_SCANNING);
    assert_phone_credentials_erased();
    boundary_enter_phone_portal();
    linx_ui_handle_key(LINX_UI_KEY_OK_LONG);
    assert(ui.at.page == LINX_UI_SETTINGS);
    assert_phone_credentials_erased();
    boundary_enter_phone_portal();
    linx_voice_event_t saved = { .generation = ui.voice_generation, .state = LINX_VOICE_WIFI_SAVED };
    assert(linx_ui_voice_event(&saved));
    assert(ui.wifi_mode == LINX_UI_WIFI_CONNECTED);
    assert_phone_credentials_erased();
    boundary_enter_phone_portal();
    linx_ui_set_voice_handler(NULL, NULL);
    assert(ui.at.page == LINX_UI_SETTINGS);
    assert_phone_credentials_erased();
    boundary_enter_phone_portal();
    linx_ui_deinit();
    assert_phone_credentials_erased();

    /* 语音页进入配网并成功，旧热点凭据不能随原上下文恢复而残留。 */
    linx_ui_init(lv_screen_active(), 0U);
    linx_ui_tick(800U);
    boundary_voice_count = 0U;
    linx_ui_set_voice_handler(boundary_voice_handler, &boundary_voice_count);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    linx_voice_event_t unconfigured = { .generation = ui.voice_generation, .state = LINX_VOICE_ERROR,
                                       .error = LINX_VOICE_NOT_CONFIGURED };
    assert(linx_ui_voice_event(&unconfigured));
    linx_ui_handle_key(LINX_UI_KEY_OK);
    linx_voice_event_t portal = { .generation = ui.voice_generation, .state = LINX_VOICE_WIFI_PORTAL,
                                 .wifi_ap_ssid = "LinxBox-Setup", .wifi_ap_password = "01234567" };
    assert(linx_ui_voice_event(&portal));
    saved.generation = ui.voice_generation;
    assert(linx_ui_voice_event(&saved));
    assert(ui.at.page == LINX_UI_VOICE && ui.voice_state == LINX_VOICE_CONNECTING);
    assert_phone_credentials_erased();

    /* 真实热点页面绘制器在池满时保留已有对象，SSID 省略分支不解引用空指针。 */
    boundary_enter_phone_portal();
    fill_labels();
    render_wifi();
    check_full_labels();
    linx_ui_deinit();
    linx_ui_init(lv_screen_active(), 0U);
    voice_sequence = UINT32_MAX;
    wifi_setup();
    assert(ui.voice_generation == 1U && ui.wifi_mode == LINX_UI_WIFI_ERROR &&
           ui.voice_error == LINX_VOICE_NO_MEMORY);
    assert_phone_credentials_erased();
    puts("手机配网私有边界：凭据全量擦除、语音恢复、绘制容量及无后端/编号回绕通过");
}

int main(void)
{
    assert(LV_MEM_SIZE == 40U * 1024U);
    lv_init();
    display = lv_display_create(WIDTH, HEIGHT);
    assert(display != NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    linx_ui_init(lv_screen_active(), 0);
    test_label_capacity();
    test_box_capacity();
    test_decoration_capacity();
    test_utf8_and_width_bounds();
    test_footer_truncation();
    test_stack_capacity();
    test_renderers_handle_exhausted_pools();
    test_voice_sequence_and_private_guards();
    test_phone_secret_and_render_boundaries();
    linx_ui_deinit();
    render();
    assert(ui.root == NULL && lv_mem_test() == LV_RESULT_OK);
    lv_display_delete(display);
    lv_deinit();
    puts("UI 私有边界通过：对象与绘制容量、复用、UTF-8 窗口、字体测宽、页脚截断和栈保护");
    return 0;
}
