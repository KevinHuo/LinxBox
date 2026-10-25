/* 私有绘制器的容量与输入边界；使用真实实现和 LVGL，不改写演示模型。 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
    assert(lv_obj_get_child_count(ui.root) == LABEL_COUNT + BOX_COUNT + 1);
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
    assert(lv_obj_get_child_count(ui.root) == LABEL_COUNT + BOX_COUNT + 1);
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
    assert(ui.decor == decor && lv_mem_test() == LV_RESULT_OK);
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
    footer("OK确认\n双击OK返回");
    assert(strcmp(lv_label_get_text(ui.labels[0]), "OK确认") == 0);
    const char *instructions[] = {"设备设置", "演示选项", "取消", "放弃", "返回"};
    const char *actions[] = {"设置", "选项", "取消", "放弃", "返回"};
    for (unsigned mode = LINX_BACK_LONG_PRESS; mode <= LINX_BACK_DOUBLE_CLICK; ++mode) {
        linx_ui_set_back_mode((linx_back_mode_t)mode);
        for (size_t i = 0U; i < sizeof(instructions) / sizeof(instructions[0]); ++i) {
            check_reset();
            footer(instructions[i]);
            char expected[24];
            snprintf(expected, sizeof(expected), "%s%s", mode == LINX_BACK_LONG_PRESS ? "长按" : "双击", actions[i]);
            assert(strcmp(lv_label_get_text(ui.labels[1]), expected) == 0);
        }
    }
    linx_ui_set_back_mode(LINX_BACK_LONG_PRESS);
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

    linx_ui_handle_key(LINX_UI_KEY_BACK);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_HISTORY && ui.history_count > 0);
    fill_labels();
    render_history();
    check_full_labels();

    linx_ui_handle_key(LINX_UI_KEY_BACK);
    linx_ui_handle_key(LINX_UI_KEY_UP);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(linx_ui_page() == LINX_UI_VOICE && ui.reply.image_id == 0U);
    fill_labels();
    render_voice();
    check_full_labels();
    check_reset();
    for (size_t i = 0; i < BOX_COUNT; ++i) assert(box(1, 2, 3, 4, COLOR_PAPER, 0) != NULL);
    render_voice();
    assert(ui.boxes_used == BOX_COUNT && ui.render_overflow);
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
    assert(boundary_voice_count == 1U && boundary_voice_requests[0].command == LINX_VOICE_PREPARE &&
           boundary_voice_requests[0].generation != 0U && ui.voice_generation == 0U);

    /* 只把编号计数器放到实际溢出边界；任务、上下文和页面均由真实按键建立。 */
    voice_sequence = UINT32_MAX - 1U;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(boundary_voice_count == 2U && ui.voice_generation == UINT32_MAX);
    assert(boundary_voice_requests[1].command == LINX_VOICE_START);
    linx_reply_context_t original = boundary_voice_requests[1].context;
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(boundary_voice_count == 4U && boundary_voice_requests[2].command == LINX_VOICE_CANCEL);
    assert(boundary_voice_requests[2].generation == UINT32_MAX);
    assert(boundary_voice_requests[3].command == LINX_VOICE_PREPARE &&
           boundary_voice_requests[3].generation == 1U && ui.voice_generation == 0U);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(boundary_voice_count == 5U && boundary_voice_requests[4].command == LINX_VOICE_START);
    assert(ui.voice_generation == 2U && boundary_voice_requests[4].generation == 2U);
    assert(memcmp(&original, &boundary_voice_requests[4].context, sizeof(original)) == 0);
    linx_voice_event_t stale = { .generation = UINT32_MAX, .state = LINX_VOICE_LISTENING };
    assert(!linx_ui_voice_event(&stale));

    /* 无法从公开事件进入的非法枚举仍有稳定默认文案。 */
    ui.voice_state = (linx_voice_state_t)-1;
    assert(strcmp(voice_status(), "正在准备录音") == 0);
    ui.voice_state = LINX_VOICE_CONNECTING;
    cancel_live_voice(LINX_VOICE_CANCEL);
    assert(boundary_voice_count == 6U && ui.voice_generation == 0U);
    linx_voice_event_t zero = { .generation = 0U, .state = LINX_VOICE_LISTENING };
    assert(!linx_ui_voice_event(&zero));
    cancel_live_voice(LINX_VOICE_CANCEL);
    assert(boundary_voice_count == 6U);
    linx_ui_set_voice_handler(NULL, NULL);
    assert(linx_ui_page() == LINX_UI_OVERVIEW && lv_mem_test() == LV_RESULT_OK);

    /* 普通返回后保留准备所有权，真正销毁时仍能取消同一连接。 */
    boundary_voice_count = 0U;
    linx_ui_set_voice_handler(boundary_voice_handler, &boundary_voice_count);
    assert(boundary_voice_count == 1U && boundary_voice_requests[0].command == LINX_VOICE_PREPARE);
    uint32_t prepared = boundary_voice_requests[0].generation;
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(ui.at.page == LINX_UI_TASKS && ui.connection_generation == prepared && boundary_voice_count == 1U);
    linx_ui_deinit();
    assert(boundary_voice_count == 2U && boundary_voice_requests[1].command == LINX_VOICE_RELEASE &&
           boundary_voice_requests[1].generation == prepared && lv_mem_test() == LV_RESULT_OK);
}

