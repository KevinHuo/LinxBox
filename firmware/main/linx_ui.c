#include "linx_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "linx_assets.h"
#include "linx_navigation.h"
#include "linx_reader.h"

#define COLOR_PAPER 0xF7F2E8U
#define COLOR_INK 0x292722U
#define COLOR_MUTED 0x746B5FU
#define COLOR_LINE 0xD8D0C3U
#define COLOR_ACCENT 0xD94F28U
#define COLOR_ACCENT_TEXT 0xAD391CU
#define COLOR_SELECTED 0xF8E0CFU
#define LABEL_COUNT 24U
#define BOX_COUNT 8U
#define DECOR_COUNT 128U
#define STACK_LIMIT 10U
#define HISTORY_ITEMS (LINX_HISTORY_CAPACITY * 2U)
#define BODY_WIDTH 208
#define BODY_HEIGHT 200
#define BODY_TOP 74

typedef struct {
    int16_t x1, y1, x2, y2;
    uint32_t color;
    uint8_t width;
    bool rectangle;
    uint8_t radius;
} decoration_t;

typedef enum { HISTORY_AGENT, HISTORY_USER } history_kind_t;

typedef struct {
    linx_ui_page_t page;
    uint32_t task;
    uint32_t turn;
    uint32_t command;
    uint32_t history_turn;
    history_kind_t history_kind;
    size_t selected;
    size_t read_page;
    bool user_text;
} location_t;

typedef struct {
    uint32_t turn;
    history_kind_t kind;
} history_item_t;

typedef struct {
    uint32_t turn;
    size_t page;
    bool user;
} read_position_t;

static struct {
    lv_obj_t *root;
    lv_obj_t *labels[LABEL_COUNT];
    lv_obj_t *boxes[BOX_COUNT];
    lv_obj_t *voice_counter;
    lv_obj_t *decor;
    decoration_t decorations[DECOR_COUNT];
    size_t decoration_count;
    bool render_overflow;
    size_t labels_used;
    size_t boxes_used;
    linx_demo_t demo;
    location_t at;
    location_t stack[STACK_LIMIT];
    size_t depth;
    location_t reply_origin;
    size_t reply_depth;
    linx_reply_context_t reply;
    linx_voice_handler_t voice_handler;
    void *voice_user;
    linx_back_mode_t back_mode;
    linx_back_mode_t pending_back_mode;
    linx_ui_back_mode_handler_t back_mode_handler;
    void *back_mode_user;
    bool back_mode_pending;
    const char *back_mode_notice;
    uint32_t connection_generation;
    linx_reply_context_t prepared_context;
    bool prepared_context_valid;
    linx_ui_page_t rendered_page;
    uint32_t voice_generation;
    linx_voice_state_t voice_state;
    linx_voice_error_t voice_error;
    char activation_code[7];
    location_t wifi_origin;
    linx_ui_wifi_mode_t wifi_mode;
    char wifi_ap_ssid[LINX_WIFI_SSID_CAPACITY];
    char wifi_ap_password[9];
    read_position_t positions[LINX_TASK_COUNT][LINX_HISTORY_CAPACITY * 2U];
    linx_reader_layout_t pages;
    history_item_t history[HISTORY_ITEMS];
    size_t history_count;
    char page_text[1024];
    uint64_t now_ms;
    uint64_t boot_ms;
    uint64_t voice_ms;
    uint64_t last_wave_ms;
    uint32_t seen_revision;
    const char *notice;
} ui;
static uint32_t voice_sequence;

static void render(void);

static const linx_task_t *task(void)
{
    return linx_demo_task(&ui.demo, ui.at.task);
}

static const linx_turn_t *latest_reply(void)
{
    const linx_task_t *current = task();
    if (!current) return NULL;
    for (size_t i = 0U; i < current->turn_count; i++) {
        const linx_turn_t *turn = linx_demo_history(&ui.demo, current->id, i);
        if (turn && turn->complete && turn->content_id != LINX_CONTENT_NONE) return turn;
    }
    return NULL;
}

static void set_hidden(lv_obj_t *object, bool hidden)
{
    if (hidden) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *text(const char *value, int x, int y, int width, int height,
                       const lv_font_t *font, uint32_t color)
{
    if (ui.labels_used >= LABEL_COUNT) { ui.render_overflow = true; return NULL; }
    lv_obj_t *label = ui.labels[ui.labels_used++];
    set_hidden(label, false);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, value ? value : "");
    return label;
}

static lv_obj_t *box(int x, int y, int width, int height, uint32_t color, int radius)
{
    if (ui.boxes_used >= BOX_COUNT) { ui.render_overflow = true; return NULL; }
    lv_obj_t *object = ui.boxes[ui.boxes_used++];
    set_hidden(object, false);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    return object;
}

static void stroke(int x1, int y1, int x2, int y2, uint32_t color, unsigned width)
{
    if (ui.decoration_count == DECOR_COUNT) { ui.render_overflow = true; return; }
    ui.decorations[ui.decoration_count++] = (decoration_t){
        x1, y1, x2, y2, color, (uint8_t)width, false, 0};
}

static void shape(int x, int y, int width, int height, uint32_t color, unsigned border, unsigned radius)
{
    if (ui.decoration_count == DECOR_COUNT) { ui.render_overflow = true; return; }
    ui.decorations[ui.decoration_count++] = (decoration_t){
        x, y, x + width - 1, y + height - 1, color, (uint8_t)border, true, (uint8_t)radius};
}

/* One retained object draws small vector details; no icon bitmap or per-frame objects. */
static void draw_decorations(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t origin;
    lv_obj_get_coords(ui.decor, &origin);
    for (size_t i = 0U; i < ui.decoration_count; i++) {
        const decoration_t *item = &ui.decorations[i];
        if (item->rectangle) {
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color = dsc.border_color = lv_color_hex(item->color);
            dsc.bg_opa = item->width ? LV_OPA_TRANSP : LV_OPA_COVER;
            dsc.border_opa = LV_OPA_COVER;
            dsc.border_width = item->width;
            dsc.radius = item->radius;
            lv_area_t area = {origin.x1 + item->x1, origin.y1 + item->y1,
                              origin.x1 + item->x2, origin.y1 + item->y2};
            lv_draw_rect(layer, &dsc, &area);
        } else {
            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.p1 = (lv_point_precise_t){origin.x1 + item->x1, origin.y1 + item->y1};
            dsc.p2 = (lv_point_precise_t){origin.x1 + item->x2, origin.y1 + item->y2};
            dsc.color = lv_color_hex(item->color);
            dsc.width = item->width;
            lv_draw_line(layer, &dsc);
        }
    }
}

static void x_mark(int x, int y, int size, int weight)
{
    int half = size / 2;
    int gap = weight <= 3 ? 2 : weight / 2 + 2;
    stroke(x, y, x + half - gap, y + half - gap, COLOR_ACCENT, weight);
    stroke(x + half + gap, y + half + gap, x + size, y + size, COLOR_ACCENT, weight);
    stroke(x + size, y, x + half + gap, y + half - gap, COLOR_ACCENT, weight);
    stroke(x + half - gap, y + half + gap, x, y + size, COLOR_ACCENT, weight);
}

static void chevron(int x, int y, uint32_t color)
{
    stroke(x, y, x + 4, y + 4, color, 2);
    stroke(x + 4, y + 4, x, y + 8, color, 2);
}

