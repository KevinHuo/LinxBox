/* 实际 controller/UI 的 stdio 适配器；网络由外层真实 TLS socket 桥接。 */
#include "linx_ui.h"
#include "linx_assets.h"
#include "bsp_display_rounding.h"
#include "cJSON.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIDTH 240
#define HEIGHT 320
static uint16_t draw_buffer[WIDTH * 40];
static uint16_t frame[HEIGHT][WIDTH];
static lv_display_t *display;
static linx_gateway_client_t client;
static uint64_t now_ms;
static const char *screens;
static unsigned voice_starts, voice_finishes, voice_cancels;
static linx_voice_request_t last_voice;

static void log_message(lv_log_level_t level, const char *message)
{
    (void)level;
    fputs(message, stderr);
}

static uint32_t codepoint(const char *value, uint32_t *index)
{
    unsigned char first = (unsigned char)value[(*index)++];
    if (first < 0x80U) return first;
    unsigned remaining = (first & 0xe0U) == 0xc0U ? 1U : (first & 0xf0U) == 0xe0U ? 2U : 3U;
    uint32_t cp = first & (remaining == 1U ? 0x1fU : remaining == 2U ? 0x0fU : 0x07U);
    while (remaining-- && value[*index]) cp = (cp << 6U) | ((unsigned char)value[(*index)++] & 0x3fU);
    return cp;
}

static void emit(cJSON *object)
{
    char *json = cJSON_PrintUnformatted(object);
    assert(json != NULL);
    puts(json);
    fflush(stdout);
    cJSON_free(json);
    cJSON_Delete(object);
}

static bool send_frame(uint32_t generation, const char *json, size_t size, void *user)
{
    (void)user;
    cJSON *data = cJSON_ParseWithLength(json, size);
    assert(data != NULL);
    cJSON *output = cJSON_CreateObject();
    cJSON_AddStringToObject(output, "event", "send");
    cJSON_AddNumberToObject(output, "generation", generation);
    cJSON_AddItemToObject(output, "data", data);
    emit(output);
    return true;
}

static bool voice(const linx_voice_request_t *request, void *user)
{
    (void)user;
    if (request->command == LINX_VOICE_START) { ++voice_starts; last_voice = *request; }
    else if (request->command == LINX_VOICE_FINISH) ++voice_finishes;
    else if (request->command == LINX_VOICE_CANCEL) ++voice_cancels;
    return true;
}

static void flush(lv_display_t *target, const lv_area_t *area, uint8_t *pixels)
{
    lv_draw_buf_t *buffer = lv_display_get_buf_active(target);
    assert(buffer != NULL && buffer->header.cf == LV_COLOR_FORMAT_RGB565);
    for (int y = area->y1; y <= area->y2; ++y) {
        int32_t x1, x2;
        assert(bsp_display_rounded_row_span(y, WIDTH, HEIGHT, 30, &x1, &x2));
        const uint16_t *row = (const uint16_t *)(pixels + (y - area->y1) * buffer->header.stride);
        for (int x = area->x1; x <= area->x2; ++x)
            frame[y][x] = x >= x1 && x <= x2 ? row[x - area->x1] : 0U;
    }
    lv_display_flush_ready(target);
}

static void labels(lv_obj_t *object, cJSON *array)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < WIDTH && area.y2 < HEIGHT);
        cJSON_AddItemToArray(array, cJSON_CreateString(lv_label_get_text(object)));
        const char *value = lv_label_get_text(object);
        const lv_font_t *font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
        uint32_t index = 0U;
        while (value[index]) {
            uint32_t cp = codepoint(value, &index);
            if (cp < 0x20U) continue;
            lv_font_glyph_dsc_t glyph = {0};
            assert(lv_font_get_glyph_dsc(font, &glyph, cp, 0U) && !glyph.is_placeholder);
        }
        lv_point_t natural;
        lv_text_get_size(&natural, lv_label_get_text(object), lv_obj_get_style_text_font(object, LV_PART_MAIN),
                        0, 0, lv_obj_get_content_width(object), LV_TEXT_FLAG_NONE);
        assert(lv_label_get_long_mode(object) == LV_LABEL_LONG_DOT || natural.y <= lv_obj_get_content_height(object));
    }
    for (uint32_t i = 0U; i < lv_obj_get_child_count(object); ++i)
        labels(lv_obj_get_child(object, (int32_t)i), array);
}