static void test_soft_cancel_without_overview_retains_owner(void)
{
    linx_ui_init(lv_screen_active(), 0U);
    linx_ui_tick(800U);
    boundary_voice_count = 0U;
    linx_ui_set_voice_handler(boundary_voice_handler, &boundary_voice_count);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    location_t tasks_origin = ui.at;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    uint32_t owner = ui.voice_generation;
    assert(owner != 0U && ui.connection_generation == owner &&
           boundary_voice_count == 2U && boundary_voice_requests[1].command == LINX_VOICE_START);

    /* 当前公开入口只从概览录入；内部恢复到其他原页面也必须保留实际连接所有者。 */
    ui.reply_origin = tasks_origin;
    ui.reply_depth = 0U;
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(ui.at.page == LINX_UI_TASKS && ui.depth == 0U && ui.voice_generation == 0U &&
           ui.connection_generation == owner && boundary_voice_count == 3U);
    assert(boundary_voice_requests[2].command == LINX_VOICE_CANCEL &&
           boundary_voice_requests[2].generation == owner);
    linx_ui_deinit();
    assert(boundary_voice_count == 4U && boundary_voice_requests[3].command == LINX_VOICE_RELEASE &&
           boundary_voice_requests[3].generation == owner && lv_mem_test() == LV_RESULT_OK);
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
    linx_ui_handle_key(LINX_UI_KEY_BACK);
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
    linx_ui_handle_key(LINX_UI_KEY_BACK);
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
    assert(ui.at.page == LINX_UI_VOICE && ui.voice_state == LINX_VOICE_PREPARING);
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


static linx_gateway_client_t remote;
static unsigned gateway_sends, gateway_captures, gateway_cancels, gateway_finishes, gateway_setups;
static unsigned gateway_confirms;
static unsigned gateway_prepares, gateway_request_cancels;
static linx_voice_request_t gateway_last_prepare, gateway_last_start;
static linx_voice_request_t gateway_last_release;
static unsigned gateway_releases;
static bool reject_capture;
static bool reject_finish;
static char gateway_last_send[4096];

static bool gateway_send(uint32_t generation, const char *json, size_t size, void *user)
{
    (void)generation; (void)user;
    assert(json && size);
    assert(size < sizeof(gateway_last_send));
    memcpy(gateway_last_send, json, size);
    gateway_last_send[size] = '\0';
    ++gateway_sends;
    gateway_confirms += strstr(json, "\"type\":\"request.confirm\"") != NULL;
    gateway_request_cancels += strstr(json, "\"type\":\"request.cancel\"") != NULL;
    assert(!strstr(json, "request.propose"));
    return true;
}

static bool gateway_voice(const linx_voice_request_t *request, void *user)
{
    (void)user;
    gateway_cancels += request->command == LINX_VOICE_CANCEL;
    gateway_finishes += request->command == LINX_VOICE_FINISH;
    if (request->command == LINX_VOICE_FINISH) return !reject_finish;
    gateway_setups += request->command == LINX_VOICE_WIFI_SETUP;
    if (request->command == LINX_VOICE_PREPARE) { ++gateway_prepares; gateway_last_prepare = *request; }
    if (request->command == LINX_VOICE_RELEASE) { ++gateway_releases; gateway_last_release = *request; }
    if (request->command == LINX_VOICE_START) {
        ++gateway_captures;
        assert(strcmp(request->gateway_request_id, remote.view.request.valid ? remote.view.request.request_id : "") == 0);
        assert(strcmp(request->gateway_agent_session_id, remote.view.request.valid ? remote.view.request.agent_session_id : "") == 0);
        gateway_last_start = *request;
        return !reject_capture;
    }
    return true;
}

static void remote_setup(void)
{
    linx_ui_deinit();
    linx_ui_init(lv_screen_active(), 0U);
    const linx_gateway_client_config_t config = {.configured = true, .instance_id = 1U, .send = gateway_send};
    linx_gateway_client_deinit(&remote);
    linx_gateway_client_init(&remote, &config);
    remote.view.connected = remote.view.bound = true;
    strcpy(remote.view.device_id, "AA:BB:CC:DD:EE:FF");
    strcpy(remote.view.selected_session_id, "session-真实");
    strcpy(remote.view.detail.agent_session_id, "session-真实");
    strcpy(remote.view.detail.title, "量子鹦鹉检查工作台");
    strcpy(remote.view.detail.project_name, "星际仓库");
    strcpy(remote.view.detail.agent, "Codex");
    strcpy(remote.view.detail.status, "running");
    remote.view.detail_valid = true;
    linx_ui_set_voice_handler(gateway_voice, NULL);
    linx_ui_set_gateway_client(&remote);
}

static bool has_remote_label(const char *value)
{
    for (size_t i = 0U; i < ui.labels_used; ++i)
        if (strcmp(lv_label_get_text(ui.labels[i]), value) == 0) return true;
    return false;
}

static void capture_session_status(const char *native, const char *last, bool detail)
{
    const char *directory = getenv("LINX_STATUS_SCREEN_DIR");
    if (!directory) return;
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s-%s-%s.ppm", directory,
        detail ? "detail" : "list", native, *last ? last : "legacy");
    assert(length > 0 && (size_t)length < sizeof(path));
    lv_obj_invalidate(ui.root); lv_refr_now(display);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; ++y) for (int x = 0; x < WIDTH; ++x) {
        uint16_t pixel = frame[y][x];
        const unsigned char rgb[] = {(unsigned char)(((pixel >> 11) & 31U) * 255U / 31U),
            (unsigned char)(((pixel >> 5) & 63U) * 255U / 63U), (unsigned char)((pixel & 31U) * 255U / 31U)};
        assert(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
    }
    assert(fclose(file) == 0);
}

static void assert_status_label_fits(const char *expected)
{
    lv_obj_update_layout(ui.root);
    bool found = false;
    for (size_t i = 0; i < ui.labels_used; ++i) {
        lv_obj_t *label = ui.labels[i];
        if (strcmp(lv_label_get_text(label), expected) != 0) continue;
        found = true;
        assert(lv_obj_get_style_text_font(label, LV_PART_MAIN) == &linx_font_remote_16);
        assert(lv_obj_get_width(label) == 68 && lv_obj_get_height(label) == 20);
        lv_point_t natural;
        lv_text_get_size(&natural, expected, &linx_font_remote_16, 0, 0, 1000, LV_TEXT_FLAG_NONE);
        assert(natural.x <= 68 && natural.y <= 20);
        lv_area_t area; lv_obj_get_coords(label, &area);
        assert(area.x1 == 145 && area.x2 == 212 && area.y2 < 270);
    }
    assert(found && !ui.render_overflow && lv_mem_test() == LV_RESULT_OK);
}

static void test_remote_live_and_saved_status(void)
{
    remote_setup();
    const char *states[] = {"idle", "running", "waiting", "completed", "failed", "unknown", "invalid"};
    const char *labels[] = {"待命", "运行中", "状态未知", "已完成", "失败", "状态未知", "状态未知"};
    for (unsigned i = 0; i < 7U; ++i) {
        strcpy(remote.view.detail.status, states[i]);
        remote.view.sessions[0] = remote.view.detail; remote.view.session_count = 1U;
        ui.at.page = LINX_UI_TASKS; render(); assert_status_label_fits(labels[i]);
        ui.at.page = LINX_UI_OVERVIEW; render(); assert_status_label_fits(labels[i]);
    }
    assert(!strcmp(gateway_session_status(NULL), "状态未知"));
}

static void test_remote_project_context(void)
{
    remote_setup();
    remote.view.session_count = 2U;
    for (size_t i = 0U; i < 2U; ++i) {
        remote.view.sessions[i] = remote.view.detail;
        snprintf(remote.view.sessions[i].agent_session_id, LINX_GATEWAY_ID_CAPACITY, "project-session-%zu", i);
    }
    strcpy(remote.view.sessions[0].project_name, "LinxBox");
    strcpy(remote.view.sessions[1].project_name, "演示项目");
    strcpy(remote.view.sessions[1].status, "completed");
    render();
    lv_obj_update_layout(ui.root);
    assert(has_remote_label("LinxBox") && has_remote_label("演示项目"));
    assert(has_remote_label("运行中") && has_remote_label("已完成"));
    assert(!ui.render_overflow);

    for (size_t i = 0U; i < 510U; i += 3U) memcpy(remote.view.sessions[0].project_name + i, "鹦", 3U);
    remote.view.sessions[0].project_name[510] = '\0';
    strcpy(remote.view.sessions[0].status, "unknown");
    render();
    lv_obj_update_layout(ui.root);
    assert(strlen(remote.view.sessions[0].project_name) == 510U);
    assert(has_remote_label("状态未知") && !ui.render_overflow);

    ui.at.page = LINX_UI_HISTORY;
    strcpy(remote.view.history_session_id, remote.view.detail.agent_session_id);
    render();
    assert(has_remote_label(remote.view.detail.project_name));
    assert(!has_remote_label(gateway_connection()));
    remote.view.connected = false; render();
    assert(has_remote_label(gateway_connection()));
    remote.view.connected = true; remote.view.bound = false; render();
    assert(has_remote_label(gateway_connection()));
    remote.view.bound = true; remote.view.error = LINX_GATEWAY_ERROR_MEMORY; render();
    assert(has_remote_label(gateway_connection()));
    remote.view.error = LINX_GATEWAY_ERROR_NONE; remote.view.detail_valid = false; render();
    assert(has_remote_label(gateway_connection()));
    remote.view.detail_valid = true;
    strcpy(remote.view.history_session_id, "other-project-session"); render();
    assert(!has_remote_label(remote.view.detail.project_name));
    assert(has_remote_label(gateway_connection()));
    puts("工程归属：同名任务分别显示真实工程与状态，长名称不挤占状态，历史页不误用其他工程且保留连接错误通过");
}