static void task_icon(size_t kind, int x, int y, uint32_t color)
{
    if (kind == 0U) {
        shape(x + 2, y, 13, 18, color, 1, 1);
        stroke(x + 5, y + 6, x + 11, y + 6, color, 1);
        stroke(x + 5, y + 10, x + 11, y + 10, color, 1);
        stroke(x + 5, y + 14, x + 9, y + 14, color, 1);
    } else if (kind == 1U) {
        stroke(x + 5, y + 3, x, y + 8, color, 2);
        stroke(x, y + 8, x + 5, y + 13, color, 2);
        stroke(x + 13, y + 3, x + 18, y + 8, color, 2);
        stroke(x + 18, y + 8, x + 13, y + 13, color, 2);
        stroke(x + 11, y, x + 7, y + 16, color, 2);
    } else {
        shape(x, y, 18, 17, color, 1, 2);
        stroke(x + 6, y, x + 6, y + 16, color, 1);
        stroke(x + 6, y + 6, x + 17, y + 6, color, 1);
        stroke(x + 10, y + 10, x + 14, y + 10, color, 1);
    }
}

static void reset_objects(void)
{
    for (size_t i = 0U; i < LABEL_COUNT; i++) set_hidden(ui.labels[i], true);
    for (size_t i = 0U; i < BOX_COUNT; i++) set_hidden(ui.boxes[i], true);
    ui.labels_used = 0U;
    ui.boxes_used = 0U;
    ui.decoration_count = 0U;
    ui.render_overflow = false;
    ui.voice_counter = NULL;
}

static void brand(void)
{
    text("Lin", 20, 12, 34, 25, &linx_font_20_bold, COLOR_INK);
    x_mark(57, 18, 13, 3);
    text(ui.at.page == LINX_UI_WIFI ? "设备" : ui.voice_handler && ui.at.page == LINX_UI_VOICE ? "灵矽" : "演示",
         141, 18, 25, 16, &linx_font_12, COLOR_MUTED);
    shape(174, 21, 15, 9, COLOR_MUTED, 1, 1);
    shape(176, 23, 9, 5, COLOR_MUTED, 0, 0);
    shape(189, 24, 2, 3, COLOR_MUTED, 0, 0);
    text("78%", 196, 17, 28, 16, &linx_font_12, COLOR_MUTED);
}

static void header(const char *title, const char *context)
{
    brand();
    bool listening = ui.at.page == LINX_UI_VOICE;
    lv_obj_t *heading = text(title, 16, listening ? 43 : 47, 208, listening ? 33 : 28,
                              listening ? &linx_font_24_bold : &linx_font_20_bold, COLOR_INK);
    if (heading) lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_t *subtitle = text(context, 16, 80, 208, 16, &linx_font_12,
                              ui.demo.online ? COLOR_MUTED : COLOR_ACCENT_TEXT);
    if (subtitle) lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
}

static void footer(const char *value)
{
    char primary[80];
    size_t length = strcspn(value, "\n");
    if (length >= sizeof(primary)) length = sizeof(primary) - 1U;
    memcpy(primary, value, length);
    primary[length] = '\0';
    const char *action = strstr(value, "设备设置") ? "设置" :
                         strstr(value, "演示选项") ? "选项" :
                         strstr(value, "取消") ? "取消" :
                         strstr(value, "放弃") ? "放弃" : "返回";
    char secondary[24];
    snprintf(secondary, sizeof(secondary), "%s%s",
             ui.back_mode == LINX_BACK_DOUBLE_CLICK ? "双击" : "长按", action);
    stroke(16, 280, 224, 280, COLOR_LINE, 1);
    text(primary, 24, 288, 136, 16, &linx_font_12, COLOR_INK);
    text(secondary, 168, 288, 48, 16, &linx_font_12, COLOR_MUTED);
}

static void selected_row(int y, int height)
{
    box(16, y, 208, height, COLOR_SELECTED, 5);
    shape(16, y + 5, 3, height - 10, COLOR_ACCENT, 0, 1);
}

static void menu_row(size_t row, size_t selected, int y, int height,
                       const char *value, bool enabled)
{
    if (row == selected) selected_row(y, height);
    text(value, 27, y + (height > 30 ? 6 : 1), 180, height > 30 ? height - 6 : height - 1,
         row == selected && enabled ? &linx_font_16_bold : &linx_font_16,
         !enabled ? COLOR_MUTED : (row == selected ? COLOR_ACCENT_TEXT : COLOR_INK));
    if (enabled) chevron(214, y + height / 2 - 4, row == selected ? COLOR_ACCENT_TEXT : COLOR_MUTED);
}

static void push(linx_ui_page_t page)
{
    if (ui.depth < STACK_LIMIT) ui.stack[ui.depth++] = ui.at;
    ui.at.page = page;
    ui.at.selected = 0U;
    ui.notice = NULL;
}

static void pop(void)
{
    if (ui.depth > 0U) ui.at = ui.stack[--ui.depth];
    else ui.at.page = LINX_UI_TASKS;
    ui.notice = NULL;
}

static void to_overview(void)
{
    uint32_t id = ui.at.task;
    memset(&ui.at, 0, sizeof(ui.at));
    ui.at.page = LINX_UI_TASKS;
    ui.at.selected = id > 0U ? id - 1U : 0U;
    ui.depth = 0U;
    push(LINX_UI_OVERVIEW);
    ui.at.task = id;
    ui.at.selected = latest_reply() ? 0U : 1U;
}

static uint32_t next_codepoint(const char *value, size_t bytes, size_t *offset)
{
    if (*offset >= bytes) return 0U;
    unsigned char first = (unsigned char)value[(*offset)++];
    if (first < 0x80U) return first;
    unsigned remaining = (first & 0xe0U) == 0xc0U ? 1U : (first & 0xf0U) == 0xe0U ? 2U : 3U;
    uint32_t codepoint = first & (remaining == 1U ? 0x1fU : remaining == 2U ? 0x0fU : 0x07U);
    while (remaining-- > 0U && *offset < bytes) {
        codepoint = (codepoint << 6U) | ((unsigned char)value[(*offset)++] & 0x3fU);
    }
    return codepoint;
}

static int32_t measure(const char *value, size_t bytes, void *context)
{
    const lv_font_t *font = context;
    size_t offset = 0U;
    uint32_t current = next_codepoint(value, bytes, &offset);
    int32_t width = 0;
    while (current != 0U) {
        uint32_t next = next_codepoint(value, bytes, &offset);
        width += lv_font_get_glyph_width(font, current, next);
        current = next;
    }
    return width;
}

static read_position_t *reading_position(uint32_t turn_id, bool user)
{
    if (ui.at.task == 0U || ui.at.task > LINX_TASK_COUNT) return NULL;
    read_position_t *positions = ui.positions[ui.at.task - 1U];
    for (size_t i = 0U; i < LINX_HISTORY_CAPACITY * 2U; i++) {
        if (positions[i].turn == turn_id && positions[i].user == user) return &positions[i];
    }
    for (size_t i = 0U; i < LINX_HISTORY_CAPACITY * 2U; i++) {
        if (positions[i].turn == 0U || !linx_demo_turn(&ui.demo, ui.at.task, positions[i].turn)) {
            positions[i] = (read_position_t){turn_id, 0U, user};
            return &positions[i];
        }
    }
    positions[0] = (read_position_t){turn_id, 0U, user};
    return &positions[0];
}

static void open_reader(uint32_t turn_id, bool user)
{
    push(LINX_UI_READER);
    ui.at.turn = turn_id;
    ui.at.user_text = user;
    read_position_t *remembered = reading_position(turn_id, user);
    ui.at.read_page = remembered ? remembered->page : 0U;
    if (!user) linx_demo_mark_read(&ui.demo, ui.at.task, turn_id);
}