static unsigned hash(const char *text)
{
    unsigned value = 2166136261U;
    for (size_t i = 0U; text[i]; ++i) value = (value ^ (unsigned char)text[i]) * 16777619U;
    return value;
}

static void capture(const char *name)
{
    if (!screens || !name || !name[0]) return;
    for (size_t i = 0U; name[i]; ++i)
        if (!isalnum((unsigned char)name[i]) && name[i] != '-' && name[i] != '_') return;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s.ppm", screens, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; ++y) for (int x = 0; x < WIDTH; ++x) {
        uint16_t pixel = frame[y][x];
        unsigned char rgb[] = {(unsigned char)(((pixel >> 11) & 31U) * 255U / 31U),
                               (unsigned char)(((pixel >> 5) & 63U) * 255U / 63U),
                               (unsigned char)((pixel & 31U) * 255U / 31U)};
        assert(fwrite(rgb, 1U, sizeof(rgb), file) == sizeof(rgb));
    }
    assert(fclose(file) == 0);
}

static void snapshot(const char *name)
{
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(display);
    assert(lv_mem_test() == LV_RESULT_OK);
    const linx_ui_debug_t debug = linx_ui_debug();
    const linx_gateway_view_t *v = linx_gateway_client_view(&client);
    assert(!debug.render_overflow);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "event", "screen");
    cJSON_AddStringToObject(out, "name", name ? name : "");
    cJSON_AddNumberToObject(out, "page", debug.page);
    static const char *const pages[] = {"boot", "tasks", "overview", "reader", "history", "voice", "confirm",
        "receipt", "settings", "reset", "about", "wifi"};
    cJSON_AddStringToObject(out, "page_name", pages[debug.page]);
    cJSON_AddNumberToObject(out, "selected", debug.selected);
    cJSON_AddStringToObject(out, "session_id", debug.session_id);
    cJSON_AddStringToObject(out, "request_id", debug.request_id);
    cJSON_AddStringToObject(out, "message_id", debug.message_id);
    cJSON_AddNumberToObject(out, "offset", debug.content_offset);
    cJSON_AddNumberToObject(out, "read_page", debug.read_page);
    cJSON_AddNumberToObject(out, "read_page_count", debug.read_page_count);
    cJSON_AddNumberToObject(out, "content_hash", hash(v->content));
    cJSON_AddBoolToObject(out, "new_reply", debug.new_reply);
    cJSON_AddBoolToObject(out, "loading", v->loading);
    cJSON_AddBoolToObject(out, "configured", v->configured);
    cJSON_AddBoolToObject(out, "connected", v->connected);
    cJSON_AddBoolToObject(out, "bound", v->bound);
    cJSON_AddNumberToObject(out, "error", v->error);
    cJSON_AddNumberToObject(out, "request_status", v->request.status);
    cJSON_AddNumberToObject(out, "sessions_depth", client.sessions_depth);
    cJSON_AddNumberToObject(out, "history_depth", client.history_depth);
    cJSON_AddNumberToObject(out, "voice_starts", voice_starts);
    cJSON_AddNumberToObject(out, "voice_finishes", voice_finishes);
    cJSON_AddNumberToObject(out, "voice_cancels", voice_cancels);
    cJSON_AddNumberToObject(out, "voice_generation", debug.voice_generation);
    cJSON_AddStringToObject(out, "voice_request_id", last_voice.gateway_request_id);
    cJSON_AddStringToObject(out, "voice_session_id", last_voice.gateway_agent_session_id);
    cJSON *array = cJSON_AddArrayToObject(out, "labels");
    labels(lv_screen_active(), array);
    capture(name);
    emit(out);
}