static void test_remote_render_and_navigation(void)
{
    remote_setup();
    assert(linx_ui_debug().gateway_online);
    assert(strcmp(gateway_connection(), "工作电脑 · 已连接") == 0);
    remote.view.configured = false; render();
    assert(strcmp(gateway_connection(), "网关未配置") == 0);
    remote.view.configured = true; remote.view.connected = false; render();
    remote.view.error = LINX_GATEWAY_ERROR_MEMORY; render();
    assert(strstr(gateway_connection(), "连接失败"));
    remote.view.bound = false; render();
    strcpy(remote.view.error_message, "内存不足"); render();
    remote.view.error = LINX_GATEWAY_ERROR_DISCONNECTED; render();
    remote.view.error = LINX_GATEWAY_ERROR_NONE;
    remote.view.bound = true;
    remote.view.session_count = 1U; render();
    assert(strstr(gateway_connection(), "快照"));
    remote.view.connected = true; remote.view.bound = false; render();
    assert(strstr(gateway_connection(), "尚未绑定"));
    remote.view.bound = true; remote.view.error = LINX_GATEWAY_ERROR_REMOTE;
    strcpy(remote.view.error_message, "服务正在恢复"); render();
    assert(strstr(gateway_connection(), "服务错误"));
    remote.view.error = LINX_GATEWAY_ERROR_NONE; remote.view.loading = true; render();
    assert(strcmp(gateway_connection(), "正在加载") == 0);
    remote.view.loading = false; remote.view.session_count = 0U; render();
    for (size_t i = 0U; i < LINX_GATEWAY_SESSION_MAX; ++i) {
        remote.view.sessions[i] = remote.view.detail;
        snprintf(remote.view.sessions[i].agent_session_id, LINX_GATEWAY_ID_CAPACITY, "session-%zu", i);
    }
    remote.view.session_count = LINX_GATEWAY_SESSION_MAX;
    strcpy(remote.view.sessions_next_cursor, "next-cursor");
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_tick(1U);
    assert(ui.at.selected == 1U);
    for (unsigned i = 1U; i < LINX_GATEWAY_SESSION_MAX; ++i) linx_ui_handle_key(LINX_UI_KEY_DOWN);
    assert(ui.at.selected == LINX_GATEWAY_SESSION_MAX);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(remote.sessions_depth == 1U);
    remote.pending.operation = 0U; remote.view.loading = false;
    ui.at.selected = LINX_GATEWAY_SESSION_MAX; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(remote.sessions_depth == 0U);
    remote.pending.operation = 0U; remote.view.loading = false;
    ui.at.selected = 0U; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_OVERVIEW);
    remote.view.target_unavailable = true; render();
    remote.view.target_unavailable = false; remote.view.detail_valid = false; render();
    remote.view.detail_valid = true; remote.view.new_reply = true; render();
    ui.at.selected = 2U; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_HISTORY);
    remote.pending.operation = 0U; remote.view.loading = false;
    for (size_t i = 0U; i < LINX_GATEWAY_HISTORY_MAX; ++i) {
        snprintf(remote.view.messages[i].message_id, LINX_GATEWAY_ID_CAPACITY, "message-%zu", i);
        strcpy(remote.view.messages[i].role, i & 1U ? "user" : "assistant");
    }
    remote.view.message_count = LINX_GATEWAY_HISTORY_MAX;
    render();
    bool assistant_label = false, user_label = false;
    for (size_t i = 0U; i < ui.labels_used; ++i) {
        assistant_label |= strcmp(lv_label_get_text(ui.labels[i]), "AI回复") == 0;
        user_label |= strcmp(lv_label_get_text(ui.labels[i]), "我的指令") == 0;
    }
    assert(assistant_label && user_label);
    assert(strcmp(remote.view.messages[0].role, "assistant") == 0);
    assert(strcmp(remote.view.messages[1].role, "user") == 0);
    strcpy(remote.view.history_next_cursor, "older");
    for (unsigned i = 0U; i < LINX_GATEWAY_HISTORY_MAX; ++i) linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(remote.history_depth == 1U);
    remote.pending.operation = 0U; remote.view.loading = false;
    ui.at.selected = LINX_GATEWAY_HISTORY_MAX; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(remote.history_depth == 0U);
    remote.pending.operation = 0U; remote.view.loading = false;
    ui.at.selected = 1U; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_READER && strcmp(ui.gateway_message, "message-1") == 0);
    assert(gateway_captures == 0U);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(gateway_captures == 0U);
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(ui.at.page == LINX_UI_TASKS);
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_SETTINGS);
    linx_ui_handle_key(LINX_UI_KEY_DOWN); linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_ABOUT);
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    ui.at.selected = 0U; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_WIFI);
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    remote.view.message_count = 0U; ui.at.page = LINX_UI_HISTORY; render();
    remote.view.history_next_cursor[0] = '\0'; remote.history_depth = 0U; render();
    ui.at.page = LINX_UI_BOOT; render(); linx_ui_handle_key(LINX_UI_KEY_BACK);
    ui.at.page = LINX_UI_BOOT; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_TASKS);
    assert(remote_measure("𠀀鹦A", strlen("𠀀鹦A"), LINX_READER_HEADING, NULL) > 0);
    lv_obj_t *fallback = remote_text("𠀀鹦", 16, 120, 208, 22, COLOR_INK);
    assert(fallback && strcmp(lv_label_get_text(fallback), "�鹦") == 0);
    remote.view.bound = false; ui.at.selected = 10U;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    linx_ui_set_gateway_client(NULL);
    assert(!linx_ui_debug().gateway_online);
}

static void test_remote_body_and_full_draft(void)
{
    remote_setup();
    ui.at.page = LINX_UI_READER;
    strcpy(ui.gateway_message, "body");
    strcpy(remote.view.content_message_id, "body");
    strcpy(remote.view.content_session_id, remote.view.selected_session_id);
    remote.view.content_valid = true;
    for (size_t i = 0U; i < 900U; i += 3U) memcpy(remote.view.content + i, "鹦", 3U);
    remote.view.content[900] = '\0';
    remote.view.content_has_next = true; remote.view.content_next_offset = 900U;
    remote.block_count = 1U;
    render(); assert(ui.pages.count > 1U);
    linx_ui_handle_key(LINX_UI_KEY_DOWN); assert(ui.at.read_page == 1U);
    linx_ui_handle_key(LINX_UI_KEY_UP); assert(ui.at.read_page == 0U);
    ui.at.read_page = ui.pages.count - 1U;
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    size_t old_page = ui.at.read_page;
    assert(remote.view.loading && old_page > 0U);
    remote.view.new_reply = true; ++remote.view.revision; linx_ui_tick(2U);
    assert(ui.at.read_page == old_page && remote.view.content_offset == 0U);
    remote.pending.operation = 0U; remote.view.loading = false;
    remote.view.content_offset = 900U; remote.block_offsets[1] = 900U;
    remote.block_count = 2U; remote.block_index = 1U;
    ++remote.view.revision; linx_ui_tick(3U);
    assert(ui.at.read_page == 0U);
    linx_ui_handle_key(LINX_UI_KEY_UP);
    assert(remote.view.loading && ui.at.read_page == 0U);
    remote.pending.operation = 0U; remote.view.loading = false; remote.view.content_offset = 0U;
    remote.block_index = 0U; ++remote.view.revision; linx_ui_tick(4U);
    assert(ui.at.read_page == ui.pages.count - 1U);
    remote.view.content_valid = false; remote.view.loading = true; render();
    remote.view.loading = false; remote.view.error = LINX_GATEWAY_ERROR_REMOTE; render();
    remote.view.error = LINX_GATEWAY_ERROR_NONE; render();
    puts("真实Gateway正文跨块等待与后台刷新通过");
}