static void rebuild_history(void)
{
    ui.history_count = 0U;
    const linx_task_t *current = task();
    if (!current) return;
    for (size_t i = 0U; i < current->turn_count; i++) {
        const linx_turn_t *turn = linx_demo_history(&ui.demo, current->id, i);
        if (!turn) continue;
        if (turn->complete && turn->content_id != LINX_CONTENT_NONE)
            ui.history[ui.history_count++] = (history_item_t){turn->id, HISTORY_AGENT};
        if (turn->user_text)
            ui.history[ui.history_count++] = (history_item_t){turn->id, HISTORY_USER};
    }
    for (size_t i = 0U; i < ui.history_count; i++) {
        if (ui.history[i].turn == ui.at.history_turn && ui.history[i].kind == ui.at.history_kind) {
            ui.at.selected = i;
            return;
        }
    }
    if (ui.at.history_turn != 0U || ui.at.selected >= ui.history_count) ui.at.selected = 0U;
    if (ui.history_count > 0U) {
        ui.at.history_turn = ui.history[ui.at.selected].turn;
        ui.at.history_kind = ui.history[ui.at.selected].kind;
    }
}

static bool voice_request(linx_voice_command_t command)
{
    const linx_voice_request_t request = {.command=command, .generation=ui.voice_generation, .context=ui.reply};
    return ui.voice_handler && ui.voice_handler(&request, ui.voice_user);
}

static uint32_t next_voice_generation(void)
{
    if (++voice_sequence == 0U) ++voice_sequence;
    return voice_sequence;
}

static void cancel_prepared_voice(void)
{
    uint32_t generation = ui.connection_generation;
    if (generation != 0U && ui.voice_handler) {
        const linx_voice_request_t request = {.command = LINX_VOICE_RELEASE,
            .generation = generation, .context = ui.prepared_context};
        ui.voice_handler(&request, ui.voice_user);
    }
    ui.connection_generation = 0U;
    ui.prepared_context_valid = false;
}

static void prepare_overview_voice(void)
{
    if (!ui.voice_handler || ui.at.page != LINX_UI_OVERVIEW) return;
    linx_reply_context_t context;
    if (!linx_demo_reply_context(&ui.demo, ui.at.task, 0U, 0U, &context)) return;
    bool same = ui.prepared_context_valid &&
        context.scene_id == ui.prepared_context.scene_id && context.task_id == ui.prepared_context.task_id &&
        context.source_turn_id == ui.prepared_context.source_turn_id && context.image_id == ui.prepared_context.image_id;
    if (ui.rendered_page == LINX_UI_OVERVIEW && same) return;
    const linx_voice_request_t request = {.command = LINX_VOICE_PREPARE,
        .generation = next_voice_generation(), .context = context};
    if (ui.voice_handler(&request, ui.voice_user)) {
        ui.connection_generation = request.generation;
    }
    ui.prepared_context = context;
    ui.prepared_context_valid = true;
}

static void cancel_live_voice(linx_voice_command_t command)
{
    if (ui.voice_generation != 0U) {
        voice_request(command);
        if (command == LINX_VOICE_RELEASE && ui.connection_generation == ui.voice_generation)
            ui.connection_generation = 0U;
        ui.voice_generation = 0U;
    }
}

static void start_voice(void)
{
    ui.at.page = LINX_UI_VOICE;
    ui.at.selected = 0U;
    ui.voice_ms = ui.now_ms;
    ui.last_wave_ms = ui.now_ms;
    if (ui.voice_handler) {
        ui.voice_generation = next_voice_generation();
        ui.voice_state = LINX_VOICE_PREPARING;
        ui.voice_error = LINX_VOICE_OK;
        memset(ui.activation_code, 0, sizeof(ui.activation_code));
        if (!voice_request(LINX_VOICE_START)) {
            ui.voice_state = LINX_VOICE_ERROR;
            ui.voice_error = LINX_VOICE_NO_MEMORY;
        } else {
            ui.connection_generation = ui.voice_generation;
        }
    }
}

static void restore_reply_origin(void)
{
    ui.voice_generation = 0U;
    ui.at = ui.reply_origin;
    ui.depth = ui.reply_depth;
    memset(&ui.reply, 0, sizeof(ui.reply));
    ui.notice = NULL;
}

static void cancel_reply(void)
{
    cancel_live_voice(LINX_VOICE_CANCEL);
    restore_reply_origin();
}

static void begin_reply(void)
{
    linx_reply_gate_t gate = linx_demo_reply_gate(&ui.demo, ui.at.task);
    if (!ui.voice_handler && (gate == LINX_REPLY_PENDING || gate == LINX_REPLY_FAILED)) {
        const linx_task_t *current = task();
        if (current) {
            ui.at.page = LINX_UI_RECEIPT;
            ui.at.command = current->pending.id;
            ui.at.selected = 0U;
        }
        return;
    }
    if ((!ui.voice_handler && gate != LINX_REPLY_ALLOWED) ||
        !linx_demo_reply_context(&ui.demo, ui.at.task, 0U, 0U, &ui.reply)) {
        ui.notice = "这轮已更新，请返回任务";
        return;
    }
    ui.reply_origin = ui.at;
    ui.reply_depth = ui.depth;
    start_voice();
    ui.notice = NULL;
}

static void render_tasks(void)
{
    brand();
    text("我的任务", 16, 42, 160, 33, &linx_font_24_bold, COLOR_INK);
    text("03", 184, 40, 42, 36, &linx_font_28_bold, COLOR_ACCENT);
    shape(17, 83, 5, 5, ui.demo.online ? COLOR_ACCENT : COLOR_MUTED, 0, 2);
    text(ui.demo.online ? "工作电脑 · 模拟在线" : "模拟离线 · 上次快照",
         29, 77, 190, 18, &linx_font_12, COLOR_MUTED);
    stroke(16, 99, 224, 99, COLOR_LINE, 1);
    for (size_t i = 0U; i < LINX_TASK_COUNT; i++) {
        const linx_task_t *current = &ui.demo.tasks[i];
        int y = 103 + (int)i * 57;
        if (i == ui.at.selected) selected_row(y, 53);
        task_icon(i, 27, y + 16, i == ui.at.selected ? COLOR_ACCENT : COLOR_MUTED);
        lv_obj_t *title = text(current->title, 54, y + 4, 151, 23, &linx_font_16_bold, COLOR_INK);
        if (title) lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        text(current->agent, 54, y + 30, 91, 17, &linx_font_12, COLOR_MUTED);
        const char *status = linx_demo_has_unread(&ui.demo, current->id) ? "新回复" : linx_demo_task_status(current);
        text(status, 151, y + 30, 68, 17, &linx_font_12,
             strcmp(status, "本轮结束") == 0 ? COLOR_MUTED : COLOR_ACCENT_TEXT);
        chevron(214, y + 11, i == ui.at.selected ? COLOR_ACCENT_TEXT : COLOR_MUTED);
        if (i + 1U < LINX_TASK_COUNT) stroke(25, y + 55, 224, y + 55, COLOR_LINE, 1);
    }
    footer(ui.voice_handler ? "↑↓选择  OK进入\n双击OK 设备设置" :
           "↑↓选择  OK进入\n双击OK 演示选项");
}

static void overview_enabled(bool enabled[3])
{
    enabled[0] = latest_reply() != NULL;
    enabled[1] = true;
    enabled[2] = task() && task()->turn_count > 0U;
}