static const char *string(cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static uint32_t number(cJSON *object, const char *key)
{
    return (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(object, key));
}

static void command(cJSON *input)
{
    const char *event = string(input, "event");
    if (strcmp(event, "connected") == 0)
        linx_gateway_client_connected(&client, number(input, "generation"), string(input, "mac_address"), (uint32_t)now_ms);
    else if (strcmp(event, "disconnected") == 0)
        linx_gateway_client_disconnected(&client, number(input, "generation"));
    else if (strcmp(event, "frame") == 0) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(input, "data");
        char *json = cJSON_IsString(data) ? NULL : cJSON_PrintUnformatted(data);
        const char *bytes = json ? json : cJSON_GetStringValue(data);
        if (bytes) linx_gateway_client_receive(&client, number(input, "generation"), bytes, strlen(bytes), (uint32_t)now_ms);
        cJSON_free(json);
    } else if (strcmp(event, "key") == 0) {
        const char *key = string(input, "key");
        linx_ui_handle_key(strcmp(key, "up") == 0 ? LINX_UI_KEY_UP : strcmp(key, "down") == 0 ? LINX_UI_KEY_DOWN :
                           strcmp(key, "back") == 0 ? LINX_UI_KEY_BACK : LINX_UI_KEY_OK);
    } else if (strcmp(event, "tick") == 0) {
        uint64_t next = number(input, "now_ms");
        if (next >= now_ms) { lv_tick_inc((uint32_t)(next - now_ms)); now_ms = next; }
    } else if (strcmp(event, "voice") == 0) {
        const linx_voice_event_t update = {.generation = number(input, "generation"),
            .state = (linx_voice_state_t)number(input, "state"), .error = (linx_voice_error_t)number(input, "error")};
        linx_ui_voice_event(&update);
    } else if (strcmp(event, "configured") == 0) {
        client.view.configured = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "value"));
        ++client.view.revision;
    }
    linx_gateway_client_tick(&client, (uint32_t)now_ms);
    linx_ui_tick(now_ms);
    if (strcmp(event, "snapshot") == 0) snapshot(string(input, "name"));
}

int main(int argc, char **argv)
{
    screens = argc > 1 ? argv[1] : NULL;
    /* lv_init 会重置全局日志回调；初始化告警也必须留在 stderr。 */
    int protocol_stdout = dup(STDOUT_FILENO);
    assert(protocol_stdout >= 0 && dup2(STDERR_FILENO, STDOUT_FILENO) >= 0);
    lv_init();
    fflush(stdout);
    assert(dup2(protocol_stdout, STDOUT_FILENO) >= 0 && close(protocol_stdout) == 0);
    lv_log_register_print_cb(log_message);
    display = lv_display_create(WIDTH, HEIGHT);
    assert(display != NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    uint64_t instance_id = 0U;
    FILE *random = fopen("/dev/urandom", "rb");
    assert(random != NULL && fread(&instance_id, sizeof(instance_id), 1U, random) == 1U);
    assert(fclose(random) == 0);
    if (instance_id == 0U) instance_id = 1U;
    const linx_gateway_client_config_t config = {.configured = true, .instance_id = instance_id, .send = send_frame};
    linx_gateway_client_init(&client, &config);
    linx_ui_init(lv_screen_active(), 0U);
    linx_ui_set_voice_handler(voice, NULL);
    linx_ui_set_gateway_client(&client);
    char *line = NULL;
    size_t capacity = 0U;
    while (getline(&line, &capacity, stdin) >= 0) {
        cJSON *input = cJSON_Parse(line);
        if (!input) { fprintf(stderr, "无效的 host 控制 JSON\n"); continue; }
        command(input);
        cJSON_Delete(input);
    }
    free(line);
    linx_ui_deinit();
    linx_gateway_client_deinit(&client);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