static void test_remote_user_intents(void)
{
    remote_setup();
    ui.at.page = LINX_UI_OVERVIEW;
    linx_ui_handle_key(LINX_UI_KEY_UP);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    ui.at.selected = 1U;
    unsigned captures = gateway_captures;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_VOICE && remote.view.request.valid && gateway_captures == captures);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(ui.at.page == LINX_UI_OVERVIEW && remote.view.request.cancel_requested);
    assert(gateway_captures == captures);
    for (int status = LINX_GATEWAY_REQUEST_AWAITING_TEXT; status <= LINX_GATEWAY_REQUEST_UNKNOWN; ++status) {
        remote.pending.operation = 0U; remote.view.loading = false;
        remote.view.request.valid = true; remote.view.request.status = (linx_gateway_request_status_t)status;
        ui.at.page = LINX_UI_OVERVIEW; ui.at.selected = 1U;
        linx_ui_handle_key(LINX_UI_KEY_OK);
        if (status == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION) assert(ui.at.page == LINX_UI_RECEIPT);
        else if (status == LINX_GATEWAY_REQUEST_COMPLETED || status == LINX_GATEWAY_REQUEST_FAILED || status == LINX_GATEWAY_REQUEST_CANCELLED)
            assert(ui.at.page == LINX_UI_VOICE);
        else assert(ui.at.page == LINX_UI_RECEIPT);
    }
    remote.view.request.valid = false;
    remote.pending.operation = 0U; remote.view.loading = false;
    remote.view.target_unavailable = true; ui.at.page = LINX_UI_OVERVIEW; ui.at.selected = 1U;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_OVERVIEW);
    remote.view.target_unavailable = false; remote.view.error = LINX_GATEWAY_ERROR_NONE;
    ui.at.selected = 0U; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_HISTORY && ui.read_latest);
    linx_ui_tick(1U); assert(ui.at.page == LINX_UI_HISTORY);
    remote.pending.operation = 0U; remote.view.loading = false;
    strcpy(remote.view.history_session_id, remote.view.selected_session_id);
    remote.view.message_count = 2U;
    strcpy(remote.view.messages[0].message_id, "first-user"); strcpy(remote.view.messages[0].role, "user");
    strcpy(remote.view.messages[1].message_id, "latest-assistant"); strcpy(remote.view.messages[1].role, "assistant");
    ++remote.view.revision; linx_ui_tick(2U);
    assert(ui.at.page == LINX_UI_READER && !strcmp(ui.gateway_message, "latest-assistant"));
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(ui.at.page == LINX_UI_OVERVIEW);
    ui.at.selected = 2U;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_HISTORY);
    assert(!remote.view.message_count && !remote.view.content_valid);
    remote.view.message_count = 2U;
    strcpy(remote.view.messages[0].message_id, "first-user"); strcpy(remote.view.messages[0].role, "user");
    strcpy(remote.view.messages[1].message_id, "latest-assistant"); strcpy(remote.view.messages[1].role, "assistant");
    ui.at.selected = 1U;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_READER);
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(ui.at.page == LINX_UI_HISTORY);
    strcpy(ui.gateway_history, "latest-assistant");
    linx_gateway_message_t swap = remote.view.messages[0];
    remote.view.messages[0] = remote.view.messages[1]; remote.view.messages[1] = swap;
    ++remote.view.revision; linx_ui_tick(3U);
    assert(ui.at.selected == 0U);
    strcpy(ui.gateway_history, "missing"); ++remote.view.revision; linx_ui_tick(4U);
    remote.view.message_count = 0U; remote.view.loading = true; render();
    remote.view.loading = false; remote.view.error = LINX_GATEWAY_ERROR_REMOTE; render();
    remote.view.error = LINX_GATEWAY_ERROR_NONE; render();
    ui.at.page = LINX_UI_TASKS; remote.view.bound = false; remote.view.configured = false; render();
    remote.view.connected = false; render();
    ui.at.page = LINX_UI_HISTORY; ui.at.selected = 19U; linx_ui_handle_key(LINX_UI_KEY_OK);
    ui.at.page = LINX_UI_SETTINGS; linx_ui_handle_key(LINX_UI_KEY_UP);
    ui.voice_handler = NULL; ui.at.selected = 0U; linx_ui_handle_key(LINX_UI_KEY_OK);
    ui.at.selected = 9U; linx_ui_handle_key(LINX_UI_KEY_OK);
    ui.at.page = LINX_UI_VOICE; remote.view.request.valid = true;
    remote.view.request.cancel_requested = true; render();
    ui.at.page = LINX_UI_READER; ui.at.read_page = 63U; render();
    linx_ui_handle_key(LINX_UI_KEY_UP);
    remote.view.content_valid = true; remote.view.loading = true; linx_ui_handle_key(LINX_UI_KEY_DOWN);
    ui.at.page = LINX_UI_CONFIRM; ui.at.read_page = 0U; render();
    linx_ui_handle_key(LINX_UI_KEY_UP);
    ui.at.read_page = ui.pages.count - 1U; linx_ui_handle_key(LINX_UI_KEY_DOWN);
    linx_ui_handle_key((linx_ui_key_t)99);
    check_reset();
    lv_obj_t *empty = remote_text(NULL, 16, 100, 208, 20, COLOR_INK);
    assert(empty && !lv_label_get_text(empty)[0]);
    assert(remote_codepoint('\n') == '\n');
    fill_labels();
    assert(remote_text("鹦𠀀", 16, 100, 208, 20, COLOR_INK) == NULL);
    assert(gateway_captures == captures);
}