static void render_overview(void)
{
    const linx_task_t *current = task();
    if (!current) return;
    char context[96];
    snprintf(context, sizeof(context), "%s · 工作电脑", current->agent);
    header(current->title, context);
    const linx_turn_t *turn = latest_reply();
    const linx_content_t *content = turn ? linx_content_get(turn->content_id) : NULL;
    const char *status = ui.demo.online ? linx_demo_task_status(current) : "模拟离线快照";
    box(16, 102, 104, 22, COLOR_SELECTED, 11);
    text(status, 25, 104, 92, 18, &linx_font_12, COLOR_ACCENT_TEXT);
    if (turn) {
        char round[32];
        snprintf(round, sizeof(round), "第 %u 轮", (unsigned)turn->number);
        lv_obj_t *label = text(round, 165, 104, 59, 18, &linx_font_12, COLOR_MUTED);
        if (label) lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    }
    text(ui.notice ? ui.notice : (content ? content->summary : "正在补充边界测试\n可以先下发下一条指令"),
         16, 132, 208, 44, &linx_font_16, COLOR_INK);
    bool enabled[3];
    overview_enabled(enabled);
    linx_reply_gate_t gate = linx_demo_reply_gate(&ui.demo, ui.at.task);
    const char *reply = gate == LINX_REPLY_FAILED ? "处理未发送指令" :
                        gate == LINX_REPLY_PENDING ? "查看待处理指令" : "继续沟通";
    const char *options[] = {turn ? "阅读本轮回复" : "阅读回复  暂无", reply, "最近对话"};
    for (size_t i = 0U; i < 3U; i++) menu_row(i, ui.at.selected, 178 + (int)i * 25, 24, options[i], enabled[i]);
    footer("↑↓选择  OK打开\n双击OK 返回任务");
}

static const char *reader_source(void)
{
    const linx_turn_t *turn = linx_demo_turn(&ui.demo, ui.at.task, ui.at.turn);
    if (!turn) return ui.back_mode == LINX_BACK_DOUBLE_CLICK ?
        "这轮记录已移出保留范围。\n双击 OK 返回。" : "这轮记录已移出保留范围。\n长按 OK 返回。";
    if (ui.at.user_text) return turn->user_text ? turn->user_text : "没有用户消息";
    const linx_content_t *content = linx_content_get(turn->content_id);
    return content ? content->body : "本轮仍在执行，请稍后查看。";
}

static int32_t reader_measure(const char *value, size_t bytes, linx_reader_kind_t kind, void *context)
{
    (void)context;
    return measure(value, bytes, (void *)(kind == LINX_READER_HEADING ? &linx_font_16_bold : &linx_font_16));
}

static void render_reader(void)
{
    const linx_task_t *current = task();
    const linx_turn_t *turn = linx_demo_turn(&ui.demo, ui.at.task, ui.at.turn);
    const char *source = reader_source();
    bool paginated = linx_reader_paginate(&ui.pages, source, BODY_WIDTH, BODY_HEIGHT, reader_measure, NULL);
    if (ui.at.read_page >= ui.pages.count) ui.at.read_page = ui.pages.count ? ui.pages.count - 1U : 0U;
    text(ui.at.user_text ? "你的指令" : "完整回复", 16, 14, 110, 28, &linx_font_20_bold, COLOR_INK);
    text("演示", 130, 21, 30, 16, &linx_font_12, COLOR_MUTED);
    char count[24];
    snprintf(count, sizeof(count), "%u / %u", (unsigned)(ui.at.read_page + 1U), (unsigned)ui.pages.count);
    shape(177, 17, 47, 21, COLOR_SELECTED, 0, 5);
    lv_obj_t *page_number = text(count, 180, 19, 41, 17, &linx_font_12, COLOR_ACCENT_TEXT);
    if (page_number) lv_obj_set_style_text_align(page_number, LV_TEXT_ALIGN_CENTER, 0);
    char context[128];
    snprintf(context, sizeof(context), "%s · %s · 第%u轮", ui.at.user_text ? "你" : (current ? current->agent : "Agent"),
             current ? current->title : "原任务", (unsigned)(turn ? turn->number : 0U));
    lv_obj_t *subtitle = text(context, 16, 47, 208, 17, &linx_font_12, COLOR_MUTED);
    if (subtitle) lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    stroke(16, 66, 224, 66, COLOR_LINE, 1);
    linx_reader_page_t page;
    if (!paginated || !linx_reader_get_page(&ui.pages, ui.at.read_page, source, reader_measure, NULL, &page)) {
        text("正文排版失败\n请返回任务后重试", 16, BODY_TOP, BODY_WIDTH, 60, &linx_font_16, COLOR_INK);
    } else for (size_t i = 0U; i < page.count; i++) {
        const linx_reader_row_t *row = &page.rows[i];
        if (row->length >= sizeof(ui.page_text)) continue;
        memcpy(ui.page_text, source + row->start, row->length);
        ui.page_text[row->length] = '\0';
        text(ui.page_text, 16 + row->x, BODY_TOP + row->y, BODY_WIDTH - row->x, 22,
             row->kind == LINX_READER_HEADING ? &linx_font_16_bold : &linx_font_16, COLOR_INK);
        if (row->first_list_line) {
            if (source[row->marker_start] == '-' || source[row->marker_start] == '*')
                shape(18, BODY_TOP + row->y + 9, 3, 3, COLOR_ACCENT, 0, 1);
            else if (row->marker_length < sizeof(ui.page_text)) {
                memcpy(ui.page_text, source + row->marker_start, row->marker_length);
                ui.page_text[row->marker_length] = '\0';
                text(ui.page_text, 16, BODY_TOP + row->y + 2, row->x, 18, &linx_font_12, COLOR_ACCENT_TEXT);
            }
        }
    }
    footer("↑↓翻页\n双击OK 返回");
}

static void render_history(void)
{
    rebuild_history();
    const linx_task_t *current = task();
    char context[72];
    snprintf(context, sizeof(context), "保留最近8轮 · %u/%u", (unsigned)(ui.at.selected + 1U), (unsigned)ui.history_count);
    header("最近对话", context);
    size_t first = ui.at.selected >= 3U ? ui.at.selected - 2U : 0U;
    for (size_t i = first; i < ui.history_count && i < first + 3U; i++) {
        history_item_t entry = ui.history[i];
        const linx_turn_t *turn = linx_demo_turn(&ui.demo, ui.at.task, entry.turn);
        if (!turn) continue;
        int y = 101 + (int)(i - first) * 55;
        if (i == ui.at.selected) selected_row(y, 49);
        char title[72];
        const char *speaker = entry.kind == HISTORY_USER ? "你" : current->agent;
        snprintf(title, sizeof(title), "%s · 第%u轮", speaker, (unsigned)turn->number);
        text(title, 26, y + 2, 190, 22, &linx_font_16_bold, COLOR_INK);
        const linx_content_t *content = linx_content_get(turn->content_id);
        const char *snippet = entry.kind == HISTORY_USER ? turn->user_text : (content ? content->summary : "执行中");
        lv_obj_t *preview = text(snippet, 26, y + 25, 190, 19, &linx_font_14, COLOR_MUTED);
        if (preview) lv_label_set_long_mode(preview, LV_LABEL_LONG_DOT);
    }
    footer("↑↓选择  OK查看\n双击OK 返回概览");
}

static void reply_target(char *out, size_t capacity)
{
    const linx_turn_t *turn = linx_demo_turn(&ui.demo, ui.reply.task_id, ui.reply.source_turn_id);
    snprintf(out, capacity, "原任务 · 第%u轮", (unsigned)(turn ? turn->number : 0U));
}

static const char *voice_status(void)
{
    switch (ui.voice_state) {
    case LINX_VOICE_PREPARING: return "正在准备录音";
    case LINX_VOICE_DRAINING: return "正在结束上一条指令";
    case LINX_VOICE_CONNECTING: return "正在连接语音服务";
    case LINX_VOICE_BINDING: return "请在灵矽平台绑定设备";
    case LINX_VOICE_LISTENING: return "麦克风已开启，请说话";
    case LINX_VOICE_WAITING: return "正在等待发送结果";
    case LINX_VOICE_SPEAKING: return "正在播报发送结果";
    case LINX_VOICE_ERROR: return "本次未能完成，按OK重录";
    case LINX_VOICE_CLOSED: return "本轮已结束，按OK重录";
    default: return "正在准备录音";
    }
}

static const char *voice_error(void)
{
    switch (ui.voice_error) {
    case LINX_VOICE_NOT_CONFIGURED: return "请先设置Wi-Fi";
    case LINX_VOICE_WIFI_FAILED: return "Wi-Fi连接失败";
    case LINX_VOICE_TIME_FAILED: return "网络时间同步失败";
    case LINX_VOICE_NETWORK_FAILED: return "灵矽连接已断开";
    case LINX_VOICE_PROTOCOL_FAILED: return "语音协议不兼容";
    case LINX_VOICE_AUDIO_FAILED: return "音频设备无法使用";
    case LINX_VOICE_NO_MEMORY: return "内存不足，请重试";
    case LINX_VOICE_TIMEOUT: return "灵矽回复超时";
    default: return "请检查网络后重试";
    }
}

static void render_voice(void)
{
    const linx_task_t *target = linx_demo_task(&ui.demo, ui.reply.task_id);
    const bool live = ui.voice_handler != NULL;
    header(live ? "发送指令" : "正在聆听", target ? target->title : "原任务");
    text(live ? voice_status() : "模拟输入，不采集声音",
         live ? 16 : 34, 106, live ? 208 : 180, 18, &linx_font_12, COLOR_MUTED);
    if (live && ui.voice_state == LINX_VOICE_BINDING) {
        text("设备激活码", 64, 148, 130, 24, &linx_font_16, COLOR_INK);
        text(ui.activation_code, 40, 183, 160, 36, &linx_font_28_bold, COLOR_ACCENT_TEXT);
        footer("OK绑定后重试\n双击OK 返回");
        return;
    }
    if (live && (ui.voice_state == LINX_VOICE_ERROR || ui.voice_state == LINX_VOICE_CLOSED)) {
        text(voice_error(), 28, 151, 184, 64, &linx_font_16, COLOR_ACCENT_TEXT);
        footer(ui.voice_error == LINX_VOICE_NOT_CONFIGURED ? "OK手机配网\n双击OK 返回" :
               "OK重新录入\n双击OK 返回");
        return;
    }
    unsigned phase = (unsigned)((ui.now_ms - ui.voice_ms) / 125U);
    for (size_t i = 0U; i < 6U; i++) {
        int height = 10 + (int)((phase * 7U + i * 13U) % 34U);
        int x = i < 3U ? 42 + (int)i * 14 : 163 + (int)(i - 3U) * 14;
        box(x, 176 - height / 2, 6, height, COLOR_ACCENT, 3);
    }
    x_mark(100, 155, 40, 7);
    char elapsed[24];
    unsigned seconds = (unsigned)((ui.now_ms - ui.voice_ms) / 1000U);
    snprintf(elapsed, sizeof(elapsed), "%02u:%02u", seconds / 60U, seconds % 60U);
    ui.voice_counter = text(elapsed, 79, 210, 90, 36, &linx_font_28_bold, COLOR_INK);
    char target_context[96];
    reply_target(target_context, sizeof(target_context));
    text(target_context, 28, 252, 184, 18, &linx_font_12, COLOR_MUTED);
    footer(!live ? "OK结束录音\n双击OK 取消" :
           ui.voice_state == LINX_VOICE_LISTENING ? "OK结束并发送\n双击OK 返回" :
           "请稍候\n双击OK 返回");
}

static void update_voice_wave(void)
{
    unsigned phase = (unsigned)((ui.now_ms - ui.voice_ms) / 125U);
    for (size_t i = 0U; i < 6U; i++) {
        int height = 10 + (int)((phase * 7U + i * 13U) % 34U);
        int x = i < 3U ? 42 + (int)i * 14 : 163 + (int)(i - 3U) * 14;
        lv_obj_set_pos(ui.boxes[i], x, 176 - height / 2);
        lv_obj_set_size(ui.boxes[i], 6, height);
    }
    if (ui.voice_counter) {
        unsigned seconds = (unsigned)((ui.now_ms - ui.voice_ms) / 1000U);
        lv_label_set_text_fmt(ui.voice_counter, "%02u:%02u", seconds / 60U, seconds % 60U);
    }
}

static void render_confirm(void)
{
    const linx_task_t *target = linx_demo_task(&ui.demo, ui.reply.task_id);
    header("确认这条指令", target ? target->title : "原任务");
    const char *transcript = linx_content_transcript(ui.reply.task_id, ui.reply.image_id);
    box(16, 104, 208, 80, COLOR_SELECTED, 8);
    text(transcript, 26, 112, 188, 64, &linx_font_16, COLOR_INK);
    char context[96];
    reply_target(context, sizeof(context));
    text(ui.notice ? ui.notice : context, 16, 190, 208, 18, &linx_font_12,
         ui.notice ? COLOR_ACCENT_TEXT : COLOR_MUTED);
    const char *options[] = {"发送到原任务", "重新模拟输入", "取消"};
    for (size_t i = 0U; i < 3U; i++) menu_row(i, ui.at.selected, 211 + (int)i * 23, 22, options[i], true);
    footer("↑↓选择  OK确认\n双击OK 放弃草稿");
}

static void render_receipt(void)
{
    const linx_command_t *command = linx_demo_command(&ui.demo, ui.at.task, ui.at.command);
    const linx_task_t *current = task();
    header("指令回执", current ? current->title : "原任务");
    linx_command_state_t state = command ? command->state : LINX_COMMAND_NONE;
    text(linx_demo_command_status(state), 16, 109, 208, 53, &linx_font_20_bold,
         state == LINX_COMMAND_FAILED ? COLOR_ACCENT_TEXT : COLOR_INK);
    const char *detail = "此处全部为模拟状态";
    if (state == LINX_COMMAND_SENDING) detail = "等待模拟电脑接收\n离开此页仍会继续";
    else if (state == LINX_COMMAND_COMPUTER_RECEIVED) detail = "模拟电脑已收到\n等待 Agent 接收";
    else if (state == LINX_COMMAND_QUEUED) detail = "当前轮结束后执行\n指令已绑定原任务";
    else if (state == LINX_COMMAND_AGENT_ACCEPTED || state == LINX_COMMAND_RUNNING) detail = "原任务继续执行\n可返回查看其他任务";
    else if (state == LINX_COMMAND_FINISHED) detail = "原任务已有新的回复\n按 OK 返回查看";
    else if (state == LINX_COMMAND_FAILED) detail = "电脑尚未接收\n保留原指令，可手动重试";
    text(detail, 16, 156, 208, 44, &linx_font_16, COLOR_MUTED);
    if (state == LINX_COMMAND_FAILED) {
        menu_row(0U, ui.at.selected, 222, 23, "重试同一条指令", true);
        menu_row(1U, ui.at.selected, 246, 23, "取消这条指令", true);
        footer("↑↓选择  OK执行\n双击OK 保留并返回");
    } else footer("OK返回任务概览\n双击OK 返回概览");
}

static void wifi_clear(void)
{
    volatile char *password = ui.wifi_ap_password;
    for (size_t i = 0U; i < sizeof(ui.wifi_ap_password); ++i) password[i] = '\0';
    memset(ui.wifi_ap_ssid, 0, sizeof(ui.wifi_ap_ssid));
}

static void wifi_setup(void)
{
    wifi_clear();
    cancel_prepared_voice();
    ui.voice_generation = next_voice_generation();
    ui.wifi_mode = LINX_UI_WIFI_SCANNING;
    ui.voice_error = LINX_VOICE_OK;
    linx_voice_request_t request = {.command = LINX_VOICE_WIFI_SETUP, .generation = voice_sequence};
    if (!ui.voice_handler || !ui.voice_handler(&request, ui.voice_user)) {
        ui.wifi_mode = LINX_UI_WIFI_ERROR;
        ui.voice_error = LINX_VOICE_NO_MEMORY;
    }
}