static void test_remote_content_priority_during_refresh(void)
{
    remote_setup();
    ui.at.page = LINX_UI_READER;
    strcpy(ui.gateway_message, "reading-message");
    strcpy(remote.view.content_message_id, ui.gateway_message);
    strcpy(remote.view.content_session_id, remote.view.selected_session_id);
    for (size_t i = 0U; i < 750U; i += 15U) memcpy(remote.view.content + i, "正文内容。", 15U);
    remote.view.content[750] = '\0';
    remote.view.content_valid = remote.view.content_has_next = true;
    remote.view.content_next_offset = 750U;
    remote.block_count = 1U;
    ui.read_offset = 0U;
    render();
    assert(ui.pages.count > 1U);
    ui.at.read_page = ui.pages.count - 1U;
    const size_t last_page = ui.at.read_page;
    const unsigned before = gateway_sends;
    assert(gateway_action(LINX_GATEWAY_ACTION_REFRESH, NULL));
    assert(remote.view.loading && !linx_gateway_client_content_pending(&remote));
    assert(gateway_sends == before + 1U && strstr(gateway_last_send, "agent_sessions.list"));

    /* 后台目录在途也能登记前台翻块；继续按键不能重复登记同一个正文请求。 */
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    assert(linx_gateway_client_content_pending(&remote));
    assert(remote.intent.offset == 750U && gateway_sends == before + 1U);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    assert(remote.intent.offset == 750U && gateway_sends == before + 1U);
    assert(remote.view.content_offset == 0U && ui.at.read_page == last_page && strlen(remote.view.content) == 750U);

    char response[1024];
    int size = snprintf(response, sizeof(response),
        "{\"id\":\"refresh-response\",\"type\":\"response\",\"reply_to\":\"%s\","
        "\"payload\":{\"ok\":true,\"data\":{\"items\":[],\"next_cursor\":null}}}", remote.pending.id);
    assert(size > 0 && (size_t)size < sizeof(response));
    assert(linx_gateway_client_receive(&remote, remote.view.generation, response, (size_t)size, 1U));
    assert(gateway_sends == before + 2U && strstr(gateway_last_send, "message.content.get"));
    assert(remote.pending.offset == 750U && !strcmp(remote.pending.message, "reading-message"));
    assert(linx_gateway_client_content_pending(&remote));
    linx_ui_tick(1U);
    linx_ui_handle_key(LINX_UI_KEY_DOWN);
    assert(gateway_sends == before + 2U && remote.view.content_offset == 0U && ui.at.read_page == last_page);

    /* 唯有相同正文的新块成功应答才替换屏幕内容、偏移和块内页。 */
    size = snprintf(response, sizeof(response),
        "{\"id\":\"content-response\",\"type\":\"response\",\"reply_to\":\"%s\","
        "\"payload\":{\"ok\":true,\"data\":{\"text\":\"下一块\",\"next_offset\":null}}}", remote.pending.id);
    assert(size > 0 && (size_t)size < sizeof(response));
    assert(linx_gateway_client_receive(&remote, remote.view.generation, response, (size_t)size, 2U));
    assert(!linx_gateway_client_content_pending(&remote));
    linx_ui_tick(2U);
    assert(remote.view.content_offset == 750U && ui.at.read_page == 0U && !strcmp(remote.view.content, "下一块"));
    assert(!linx_gateway_client_content_pending(NULL));
    puts("真实 UI 正文公平调度：后台目录刷新不吞跨块键、正文请求不重复登记、应答前阅读位置不变通过");
}


static void setup_failed_gateway_voice(linx_voice_state_t state, linx_voice_error_t error)
{
    remote_setup();
    linx_gateway_request_t *request = &remote.view.request;
    request->valid = true;
    strcpy(request->request_id, "failed-voice-request");
    strcpy(request->agent_session_id, remote.view.selected_session_id);
    strcpy(request->target_title, remote.view.detail.title);
    request->status = LINX_GATEWAY_REQUEST_AWAITING_TEXT;
    remote.prepare_sent = true;
    ui.at.page = LINX_UI_OVERVIEW; ui.at.selected = 1U; push(LINX_UI_VOICE);
    ui.voice_generation = 42U; ui.voice_state = state; ui.voice_error = error;
    render();
}

static void test_gateway_voice_failure_returns_safely(void)
{
    const linx_voice_state_t states[] = {LINX_VOICE_ERROR, LINX_VOICE_CLOSED};
    for (unsigned i = 0; i < 2U; ++i) {
        setup_failed_gateway_voice(states[i], LINX_VOICE_NETWORK_FAILED);
        unsigned captures = gateway_captures, finishes = gateway_finishes, cancels = gateway_cancels;
        assert(has_remote_label("本轮已结束，按OK返回"));
        capture_session_status("voice", i ? "closed" : "error", false);
        for (size_t label = 0; label < ui.labels_used; ++label)
            assert(strstr(lv_label_get_text(ui.labels[label]), "重录") == NULL);
        linx_ui_handle_key(LINX_UI_KEY_OK);
        assert(ui.at.page == LINX_UI_OVERVIEW && ui.voice_generation == 0U);
        assert(remote.view.request.cancel_requested && remote.view.request.cancel_sent);
        assert(strstr(gateway_last_send, "request.cancel"));
        assert(gateway_cancels == cancels + 1U && gateway_captures == captures && gateway_finishes == finishes);
        unsigned sends = gateway_sends;
        linx_ui_handle_key(LINX_UI_KEY_OK);
        assert(ui.at.page == LINX_UI_RECEIPT && has_remote_label("正在核对取消结果"));
        linx_ui_handle_key(LINX_UI_KEY_OK);
        assert(gateway_sends == sends && gateway_captures == captures);
        char response[2048];
        int size = snprintf(response, sizeof(response),
            "{\"id\":\"cancelled\",\"type\":\"response\",\"reply_to\":\"%s\",\"payload\":{\"ok\":true,\"data\":{"
            "\"request_id\":\"failed-voice-request\",\"agent_session_id\":\"%s\",\"target_title\":\"%s\","
            "\"text\":null,\"status\":\"cancelled\",\"error\":null}}}",
            remote.pending.id, remote.view.request.agent_session_id, remote.view.request.target_title);
        assert(size > 0 && (size_t)size < sizeof(response));
        assert(linx_gateway_client_receive(&remote, remote.view.generation, response, (size_t)size, 1U));
        linx_ui_handle_key(LINX_UI_KEY_BACK);
        assert(ui.at.page == LINX_UI_OVERVIEW);
        linx_ui_handle_key(LINX_UI_KEY_OK);
        assert(ui.at.page == LINX_UI_VOICE && !strcmp(remote.view.request.agent_session_id, "session-真实"));
        assert(strcmp(remote.view.request.request_id, "failed-voice-request") != 0);
        assert(!remote.view.request.text_present);
        assert(strstr(gateway_last_send, "request.prepare") && gateway_captures == captures);
    }
    for (unsigned i = 0; i < 2U; ++i) {
        setup_failed_gateway_voice(i ? LINX_VOICE_SPEAKING : LINX_VOICE_WAITING, LINX_VOICE_OK);
        unsigned sends = gateway_sends, captures = gateway_captures, cancels = gateway_cancels, finishes = gateway_finishes;
        linx_ui_handle_key(LINX_UI_KEY_OK); linx_ui_handle_key(LINX_UI_KEY_OK);
        assert(ui.at.page == LINX_UI_VOICE && !remote.view.request.cancel_requested);
        assert(gateway_sends == sends && gateway_captures == captures && gateway_cancels == cancels && gateway_finishes == finishes);
    }
    const linx_gateway_request_status_t submitted[] = {LINX_GATEWAY_REQUEST_SENDING, LINX_GATEWAY_REQUEST_RUNNING, LINX_GATEWAY_REQUEST_UNKNOWN};
    for (unsigned i = 0; i < 3U; ++i) {
        setup_failed_gateway_voice(LINX_VOICE_ERROR, LINX_VOICE_NETWORK_FAILED);
        remote.view.request.status = submitted[i];
        unsigned sends = gateway_sends, captures = gateway_captures;
        linx_ui_handle_key(LINX_UI_KEY_OK);
        assert(ui.at.page == LINX_UI_OVERVIEW && !remote.view.request.cancel_requested);
        assert(remote.view.request.status == submitted[i]);
        linx_ui_handle_key(LINX_UI_KEY_OK);
        assert(ui.at.page == LINX_UI_RECEIPT && gateway_sends == sends && gateway_captures == captures);
    }
    setup_failed_gateway_voice(LINX_VOICE_ERROR, LINX_VOICE_NOT_CONFIGURED);
    unsigned captures = gateway_captures, setups = gateway_setups;
    assert(has_remote_label("本次未能完成，按OK配网"));
    capture_session_status("voice", "configure", false);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_WIFI && ui.wifi_origin.page == LINX_UI_OVERVIEW);
    assert(remote.view.request.cancel_requested && gateway_setups == setups + 1U);
    const linx_voice_event_t saved = {.generation = ui.voice_generation, .state = LINX_VOICE_WIFI_SAVED};
    assert(linx_ui_voice_event(&saved));
    assert(ui.at.page == LINX_UI_WIFI && ui.wifi_mode == LINX_UI_WIFI_CONNECTED && gateway_captures == captures);
    puts("Gateway录音失败：ERROR/CLOSED单击取消本条并返回，等待取消确认再prepare新编号；重复等待OK和已提交任务不取消不重投，配网成功不自动录音通过");
}

static void test_gateway_voice_pause_context(void)
{
    setup_failed_gateway_voice(LINX_VOICE_LISTENING, LINX_VOICE_OK);
    remote.view.request.capture_claimed = true;
    remote.view.connected = false; remote.view.bound = false;
    remote.view.error = LINX_GATEWAY_ERROR_DISCONNECTED;
    strcpy(remote.view.error_message, "连接已断开，保留上次数据");
    render();
    assert(has_remote_label("发送指令") && !has_remote_label(gateway_connection()));
    assert(!has_remote_label(remote.view.error_message) && !has_remote_label("请检查网络后重试"));
    capture_session_status("voice", "paused-listening", false);
    ui.voice_state = LINX_VOICE_WAITING; render();
    assert(has_remote_label("发送指令"));
    capture_session_status("voice", "paused-waiting", false);
    remote.view.error = LINX_GATEWAY_ERROR_NONE; render();
    assert(has_remote_label("发送指令"));
    remote.view.error = LINX_GATEWAY_ERROR_TRANSPORT; render();
    assert(has_remote_label("发送指令") && !has_remote_label(remote.view.error_message));
    remote.view.error = LINX_GATEWAY_ERROR_DISCONNECTED;
    ui.voice_state = LINX_VOICE_ERROR; ui.voice_error = LINX_VOICE_NETWORK_FAILED; render();
    assert(!has_remote_label("语音输入") && has_remote_label("本轮已结束，按OK返回"));
    ui.voice_state = LINX_VOICE_CLOSED; render(); assert(!has_remote_label("语音输入"));
    ui.voice_state = LINX_VOICE_LISTENING; remote.view.request.cancel_requested = true; render();
    assert(!has_remote_label("语音输入") && has_remote_label("正在核对取消结果"));
    remote.view.request.cancel_requested = false; remote.view.request.capture_claimed = false; render();
    assert(!has_remote_label("语音输入"));
    ui.voice_generation = 0U; render(); assert(!has_remote_label("语音输入"));
    puts("Gateway业务暂停：录音和等待原文使用语音输入语境，真实语音错误与取消仍显示，未授权录音不冒充在线通过");
}

static void setup_gateway_one_key_capture(void)
{
    remote_setup(); reject_capture = reject_finish = false;
    ui.at.page = LINX_UI_OVERVIEW; ui.at.selected = 1U;
    render();
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_VOICE);
    remote.pending.operation = 0U; remote.view.loading = false;
    remote.view.request.status = LINX_GATEWAY_REQUEST_AWAITING_TEXT;
    remote.view.request.capture_pending = true;
    ui.reply.task_id = 999U; /* Gateway 必须使用真实字符串目标，不夹带演示任务编号。 */
    ++remote.view.revision; linx_ui_tick(1U);
    assert(ui.at.page == LINX_UI_VOICE && ui.voice_generation);
    linx_voice_event_t event = {.generation = ui.voice_generation, .state = LINX_VOICE_LISTENING};
    assert(linx_ui_voice_event(&event));
}

static void test_gateway_recording_wave_and_timer(void)
{
    setup_failed_gateway_voice(LINX_VOICE_PREPARING, LINX_VOICE_OK);
    remote.view.request.capture_claimed = true; render();
    assert(ui.voice_counter && !strcmp(lv_label_get_text(ui.voice_counter), "00:00"));
    linx_ui_tick(10000U);
    assert(!strcmp(lv_label_get_text(ui.voice_counter), "00:00"));
    linx_voice_event_t event = {.generation = ui.voice_generation - 1U, .state = LINX_VOICE_LISTENING};
    assert(!linx_ui_voice_event(&event));
    event.generation = ui.voice_generation; assert(linx_ui_voice_event(&event));
    lv_obj_update_layout(ui.root);
    assert(ui.boxes_used == 6U && ui.voice_counter && ui.labels_used < LABEL_COUNT);
    assert(lv_obj_get_x(ui.voice_counter) == 79 && lv_obj_get_y(ui.voice_counter) == 210);
    assert(lv_obj_get_width(ui.voice_counter) == 90 && lv_obj_get_height(ui.voice_counter) == 36);
    assert(lv_obj_get_style_text_font(ui.voice_counter, LV_PART_MAIN) == &linx_font_28_bold);
    bool heading = false;
    for (size_t i = 0; i < ui.labels_used; ++i) if (!strcmp(lv_label_get_text(ui.labels[i]), "发送指令")) {
        heading = true;
        assert(lv_obj_get_style_text_font(ui.labels[i], LV_PART_MAIN) == &linx_font_24_bold);
        assert(lv_obj_get_x(ui.labels[i]) == 16 && lv_obj_get_y(ui.labels[i]) == 43);
    }
    assert(heading && lv_obj_get_x(ui.boxes[0]) == 42 && lv_obj_get_width(ui.boxes[0]) == 6);
    int first = lv_obj_get_height(ui.boxes[0]);
    size_t children = lv_obj_get_child_count(ui.root);
    linx_ui_tick(10124U); lv_obj_update_layout(ui.root);
    assert(lv_obj_get_height(ui.boxes[0]) == first);
    linx_ui_tick(10125U); lv_obj_update_layout(ui.root);
    assert(lv_obj_get_height(ui.boxes[0]) != first);
    linx_ui_tick(18000U);
    assert(!strcmp(lv_label_get_text(ui.voice_counter), "00:08"));
    assert(has_remote_label("发送指令") && has_remote_label(remote.view.request.target_title));
    assert(has_remote_label(remote.view.detail.project_name));
    capture_session_status("voice", "timer-eight", false);
    ++remote.view.revision; linx_ui_tick(18500U);
    assert(!strcmp(lv_label_get_text(ui.voice_counter), "00:08"));
    assert(linx_ui_voice_event(&event));
    linx_ui_tick(19000U);
    assert(!strcmp(lv_label_get_text(ui.voice_counter), "00:09"));
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.voice_state == LINX_VOICE_WAITING);
    linx_ui_tick(25000U); ++remote.view.revision; linx_ui_tick(26000U);
    assert(!strcmp(lv_label_get_text(ui.voice_counter), "00:09"));
    assert(!linx_ui_voice_event(&event)); /* 同代迟到 LISTENING 也不能重新开启本条。 */
    capture_session_status("voice", "timer-waiting", false);
    unsigned captures = gateway_captures, finishes = gateway_finishes;
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(ui.at.page == LINX_UI_OVERVIEW && !ui.voice_counter);
    linx_ui_tick(90000U);
    assert(!ui.voice_counter && gateway_captures == captures && gateway_finishes == finishes);
    assert(lv_obj_get_child_count(ui.root) == children && lv_mem_test() == LV_RESULT_OK);
    assert(!linx_ui_voice_event(&event));
    setup_failed_gateway_voice(LINX_VOICE_CONNECTING, LINX_VOICE_OK);
    remote.view.request.capture_claimed = true;
    event.generation = ui.voice_generation; event.state = LINX_VOICE_LISTENING;
    assert(linx_ui_voice_event(&event));
    linx_ui_tick(8500U); event.state = LINX_VOICE_WAITING;
    assert(linx_ui_voice_event(&event));
    ++remote.view.revision; linx_ui_tick(30000U);
    assert(!strcmp(lv_label_get_text(ui.voice_counter), "00:08"));
    /* 目录更新不能拿别的工程名代替当前固定请求的工程。 */
    strcpy(remote.view.detail.agent_session_id, "another-session"); render();
    assert(!has_remote_label(remote.view.detail.project_name));
    puts("Gateway录音视觉：同代开麦起计时、125ms对象复用、目录重绘不重置、OK后冻结和离页隔离通过");
}