static void open_wifi(void)
{
    ui.wifi_origin = ui.at;
    cancel_live_voice(LINX_VOICE_RELEASE);
    ui.at.page = LINX_UI_WIFI;
    ui.notice = NULL;
    wifi_setup();
}

static void close_wifi(void)
{
    cancel_live_voice(LINX_VOICE_RELEASE);
    wifi_clear();
    ui.at = ui.wifi_origin;
    ui.notice = NULL;
    if (ui.at.page == LINX_UI_VOICE) {
        ui.voice_state = LINX_VOICE_ERROR;
        ui.voice_error = LINX_VOICE_NOT_CONFIGURED;
    }
}

static void wifi_ok(void)
{
    if (ui.wifi_mode == LINX_UI_WIFI_ERROR) wifi_setup();
    else if (ui.wifi_mode == LINX_UI_WIFI_CONNECTED) close_wifi();
}

static bool wifi_event(const linx_voice_event_t *event)
{
    switch (event->state) {
    case LINX_VOICE_WIFI_SCANNING: ui.wifi_mode = LINX_UI_WIFI_SCANNING; break;
    case LINX_VOICE_WIFI_PORTAL:
        if (!event->wifi_ap_ssid[0] || !memchr(event->wifi_ap_ssid, '\0', sizeof(event->wifi_ap_ssid)) ||
            event->wifi_ap_password[8] != '\0') return false;
        for (size_t i = 0U; event->wifi_ap_ssid[i]; ++i) {
            unsigned char character = (unsigned char)event->wifi_ap_ssid[i];
            if (character < 32U || character > 126U) return false;
        }
        for (size_t i = 0U; i < 8U; ++i)
            if (event->wifi_ap_password[i] < '0' || event->wifi_ap_password[i] > '9') return false;
        memcpy(ui.wifi_ap_ssid, event->wifi_ap_ssid, sizeof(ui.wifi_ap_ssid));
        memcpy(ui.wifi_ap_password, event->wifi_ap_password, sizeof(ui.wifi_ap_password));
        ui.voice_error = event->error;
        ui.wifi_mode = LINX_UI_WIFI_PHONE;
        break;
    case LINX_VOICE_WIFI_CONNECTING: ui.wifi_mode = LINX_UI_WIFI_CONNECTING; break;
    case LINX_VOICE_WIFI_SAVED:
        wifi_clear();
        if (ui.wifi_origin.page == LINX_UI_VOICE) {
            ui.at = ui.wifi_origin;
            ui.voice_generation = 0U;
            start_voice();
        } else ui.wifi_mode = LINX_UI_WIFI_CONNECTED;
        break;
    case LINX_VOICE_ERROR:
        wifi_clear();
        ui.voice_error = event->error;
        ui.wifi_mode = LINX_UI_WIFI_ERROR;
        break;
    default: return false;
    }
    ui.notice = NULL;
    render();
    return true;
}

static void render_wifi(void)
{
    header("手机配网", "用手机键盘输入Wi-Fi密码");
    if (ui.wifi_mode == LINX_UI_WIFI_PHONE || ui.wifi_mode == LINX_UI_WIFI_CONNECTING) {
        text("1  手机连接热点", 16, 106, 208, 20, &linx_font_14, COLOR_MUTED);
        lv_obj_t *name = text(ui.wifi_ap_ssid, 16, 132, 208, 25, &linx_font_20_bold, COLOR_INK);
        if (name) lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char password[40];
        snprintf(password, sizeof(password), "热点密码：%s", ui.wifi_ap_password);
        text(password, 16, 160, 208, 23, &linx_font_16, COLOR_INK);
        text("2  浏览器打开", 16, 195, 208, 20, &linx_font_14, COLOR_MUTED);
        text("http://192.168.4.1", 16, 219, 208, 22, &linx_font_16, COLOR_ACCENT_TEXT);
        const char *status = ui.wifi_mode == LINX_UI_WIFI_CONNECTING ? "正在连接，请稍候" :
            ui.voice_error != LINX_VOICE_OK ? "连接失败，请在手机上重试" : "选Wi-Fi后连接并保存";
        text(status, 16, 250, 208, 18, &linx_font_12, COLOR_MUTED);
        volatile char *secret = password;
        for (size_t i = 0U; i < sizeof(password); ++i) secret[i] = '\0';
    } else {
        const char *status = ui.wifi_mode == LINX_UI_WIFI_SCANNING ? "正在准备手机配网" :
            ui.wifi_mode == LINX_UI_WIFI_CONNECTED ? "已连接并保存" :
            ui.voice_error == LINX_VOICE_TIMEOUT ? "配网已超时，请重试" : voice_error();
        text(status, 28, 151, 184, 64, &linx_font_16, COLOR_INK);
    }
    footer(ui.wifi_mode == LINX_UI_WIFI_ERROR ? "OK重新配网\n双击OK 返回" :
           ui.wifi_mode == LINX_UI_WIFI_CONNECTED ? "OK返回设置\n双击OK 返回" :
           "5分钟内完成配网\n双击OK 取消");
}

static void render_settings(void)
{
    header(ui.voice_handler ? "设备设置" : "演示选项",
           ui.voice_handler ? "网络与演示选项" : "固定剧情 · 无网络连接");
    const char *options[] = {"恢复初始场景", ui.demo.online ? "模拟电脑：在线" : "模拟电脑：离线",
                              ui.demo.fail_next_send ? "下次发送失败：开" : "下次发送失败：关", "关于 LinX 演示"};
    size_t count = ui.voice_handler ? 6U : 5U;
    for (size_t i = 0U; i < count; ++i) {
        const char *option;
        if (i == count - 1U) option = ui.back_mode == LINX_BACK_DOUBLE_CLICK ? "返回方式：双击" : "返回方式：长按";
        else if (ui.voice_handler && i == 0U) option = "Wi-Fi网络";
        else option = options[i - (ui.voice_handler ? 1U : 0U)];
        menu_row(i, ui.at.selected, (ui.voice_handler ? 106 : 108) + (int)i * (ui.voice_handler ? 26 : 30),
                 ui.voice_handler ? 24 : 27, option, true);
    }
    const char *status = ui.back_mode_pending ? "正在保存，请稍候" : ui.back_mode_notice;
    if (status) text(status, 24, 262, 192, 16, &linx_font_12, COLOR_MUTED);
    footer("↑↓选择  OK切换\n双击OK 返回任务");
}

static void change_back_mode(void)
{
    if (ui.back_mode_pending) return;
    if (!ui.back_mode_handler) {
        ui.back_mode_notice = "无法保存，请重试";
        return;
    }
    ui.pending_back_mode = ui.back_mode == LINX_BACK_LONG_PRESS ? LINX_BACK_DOUBLE_CLICK : LINX_BACK_LONG_PRESS;
    ui.back_mode_pending = true;
    ui.back_mode_notice = NULL;
    if (!ui.back_mode_handler(ui.pending_back_mode, ui.back_mode_user)) {
        ui.back_mode_pending = false;
        ui.back_mode_notice = "保存失败，请重试";
    }
}

static void render_reset(void)
{
    header("恢复初始场景？", "只重置本次模拟内容");
    text("清除历史、未发送指令\n和全部演示计时，\n重新载入三条任务。", 16, 110, 208, 92, &linx_font_16, COLOR_INK);
    menu_row(0U, ui.at.selected, 211, 27, "取消", true);
    menu_row(1U, ui.at.selected, 242, 27, "恢复初始场景", true);
    footer("↑↓选择  OK确认\n双击OK 取消");
}