static void gateway_request_response(const char *status)
{
    char response[4096];
    int size = snprintf(response, sizeof(response), "{\"id\":\"voice-status\",\"type\":\"response\",\"reply_to\":\"%s\","
        "\"payload\":{\"ok\":true,\"data\":{\"request_id\":\"%s\",\"agent_session_id\":\"%s\",\"target_title\":\"%s\","
        "\"text\":\"原话\",\"status\":\"%s\",\"error\":null}}}", remote.pending.id, remote.view.request.request_id,
        remote.view.request.agent_session_id, remote.view.request.target_title, status);
    assert(size > 0 && (size_t)size < sizeof(response));
    assert(linx_gateway_client_receive(&remote, remote.view.generation, response, (size_t)size, (uint32_t)ui.now_ms));
}

static void test_gateway_original_agent_voice_lifecycle(void)
{
    remote_setup(); ui.at.page = LINX_UI_OVERVIEW;
    unsigned prepares = gateway_prepares, captures = gateway_captures;
    render();
    assert(gateway_prepares == prepares + 1U && gateway_captures == captures);
    assert(!gateway_last_prepare.gateway_request_id[0] &&
        !strcmp(gateway_last_prepare.gateway_agent_session_id, remote.view.selected_session_id));
    render(); assert(gateway_prepares == prepares + 1U);
    remote.view.sessions_omitted = true; ui.at.page = LINX_UI_TASKS; render();
    assert(has_remote_label("本批仅显示最近2项"));
    remote.view.messages_omitted = true; ui.at.page = LINX_UI_HISTORY; render();
    assert(has_remote_label("本批仅显示最近2项") && gateway_captures == captures);

    setup_gateway_one_key_capture();
    assert(!strcmp(gateway_last_start.gateway_request_id, remote.view.request.request_id));
    assert(!strcmp(gateway_last_start.gateway_agent_session_id, remote.view.request.agent_session_id));
    assert(gateway_last_start.context.task_id == 0U);
    unsigned finishes = gateway_finishes, cancels = gateway_cancels, requests_cancelled = gateway_request_cancels;
    unsigned confirms = gateway_confirms;
    linx_ui_tick(8001U); linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(gateway_finishes == finishes + 1U && ui.gateway_finish_pending);
    assert(remote.view.request.confirm_requested && !remote.view.request.confirm_sent);
    assert(has_remote_label("正在结束录音"));
    for (unsigned i = 0U; i < 3U; ++i) linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(gateway_finishes == finishes + 1U);
    linx_voice_event_t event = {.generation = ui.voice_generation, .state = LINX_VOICE_WAITING};
    assert(linx_ui_voice_event(&event) && !ui.gateway_finish_pending);
    assert(has_remote_label("录音已结束，等待灵矽回复"));
    remote.view.request.status = LINX_GATEWAY_REQUEST_RUNNING; ++remote.view.revision; linx_ui_tick(18000U);
    assert(ui.at.page == LINX_UI_VOICE && !strcmp(lv_label_get_text(ui.voice_counter), "00:08"));
    remote.view.request.status = LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION;
    event.state = LINX_VOICE_SPEAKING; assert(linx_ui_voice_event(&event));
    capture_session_status("agent", "speaking", false);
    event.state = LINX_VOICE_FINISHED; event.error = LINX_VOICE_TIMEOUT;
    assert(!linx_ui_voice_event(&event));
    event.error = LINX_VOICE_OK; assert(linx_ui_voice_event(&event));
    assert(ui.at.page == LINX_UI_OVERVIEW && !ui.voice_generation && !linx_ui_voice_event(&event));
    assert(gateway_cancels == cancels && gateway_request_cancels == requests_cancelled);
    assert(strstr(gateway_last_send, "request.get"));
    gateway_request_response("awaiting_confirmation"); linx_ui_tick(18001U);
    assert(!remote.view.request.cancel_requested && gateway_request_cancels == requests_cancelled);
    assert(remote.view.request.confirm_sent && gateway_confirms == confirms + 1U);
    assert(strstr(gateway_last_send, "request.confirm"));
    /* 返回概览后再打开状态、重复 OK 和播完清理均不能取消或重新确认本轮。 */
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_RECEIPT);
    strcpy(remote.view.request.error_message, "任务调度尚未实现，请求未执行");
    render();
    assert(has_remote_label(remote.view.request.error_message));
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(!remote.view.request.cancel_requested && gateway_confirms == confirms + 1U);

    const linx_gateway_request_status_t running[] = {LINX_GATEWAY_REQUEST_SENDING, LINX_GATEWAY_REQUEST_RUNNING, LINX_GATEWAY_REQUEST_COMPLETED};
    for (unsigned i = 0; i < 3; ++i) {
        setup_gateway_one_key_capture(); linx_ui_handle_key(LINX_UI_KEY_OK);
        remote.view.request.status = running[i];
        event.generation = ui.voice_generation; event.state = LINX_VOICE_FINISHED;
        requests_cancelled = gateway_request_cancels;
        assert(linx_ui_voice_event(&event));
        remote.pending.operation = 0U; remote.view.loading = false; linx_ui_tick(2U);
        assert(gateway_request_cancels == requests_cancelled && !remote.view.request.cancel_requested);
    }
    setup_gateway_one_key_capture(); finishes = gateway_finishes; cancels = gateway_cancels;
    linx_ui_handle_key(LINX_UI_KEY_BACK);
    assert(ui.at.page == LINX_UI_OVERVIEW && remote.view.request.cancel_requested && gateway_cancels == cancels + 1U);
    assert(gateway_finishes == finishes && !linx_ui_voice_event(&event));

    setup_gateway_one_key_capture(); linx_ui_handle_key(LINX_UI_KEY_OK);
    remote.view.request.status = LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION;
    event.generation = ui.voice_generation; event.state = LINX_VOICE_FINISHED;
    assert(linx_ui_voice_event(&event));
    gateway_request_response("unknown"); linx_ui_tick(2U);
    assert(ui.gateway_voice_finished && !remote.view.request.cancel_requested);
    assert(gateway_action(LINX_GATEWAY_ACTION_REQUEST_GET, NULL));
    gateway_request_response("running"); linx_ui_tick(3U);
    assert(!remote.view.request.cancel_requested && !ui.gateway_voice_finished);

    setup_gateway_one_key_capture();
    event.generation = ui.voice_generation; event.state = LINX_VOICE_ERROR; event.error = LINX_VOICE_NETWORK_FAILED;
    captures = gateway_captures; finishes = gateway_finishes;
    assert(linx_ui_voice_event(&event) && ui.at.page == LINX_UI_VOICE && has_remote_label("灵矽连接已断开"));
    assert(gateway_captures == captures && gateway_finishes == finishes);
    uint32_t previous = ui.voice_generation;
    linx_ui_set_voice_handler(gateway_voice, NULL);
    assert(ui.at.page == LINX_UI_OVERVIEW && remote.view.request.cancel_requested && !ui.voice_generation);
    event.generation = previous; event.state = LINX_VOICE_FINISHED; event.error = LINX_VOICE_OK;
    assert(!linx_ui_voice_event(&event));
    puts("原灵矽同会话UI：真实目标预热、prepare许可、一次OK停录并确认、迟到文字不取消、原声结束返回和未知状态不重投通过");
}

static void test_gateway_ok_authorizes_only_after_stop_accepted(void)
{
    setup_gateway_one_key_capture();
    unsigned confirms = gateway_confirms;
    reject_finish = true;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.voice_state == LINX_VOICE_LISTENING && !remote.view.request.confirm_requested);
    assert(gateway_confirms == confirms);
    reject_finish = false;
    /* 原文先到也必须等用户 OK，不能由 proposed 自动授权。 */
    remote.view.request.status = LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION;
    remote.view.request.text_present = true;
    linx_gateway_client_tick(&remote, 2U);
    assert(gateway_confirms == confirms);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.voice_state == LINX_VOICE_WAITING && remote.view.request.confirm_requested);
    assert(remote.view.request.confirm_sent && gateway_confirms == confirms + 1U);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(gateway_confirms == confirms + 1U);
}

static void test_gateway_error_then_prepare_resets_ui(void)
{
    setup_failed_gateway_voice(LINX_VOICE_ERROR, LINX_VOICE_NETWORK_FAILED);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_OVERVIEW && remote.view.request.cancel_requested);
    gateway_request_response("cancelled");
    ui.at.selected = 1U; linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(ui.at.page == LINX_UI_VOICE && ui.voice_generation == 0U &&
        ui.voice_state == LINX_VOICE_PREPARING && ui.voice_error == LINX_VOICE_OK);
    unsigned finishes = gateway_finishes, cancels = gateway_cancels;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    assert(!remote.view.request.cancel_requested && gateway_finishes == finishes && gateway_cancels == cancels);
    assert(!has_remote_label("灵矽连接已断开") && has_remote_label("正在登记本条指令"));
}

static void test_gateway_started_owner_released_after_disconnect(void)
{
    setup_gateway_one_key_capture();
    uint32_t owner = ui.voice_generation;
    assert(ui.connection_generation == owner && gateway_last_prepare.generation != owner);
    linx_ui_handle_key(LINX_UI_KEY_OK);
    remote.view.connected = remote.view.bound = false;
    const linx_voice_event_t done = {.generation = owner, .state = LINX_VOICE_FINISHED};
    assert(linx_ui_voice_event(&done));
    unsigned releases = gateway_releases;
    linx_ui_set_gateway_client(NULL);
    assert(gateway_releases == releases + 1U && gateway_last_release.generation == owner);

    remote_setup(); ui.at.page = LINX_UI_OVERVIEW; ui.at.selected = 1U; render();
    uint32_t prepared = ui.connection_generation;
    linx_ui_handle_key(LINX_UI_KEY_OK);
    remote.pending.operation = 0U; remote.view.loading = false;
    remote.view.request.status = LINX_GATEWAY_REQUEST_AWAITING_TEXT; remote.view.request.capture_pending = true;
    reject_capture = true; ++remote.view.revision; linx_ui_tick(1U);
    assert(ui.voice_state == LINX_VOICE_ERROR && ui.connection_generation == prepared);
    reject_capture = false;
}

static void test_remote_initial_transport_error_and_recovery(void)
{
    remote_setup();
    const linx_gateway_client_config_t config = {.configured = true, .instance_id = 2U, .send = gateway_send};
    linx_gateway_client_deinit(&remote);
    linx_gateway_client_init(&remote, &config);
    render();
    assert(!remote.view.connected && remote.view.generation == 0U);
    assert(strcmp(gateway_connection(), "正在连接网关") == 0);
    linx_gateway_client_transport_error(&remote, 0U);
    linx_ui_tick(100U);
    assert(remote.view.error == LINX_GATEWAY_ERROR_TRANSPORT && !remote.view.connected);
    bool visible_error = false;
    for (size_t i = 0U; i < ui.labels_used; ++i)
        visible_error |= strstr(lv_label_get_text(ui.labels[i]), "网关连接失败，请检查网络与服务配置") != NULL;
    assert(visible_error && strstr(gateway_connection(), "连接失败"));

    linx_gateway_client_connected(&remote, 1U, "AA:BB:CC:DD:EE:FF", 101U);
    linx_ui_tick(101U);
    assert(remote.view.error == LINX_GATEWAY_ERROR_NONE && remote.view.connected);
    char response[1024];
    int size = snprintf(response, sizeof(response),
        "{\"id\":\"hello-recovered\",\"type\":\"response\",\"reply_to\":\"%s\","
        "\"payload\":{\"ok\":true,\"data\":{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":\"desktop\"}}}", remote.pending.id);
    assert(size > 0 && (size_t)size < sizeof(response));
    assert(linx_gateway_client_receive(&remote, 1U, response, (size_t)size, 102U));
    size = snprintf(response, sizeof(response),
        "{\"id\":\"list-recovered\",\"type\":\"response\",\"reply_to\":\"%s\","
        "\"payload\":{\"ok\":true,\"data\":{\"items\":[],\"next_cursor\":null}}}", remote.pending.id);
    assert(size > 0 && (size_t)size < sizeof(response));
    assert(linx_gateway_client_receive(&remote, 1U, response, (size_t)size, 103U));
    linx_ui_tick(103U);
    assert(remote.view.error == LINX_GATEWAY_ERROR_NONE && remote.view.connected && remote.view.bound);
    assert(strcmp(gateway_connection(), "工作电脑 · 已连接") == 0);
    for (size_t i = 0U; i < ui.labels_used; ++i)
        assert(strstr(lv_label_get_text(ui.labels[i]), "连接失败") == NULL);
    puts("真实 UI 首次连接失败：generation0传输错误明确显示，随后hello/绑定/列表成功清除错误通过");
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
    test_soft_cancel_without_overview_retains_owner();
    test_phone_secret_and_render_boundaries();
    test_remote_live_and_saved_status();
    test_remote_project_context();
    test_remote_render_and_navigation();
    test_remote_body_and_full_draft();
    test_remote_user_intents();
    test_remote_content_priority_during_refresh();
    test_remote_initial_transport_error_and_recovery();
    test_gateway_voice_failure_returns_safely();
    test_gateway_voice_pause_context();
    test_gateway_recording_wave_and_timer();
    test_gateway_original_agent_voice_lifecycle();
    test_gateway_ok_authorizes_only_after_stop_accepted();
    test_gateway_error_then_prepare_resets_ui();
    test_gateway_started_owner_released_after_disconnect();
    linx_ui_deinit();
    linx_gateway_client_deinit(&remote);
    render();
    assert(ui.root == NULL && lv_mem_test() == LV_RESULT_OK);
    lv_display_delete(display);
    lv_deinit();
    puts("UI 私有边界通过：对象与绘制容量、复用、UTF-8 窗口、字体测宽、页脚截断和栈保护");
    return 0;
}