static void render_about(void)
{
    header("关于 LinX", "随身 Agent 对话终端");
    text("暖白交互演示\n\n任务、文字对话、\n电脑状态与电量均为\n内置模拟内容。\n\n不会采集声音或联网。", 16, 102, 208, 168, &linx_font_16, COLOR_INK);
    footer("OK返回演示选项\n双击OK 返回");
}

static void render(void)
{
    if (!ui.root) return;
    prepare_overview_voice();
    reset_objects();
    switch (ui.at.page) {
        case LINX_UI_BOOT:
            text("Lin", 73, 111, 50, 36, &linx_font_28_bold, COLOR_INK);
            x_mark(127, 119, 25, 5);
            text("离线交互演示", 64, 162, 145, 27, &linx_font_16, COLOR_MUTED);
            text("暖白 · 随身对话", 58, 201, 150, 24, &linx_font_14, COLOR_MUTED);
            break;
        case LINX_UI_TASKS: render_tasks(); break;
        case LINX_UI_OVERVIEW: render_overview(); break;
        case LINX_UI_READER: render_reader(); break;
        case LINX_UI_HISTORY: render_history(); break;
        case LINX_UI_VOICE: render_voice(); break;
        case LINX_UI_CONFIRM: render_confirm(); break;
        case LINX_UI_RECEIPT: render_receipt(); break;
        case LINX_UI_SETTINGS: render_settings(); break;
        case LINX_UI_RESET: render_reset(); break;
        case LINX_UI_ABOUT: render_about(); break;
        case LINX_UI_WIFI: render_wifi(); break;
    }
    lv_obj_invalidate(ui.decor);
    ui.seen_revision = ui.demo.revision;
    ui.rendered_page = ui.at.page;
}

void linx_ui_init(lv_obj_t *screen, uint64_t now_ms)
{
    linx_ui_deinit();
    if (!screen) return;
    ui.now_ms = now_ms;
    ui.boot_ms = now_ms;
    ui.at.page = LINX_UI_BOOT;
    linx_demo_init(&ui.demo, now_ms);
    ui.root = lv_obj_create(screen);
    lv_obj_remove_style_all(ui.root);
    lv_obj_set_pos(ui.root, 0, 0);
    lv_obj_set_size(ui.root, 240, 320);
    lv_obj_remove_flag(ui.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui.root, lv_color_hex(COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(ui.root, LV_OPA_COVER, 0);
    for (size_t i = 0U; i < BOX_COUNT; i++) {
        ui.boxes[i] = lv_obj_create(ui.root);
        lv_obj_remove_style_all(ui.boxes[i]);
        lv_obj_remove_flag(ui.boxes[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(ui.boxes[i], LV_OPA_COVER, 0);
    }
    ui.decor = lv_obj_create(ui.root);
    lv_obj_remove_style_all(ui.decor);
    lv_obj_remove_flag(ui.decor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ui.decor, 240, 320);
    lv_obj_add_event_cb(ui.decor, draw_decorations, LV_EVENT_DRAW_MAIN, NULL);
    for (size_t i = 0U; i < LABEL_COUNT; i++) {
        ui.labels[i] = lv_label_create(ui.root);
        lv_obj_remove_style_all(ui.labels[i]);
        lv_obj_remove_flag(ui.labels[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    render();
}

void linx_ui_deinit(void)
{
    cancel_live_voice(LINX_VOICE_RELEASE);
    cancel_prepared_voice();
    wifi_clear();
    if (ui.root) lv_obj_delete(ui.root);
    memset(&ui, 0, sizeof(ui));
}

void linx_ui_set_voice_handler(linx_voice_handler_t handler, void *user)
{
    if (ui.at.page == LINX_UI_WIFI) close_wifi();
    if (ui.at.page == LINX_UI_VOICE || ui.at.page == LINX_UI_CONFIRM) {
        cancel_live_voice(LINX_VOICE_RELEASE);
        restore_reply_origin();
    }
    cancel_prepared_voice();
    ui.voice_handler = handler;
    ui.voice_user = user;
    if (ui.at.page == LINX_UI_SETTINGS && ui.at.selected >= (handler ? 6U : 5U))
        ui.at.selected = handler ? 5U : 4U;
    render();
}

void linx_ui_set_back_mode(linx_back_mode_t mode)
{
    if ((unsigned)mode > LINX_BACK_DOUBLE_CLICK) return;
    ui.back_mode = mode;
    ui.back_mode_pending = false;
    ui.back_mode_notice = NULL;
    render();
}

linx_back_mode_t linx_ui_back_mode(void)
{
    return ui.back_mode;
}

void linx_ui_set_back_mode_handler(linx_ui_back_mode_handler_t handler, void *user)
{
    ui.back_mode_handler = handler;
    ui.back_mode_user = user;
}

void linx_ui_back_mode_result(linx_back_mode_t mode, bool saved)
{
    if (!ui.back_mode_pending || mode != ui.pending_back_mode) return;
    ui.back_mode_pending = false;
    if (saved) ui.back_mode = mode;
    ui.back_mode_notice = saved ? "已保存" : "保存失败，请重试";
    render();
}

bool linx_ui_voice_event(const linx_voice_event_t *event)
{
    if (!event || !ui.voice_handler || ui.voice_generation == 0U ||
        event->generation != ui.voice_generation || (unsigned)event->error > LINX_VOICE_TIMEOUT) return false;
    if (ui.at.page == LINX_UI_WIFI) return wifi_event(event);
    if (ui.at.page != LINX_UI_VOICE ||
        ((unsigned)event->state > LINX_VOICE_CLOSED && event->state != LINX_VOICE_PREPARING &&
         event->state != LINX_VOICE_DRAINING)) return false;
    if (event->state == LINX_VOICE_FINISHED &&
        (event->error != LINX_VOICE_OK ||
         (ui.voice_state != LINX_VOICE_WAITING && ui.voice_state != LINX_VOICE_SPEAKING))) return false;
    if (event->state == LINX_VOICE_BINDING) {
        if (event->activation_code[6] != '\0') return false;
        for (size_t i = 0U; i < 6U; ++i) {
            if (event->activation_code[i] < '0' || event->activation_code[i] > '9') return false;
        }
    }
    if (event->state == LINX_VOICE_LISTENING && ui.voice_state != LINX_VOICE_LISTENING) {
        ui.voice_ms = ui.now_ms;
        ui.last_wave_ms = ui.now_ms;
    }
    ui.voice_state = event->state;
    ui.voice_error = event->error;
    memcpy(ui.activation_code, event->activation_code, sizeof(ui.activation_code));
    if (event->state == LINX_VOICE_FINISHED) restore_reply_origin();
    render();
    return true;
}

static void handle_back(void)
{
    switch (ui.at.page) {
        case LINX_UI_BOOT: ui.at.page = LINX_UI_TASKS; break;
        case LINX_UI_TASKS: push(LINX_UI_SETTINGS); break;
        case LINX_UI_OVERVIEW: pop(); break;
        case LINX_UI_WIFI: close_wifi(); break;
        case LINX_UI_VOICE:
        case LINX_UI_CONFIRM: cancel_reply(); break;
        case LINX_UI_RECEIPT: to_overview(); break;
        default: pop(); break;
    }
}

static void handle_direction(int direction)
{
    switch (ui.at.page) {
        case LINX_UI_TASKS: ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, LINX_TASK_COUNT); break;
        case LINX_UI_OVERVIEW: {
            bool enabled[3];
            overview_enabled(enabled);
            ui.at.selected = linx_menu_step(ui.at.selected, direction, enabled, 3U);
            break;
        }
        case LINX_UI_READER: {
            ui.at.read_page = linx_menu_step(ui.at.read_page, direction, NULL, ui.pages.count);
            read_position_t *position = reading_position(ui.at.turn, ui.at.user_text);
            if (position) position->page = ui.at.read_page;
            break;
        }
        case LINX_UI_HISTORY:
            rebuild_history();
            ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, ui.history_count);
            if (ui.history_count > 0U) {
                ui.at.history_turn = ui.history[ui.at.selected].turn;
                ui.at.history_kind = ui.history[ui.at.selected].kind;
            }
            break;
        case LINX_UI_CONFIRM: ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, 3U); break;
        case LINX_UI_RECEIPT: {
            const linx_command_t *command = linx_demo_command(&ui.demo, ui.at.task, ui.at.command);
            if (command && command->state == LINX_COMMAND_FAILED)
                ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, 2U);
            break;
        }
        case LINX_UI_SETTINGS: ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, ui.voice_handler ? 6U : 5U); break;
        case LINX_UI_WIFI: break;
        case LINX_UI_RESET: ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, 2U); break;
        case LINX_UI_VOICE: break;
        default: break;
    }
}

static void handle_ok(void)
{
    switch (ui.at.page) {
        case LINX_UI_BOOT: ui.at.page = LINX_UI_TASKS; break;
        case LINX_UI_TASKS: {
            uint32_t id = ui.demo.tasks[ui.at.selected].id;
            push(LINX_UI_OVERVIEW);
            ui.at.task = id;
            linx_demo_enter_task(&ui.demo, id, ui.now_ms);
            ui.at.selected = latest_reply() ? 0U : 1U;
            break;
        }
        case LINX_UI_OVERVIEW:
            if (ui.at.selected == 0U) {
                const linx_turn_t *turn = latest_reply();
                if (turn) open_reader(turn->id, false);
            } else if (ui.at.selected == 1U) begin_reply();
            else if (ui.at.selected == 2U) {
                push(LINX_UI_HISTORY);
                ui.at.history_turn = 0U;
                rebuild_history();
            }
            break;
        case LINX_UI_READER: break;
        case LINX_UI_HISTORY:
            rebuild_history();
            if (ui.history_count > 0U) {
                history_item_t item = ui.history[ui.at.selected];
                open_reader(item.turn, item.kind == HISTORY_USER);
            }
            break;
        case LINX_UI_VOICE:
            if (ui.voice_handler) {
                if (ui.voice_state == LINX_VOICE_LISTENING) {
                    if (voice_request(LINX_VOICE_FINISH)) ui.voice_state = LINX_VOICE_WAITING;
                    else {
                        ui.voice_state = LINX_VOICE_ERROR;
                        ui.voice_error = LINX_VOICE_NO_MEMORY;
                    }
                } else if (ui.voice_state == LINX_VOICE_ERROR && ui.voice_error == LINX_VOICE_NOT_CONFIGURED) open_wifi();
                else if (ui.voice_state == LINX_VOICE_ERROR || ui.voice_state == LINX_VOICE_CLOSED ||
                           ui.voice_state == LINX_VOICE_BINDING) start_voice();
            } else {
                ui.at.page = LINX_UI_CONFIRM;
                ui.at.selected = 0U;
            }
            break;
        case LINX_UI_CONFIRM:
            if (ui.at.selected == 0U) {
                uint32_t command = linx_demo_send(&ui.demo, &ui.reply, ui.now_ms);
                if (command != 0U) {
                    ui.at.page = LINX_UI_RECEIPT;
                    ui.at.command = command;
                    ui.at.task = ui.reply.task_id;
                    ui.at.selected = 0U;
                    ui.notice = NULL;
                } else ui.notice = "已有指令，请保留并返回";
            } else if (ui.at.selected == 1U) {
                start_voice();
            } else cancel_reply();
            break;
        case LINX_UI_RECEIPT: {
            const linx_command_t *command = linx_demo_command(&ui.demo, ui.at.task, ui.at.command);
            if (command && command->state == LINX_COMMAND_FAILED) {
                if (ui.at.selected == 0U) {
                    linx_demo_retry(&ui.demo, ui.at.task, ui.at.command, ui.now_ms);
                } else {
                    linx_demo_cancel_failed(&ui.demo, ui.at.task, ui.at.command);
                    to_overview();
                }
            } else to_overview();
            break;
        }
        case LINX_UI_WIFI: wifi_ok(); break;
        case LINX_UI_SETTINGS: {
            size_t selected = ui.at.selected;
            if (selected == (ui.voice_handler ? 5U : 4U)) { change_back_mode(); break; }
            if (ui.voice_handler) {
                if (selected == 0U) { open_wifi(); break; }
                --selected;
            }
            if (selected == 0U) push(LINX_UI_RESET);
            else if (selected == 1U) linx_demo_set_online(&ui.demo, !ui.demo.online, ui.now_ms);
            else if (selected == 2U) linx_demo_fail_next_send(&ui.demo, !ui.demo.fail_next_send);
            else push(LINX_UI_ABOUT);
            break;
        }
        case LINX_UI_RESET:
            if (ui.at.selected == 1U) {
                linx_demo_reset(&ui.demo, ui.now_ms);
                memset(ui.positions, 0, sizeof(ui.positions));
                memset(&ui.reply, 0, sizeof(ui.reply));
                memset(&ui.at, 0, sizeof(ui.at));
                ui.at.page = LINX_UI_TASKS;
                ui.depth = 0U;
                ui.notice = NULL;
            } else pop();
            break;
        case LINX_UI_ABOUT: pop(); break;
    }
}

void linx_ui_handle_key(linx_ui_key_t key)
{
    if (!ui.root) return;
    if (key == LINX_UI_KEY_BACK) handle_back();
    else if (key == LINX_UI_KEY_OK) handle_ok();
    else if (key == LINX_UI_KEY_UP) handle_direction(-1);
    else if (key == LINX_UI_KEY_DOWN) handle_direction(1);
    render();
}

void linx_ui_tick(uint64_t now_ms)
{
    if (!ui.root) return;
    ui.now_ms = now_ms;
    linx_demo_tick(&ui.demo, now_ms);
    bool refresh = ui.demo.revision != ui.seen_revision;
    if (ui.at.page == LINX_UI_BOOT && now_ms - ui.boot_ms >= 800U) {
        ui.at.page = LINX_UI_TASKS;
        refresh = true;
    }
    if (ui.at.page == LINX_UI_VOICE) {
        if (!ui.voice_handler && now_ms - ui.voice_ms >= 8000U) {
            ui.at.page = LINX_UI_CONFIRM;
            ui.at.selected = 0U;
            refresh = true;
        } else if (now_ms - ui.last_wave_ms >= 125U &&
                   (!ui.voice_handler || ui.voice_state == LINX_VOICE_LISTENING)) {
            ui.last_wave_ms = now_ms;
            if (!refresh) update_voice_wave();
        }
    }
    if (refresh) render();
}

linx_ui_page_t linx_ui_page(void)
{
    return ui.at.page;
}

const linx_demo_t *linx_ui_demo(void)
{
    return &ui.demo;
}

linx_ui_debug_t linx_ui_debug(void)
{
    linx_ui_debug_t result = {
        .page = ui.at.page,
        .task_id = ui.at.task,
        .turn_id = ui.at.turn,
        .command_id = ui.at.command,
        .selected = ui.at.selected,
        .read_page = ui.at.read_page,
        .read_page_count = ui.pages.count,
        .render_overflow = ui.render_overflow,
        .labels_used = ui.labels_used,
        .voice_generation = ui.voice_generation,
        .voice_state = ui.voice_state,
        .voice_error = ui.voice_error,
        .wifi_mode = ui.wifi_mode,
    };
    if (ui.at.page == LINX_UI_VOICE || ui.at.page == LINX_UI_CONFIRM) {
        result.image_id = ui.reply.image_id;
        result.turn_id = ui.reply.source_turn_id;
        result.task_id = ui.reply.task_id;
    }
    return result;
}
