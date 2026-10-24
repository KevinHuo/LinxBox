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
#define HISTORY_ITEMS (LINX_HISTORY_CAPACITY * 3U)
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

typedef enum { HISTORY_AGENT, HISTORY_USER, HISTORY_IMAGES } history_kind_t;

typedef struct {
    linx_ui_page_t page;
    uint32_t task;
    uint32_t turn;
    uint32_t filter;
    uint32_t command;
    uint32_t history_turn;
    history_kind_t history_kind;
    size_t selected;
    size_t read_page;
    size_t image_index;
    uint32_t image_id;
    uint32_t image_turn;
    unsigned image_view;
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
    lv_obj_t *image;
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
    read_position_t positions[LINX_TASK_COUNT][LINX_HISTORY_CAPACITY * 2U];
    linx_image_ref_t remembered_image[LINX_TASK_COUNT];
    linx_reader_layout_t pages;
    history_item_t history[HISTORY_ITEMS];
    size_t history_count;
    linx_image_ref_t images[LINX_HISTORY_CAPACITY * LINX_IMAGE_COUNT];
    size_t image_count;
    char page_text[1024];
    uint64_t now_ms;
    uint64_t boot_ms;
    uint64_t voice_ms;
    uint64_t last_wave_ms;
    uint32_t seen_revision;
    const char *notice;
} ui;

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
    set_hidden(ui.image, true);
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
    text("演示", 141, 18, 25, 16, &linx_font_12, COLOR_MUTED);
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
    const char *secondary = strstr(value, "演示选项") ? "长按选项" :
                            strstr(value, "取消") ? "长按取消" :
                            strstr(value, "放弃") ? "长按放弃" : "长按返回";
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
        if (turn->image_count > 0U)
            ui.history[ui.history_count++] = (history_item_t){turn->id, HISTORY_IMAGES};
        if (turn->user_text)
            ui.history[ui.history_count++] = (history_item_t){turn->id, HISTORY_USER};
    }
    for (size_t i = 0U; i < ui.history_count; i++) {
        if (ui.history[i].turn == ui.at.history_turn && ui.history[i].kind == ui.at.history_kind) {
            ui.at.selected = i;
            return;
        }
    }
    if (ui.at.selected >= ui.history_count) ui.at.selected = 0U;
    if (ui.history_count > 0U) {
        ui.at.history_turn = ui.history[ui.at.selected].turn;
        ui.at.history_kind = ui.history[ui.at.selected].kind;
    }
}

static void rebuild_images(void)
{
    ui.image_count = linx_demo_images(&ui.demo, ui.at.task, ui.at.filter, ui.images,
                                     sizeof(ui.images) / sizeof(ui.images[0]));
    if (ui.at.image_id != 0U) {
        for (size_t i = 0U; i < ui.image_count; i++) {
            if (ui.images[i].image_id == ui.at.image_id && ui.images[i].turn_id == ui.at.image_turn) {
                ui.at.image_index = i;
                return;
            }
        }
    }
    if (ui.at.image_index >= ui.image_count) ui.at.image_index = 0U;
    if (ui.image_count > 0U) {
        ui.at.image_id = ui.images[ui.at.image_index].image_id;
        ui.at.image_turn = ui.images[ui.at.image_index].turn_id;
    }
}

static const linx_image_ref_t *current_image(void)
{
    rebuild_images();
    return ui.image_count > 0U ? &ui.images[ui.at.image_index] : NULL;
}

static void open_images(uint32_t filter)
{
    push(LINX_UI_IMAGES);
    ui.at.filter = filter;
    linx_image_ref_t remembered = ui.remembered_image[ui.at.task - 1U];
    ui.at.image_index = 0U;
    ui.at.image_id = filter == 0U ? remembered.image_id : 0U;
    ui.at.image_turn = filter == 0U ? remembered.turn_id : 0U;
    rebuild_images();
}

static void cancel_reply(void)
{
    ui.at = ui.reply_origin;
    ui.depth = ui.reply_depth;
    memset(&ui.reply, 0, sizeof(ui.reply));
    ui.notice = NULL;
}

static void begin_reply(uint32_t source_turn, uint32_t image_id)
{
    linx_reply_gate_t gate = linx_demo_reply_gate(&ui.demo, ui.at.task);
    if (gate == LINX_REPLY_PENDING || gate == LINX_REPLY_FAILED) {
        const linx_task_t *current = task();
        if (current) {
            ui.at.page = LINX_UI_RECEIPT;
            ui.at.command = current->pending.id;
            ui.at.selected = 0U;
        }
        return;
    }
    if (gate != LINX_REPLY_ALLOWED ||
        !linx_demo_reply_context(&ui.demo, ui.at.task, source_turn, image_id, &ui.reply)) {
        ui.notice = "这轮已更新，请返回任务";
        return;
    }
    ui.reply_origin = ui.at;
    ui.reply_depth = ui.depth;
    ui.at.page = LINX_UI_VOICE;
    ui.at.selected = 0U;
    ui.voice_ms = ui.now_ms;
    ui.last_wave_ms = ui.now_ms;
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
    footer("↑↓选择  OK进入\n长按OK 演示选项");
}

static void overview_enabled(bool enabled[4])
{
    enabled[0] = latest_reply() != NULL;
    enabled[1] = true;
    enabled[2] = task() && task()->turn_count > 0U;
    enabled[3] = linx_demo_images(&ui.demo, ui.at.task, 0U, NULL, 0U) > 0U;
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
    bool enabled[4];
    overview_enabled(enabled);
    char image_label[48];
    size_t count = linx_demo_images(&ui.demo, ui.at.task, 0U, NULL, 0U);
    snprintf(image_label, sizeof(image_label), "查看图片  %u 张", (unsigned)count);
    linx_reply_gate_t gate = linx_demo_reply_gate(&ui.demo, ui.at.task);
    const char *reply = gate == LINX_REPLY_FAILED ? "处理未发送指令" :
                        gate == LINX_REPLY_PENDING ? "查看待处理指令" : "继续沟通";
    const char *options[] = {turn ? "阅读本轮回复" : "阅读回复  暂无", reply, "最近对话", image_label};
    for (size_t i = 0U; i < 4U; i++) menu_row(i, ui.at.selected, 178 + (int)i * 25, 24, options[i], enabled[i]);
    footer("↑↓选择  OK打开\n长按OK 返回任务");
}

static const char *reader_source(void)
{
    const linx_turn_t *turn = linx_demo_turn(&ui.demo, ui.at.task, ui.at.turn);
    if (!turn) return "这轮记录已移出保留范围。\n长按 OK 返回。";
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
    footer(ui.at.user_text ? "↑↓翻页  OK返回\n长按OK 返回" : "↑↓翻页  OK回复\n长按OK 返回");
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
        const char *speaker = entry.kind == HISTORY_USER ? "你" :
                              entry.kind == HISTORY_IMAGES ? "图片附件" : current->agent;
        snprintf(title, sizeof(title), "%s · 第%u轮", speaker, (unsigned)turn->number);
        text(title, 26, y + 2, 190, 22, &linx_font_16_bold, COLOR_INK);
        const linx_content_t *content = linx_content_get(turn->content_id);
        const char *snippet = entry.kind == HISTORY_USER ? turn->user_text :
                              entry.kind == HISTORY_IMAGES ? "查看这一轮的图片" : (content ? content->summary : "执行中");
        lv_obj_t *preview = text(snippet, 26, y + 25, 190, 19, &linx_font_14, COLOR_MUTED);
        if (preview) lv_label_set_long_mode(preview, LV_LABEL_LONG_DOT);
    }
    footer("↑↓选择  OK查看\n长按OK 返回概览");
}

static void draw_image(unsigned view)
{
    const linx_image_ref_t *image = current_image();
    const lv_image_dsc_t *asset = image ? linx_asset_image(image->image_id, view) : NULL;
    if (!asset) {
        text("图片暂不可用\n仍可阅读配套说明", 25, 152, 190, 64, &linx_font_16, COLOR_MUTED);
        return;
    }
    set_hidden(ui.image, false);
    lv_image_set_src(ui.image, asset);
    lv_obj_set_pos(ui.image, (240 - (int)asset->header.w) / 2, 100);
}

static void render_images(void)
{
    const linx_image_ref_t *image = current_image();
    const linx_image_info_t *info = image ? linx_content_image(image->image_id) : NULL;
    char context[72];
    snprintf(context, sizeof(context), "第%u轮附件 · %u/%u", (unsigned)(image ? image->turn_number : 0U),
             (unsigned)(ui.at.image_index + 1U), (unsigned)ui.image_count);
    header(info ? info->name : "图片附件", context);
    draw_image(0U);
    footer("↑↓切图  OK操作\n长按OK 返回");
}

static void render_image_menu(void)
{
    const linx_image_ref_t *image = current_image();
    const linx_image_info_t *info = image ? linx_content_image(image->image_id) : NULL;
    header("图片操作", info ? info->name : "图片暂不可用");
    const char *options[] = {"查看上下局部", "针对这张图回复", "返回图片"};
    for (size_t i = 0U; i < 3U; i++) menu_row(i, ui.at.selected, 119 + (int)i * 42, 36, options[i], true);
    footer("↑↓选择  OK确认\n长按OK 返回图片");
}

static void render_image_detail(void)
{
    const linx_image_ref_t *image = current_image();
    const linx_image_info_t *info = image ? linx_content_image(image->image_id) : NULL;
    header(info ? info->name : "图片局部", ui.at.image_view == 1U ? "上部局部 · 1/2" : "下部局部 · 2/2");
    lv_obj_t *viewport = box(15, 99, 210, 170, COLOR_PAPER, 1);
    if (viewport) {
        lv_obj_set_style_border_color(viewport, lv_color_hex(COLOR_LINE), 0);
        lv_obj_set_style_border_width(viewport, 1, 0);
    }
    draw_image(ui.at.image_view);
    footer("↑↓看局部  OK回复\n长按OK 返回图片");
}

static void reply_target(char *out, size_t capacity)
{
    const linx_image_info_t *info = ui.reply.image_id ? linx_content_image(ui.reply.image_id) : NULL;
    const linx_turn_t *turn = linx_demo_turn(&ui.demo, ui.reply.task_id, ui.reply.source_turn_id);
    if (info) snprintf(out, capacity, "关联图片：%s", info->name);
    else snprintf(out, capacity, "原任务 · 第%u轮", (unsigned)(turn ? turn->number : 0U));
}

static void render_voice(void)
{
    const linx_task_t *target = linx_demo_task(&ui.demo, ui.reply.task_id);
    header("正在聆听", target ? target->title : "原任务");
    text("模拟输入，不采集声音", 34, 106, 180, 18, &linx_font_12, COLOR_MUTED);
    unsigned phase = (unsigned)((ui.now_ms - ui.voice_ms) / 125U);
    for (size_t i = 0U; i < 6U; i++) {
        int height = 10 + (int)((phase * 7U + i * 13U) % 34U);
        int x = i < 3U ? 42 + (int)i * 14 : 163 + (int)(i - 3U) * 14;
        box(x, 176 - height / 2, 6, height, COLOR_ACCENT, 3);
    }
    x_mark(100, 155, 40, 7);
    char elapsed[24];
    unsigned seconds = (unsigned)((ui.now_ms - ui.voice_ms) / 1000U);
    snprintf(elapsed, sizeof(elapsed), "00:%02u", seconds);
    ui.voice_counter = text(elapsed, 79, 210, 90, 36, &linx_font_28_bold, COLOR_INK);
    char target_context[96];
    reply_target(target_context, sizeof(target_context));
    text(target_context, 28, 252, 184, 18, &linx_font_12, COLOR_MUTED);
    footer("OK结束录音  ↓取消\n长按OK 取消");
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
        lv_label_set_text_fmt(ui.voice_counter, "00:%02u", seconds);
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
    footer("↑↓选择  OK确认\n长按OK 放弃草稿");
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
    if (command && command->context.image_id) {
        const linx_image_info_t *info = linx_content_image(command->context.image_id);
        text(info ? info->name : "关联原图片", 16, 201, 208, 19, &linx_font_14, COLOR_MUTED);
    }
    if (state == LINX_COMMAND_FAILED) {
        menu_row(0U, ui.at.selected, 222, 23, "重试同一条指令", true);
        menu_row(1U, ui.at.selected, 246, 23, "取消这条指令", true);
        footer("↑↓选择  OK执行\n长按OK 保留并返回");
    } else footer("OK返回任务概览\n长按OK 返回概览");
}

static void render_settings(void)
{
    header("演示选项", "固定剧情 · 无网络连接");
    const char *options[] = {"恢复初始场景", ui.demo.online ? "模拟电脑：在线" : "模拟电脑：离线",
                              ui.demo.fail_next_send ? "下次发送失败：开" : "下次发送失败：关", "关于 LinX 演示"};
    for (size_t i = 0U; i < 4U; i++) menu_row(i, ui.at.selected, 110 + (int)i * 37, 33, options[i], true);
    footer("↑↓选择  OK切换\n长按OK 返回任务");
}

static void render_reset(void)
{
    header("恢复初始场景？", "只重置本次模拟内容");
    text("清除历史、未发送指令\n和全部演示计时，\n重新载入三条任务。", 16, 110, 208, 92, &linx_font_16, COLOR_INK);
    menu_row(0U, ui.at.selected, 211, 27, "取消", true);
    menu_row(1U, ui.at.selected, 242, 27, "恢复初始场景", true);
    footer("↑↓选择  OK确认\n长按OK 取消");
}

static void render_about(void)
{
    header("关于 LinX", "随身 Agent 对话终端");
    text("暖白交互演示\n\n任务、对话、图片、\n电脑状态与电量均为\n内置模拟内容。\n\n不会采集声音或联网。", 16, 102, 208, 168, &linx_font_16, COLOR_INK);
    footer("OK返回演示选项\n长按OK 返回");
}

static void render(void)
{
    if (!ui.root) return;
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
        case LINX_UI_IMAGES: render_images(); break;
        case LINX_UI_IMAGE_MENU: render_image_menu(); break;
        case LINX_UI_IMAGE_DETAIL: render_image_detail(); break;
        case LINX_UI_VOICE: render_voice(); break;
        case LINX_UI_CONFIRM: render_confirm(); break;
        case LINX_UI_RECEIPT: render_receipt(); break;
        case LINX_UI_SETTINGS: render_settings(); break;
        case LINX_UI_RESET: render_reset(); break;
        case LINX_UI_ABOUT: render_about(); break;
    }
    lv_obj_invalidate(ui.decor);
    ui.seen_revision = ui.demo.revision;
}

void linx_ui_init(lv_obj_t *screen, uint64_t now_ms)
{
    linx_ui_deinit();
    memset(&ui, 0, sizeof(ui));
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
    ui.image = lv_image_create(ui.root);
    for (size_t i = 0U; i < LABEL_COUNT; i++) {
        ui.labels[i] = lv_label_create(ui.root);
        lv_obj_remove_style_all(ui.labels[i]);
        lv_obj_remove_flag(ui.labels[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    render();
}

void linx_ui_deinit(void)
{
    if (ui.root) lv_obj_delete(ui.root);
    memset(&ui, 0, sizeof(ui));
}

static void handle_back(void)
{
    switch (ui.at.page) {
        case LINX_UI_BOOT: ui.at.page = LINX_UI_TASKS; break;
        case LINX_UI_TASKS: push(LINX_UI_SETTINGS); break;
        case LINX_UI_VOICE:
        case LINX_UI_CONFIRM: cancel_reply(); break;
        case LINX_UI_RECEIPT: to_overview(); break;
        case LINX_UI_IMAGE_DETAIL:
            pop();
            if (ui.at.page == LINX_UI_IMAGE_MENU) pop();
            break;
        default: pop(); break;
    }
}

static void handle_direction(int direction)
{
    switch (ui.at.page) {
        case LINX_UI_TASKS: ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, LINX_TASK_COUNT); break;
        case LINX_UI_OVERVIEW: {
            bool enabled[4];
            overview_enabled(enabled);
            ui.at.selected = linx_menu_step(ui.at.selected, direction, enabled, 4U);
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
        case LINX_UI_IMAGES:
            rebuild_images();
            ui.at.image_index = linx_menu_step(ui.at.image_index, direction, NULL, ui.image_count);
            if (ui.image_count > 0U) {
                const linx_image_ref_t *image = &ui.images[ui.at.image_index];
                ui.at.image_id = image->image_id;
                ui.at.image_turn = image->turn_id;
                if (ui.at.filter == 0U) ui.remembered_image[ui.at.task - 1U] = *image;
            }
            break;
        case LINX_UI_IMAGE_DETAIL: ui.at.image_view = direction > 0 ? 2U : 1U; break;
        case LINX_UI_IMAGE_MENU:
        case LINX_UI_CONFIRM: ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, 3U); break;
        case LINX_UI_RECEIPT: {
            const linx_command_t *command = linx_demo_command(&ui.demo, ui.at.task, ui.at.command);
            if (command && command->state == LINX_COMMAND_FAILED)
                ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, 2U);
            break;
        }
        case LINX_UI_SETTINGS: ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, 4U); break;
        case LINX_UI_RESET: ui.at.selected = linx_menu_step(ui.at.selected, direction, NULL, 2U); break;
        case LINX_UI_VOICE: if (direction > 0) cancel_reply(); break;
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
            } else if (ui.at.selected == 1U) begin_reply(0U, 0U);
            else if (ui.at.selected == 2U) {
                push(LINX_UI_HISTORY);
                ui.at.history_turn = 0U;
                rebuild_history();
            } else if (ui.at.selected == 3U && linx_demo_images(&ui.demo, ui.at.task, 0U, NULL, 0U) > 0U) open_images(0U);
            break;
        case LINX_UI_READER:
            if (ui.at.user_text) pop();
            else begin_reply(ui.at.turn, 0U);
            break;
        case LINX_UI_HISTORY:
            rebuild_history();
            if (ui.history_count > 0U) {
                history_item_t item = ui.history[ui.at.selected];
                if (item.kind == HISTORY_IMAGES) open_images(item.turn);
                else open_reader(item.turn, item.kind == HISTORY_USER);
            }
            break;
        case LINX_UI_IMAGES:
            if (current_image()) {
                push(LINX_UI_IMAGE_MENU);
                ui.at.selected = 1U;
            }
            break;
        case LINX_UI_IMAGE_MENU:
            if (ui.at.selected == 0U) {
                push(LINX_UI_IMAGE_DETAIL);
                ui.at.image_view = 1U;
            } else if (ui.at.selected == 1U) {
                const linx_image_ref_t *image = current_image();
                if (image) begin_reply(image->turn_id, image->image_id);
            } else pop();
            break;
        case LINX_UI_IMAGE_DETAIL: {
            const linx_image_ref_t *image = current_image();
            if (image) begin_reply(image->turn_id, image->image_id);
            break;
        }
        case LINX_UI_VOICE:
            ui.at.page = LINX_UI_CONFIRM;
            ui.at.selected = 0U;
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
                ui.at.page = LINX_UI_VOICE;
                ui.voice_ms = ui.now_ms;
                ui.last_wave_ms = ui.now_ms;
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
        case LINX_UI_SETTINGS:
            if (ui.at.selected == 0U) push(LINX_UI_RESET);
            else if (ui.at.selected == 1U) linx_demo_set_online(&ui.demo, !ui.demo.online, ui.now_ms);
            else if (ui.at.selected == 2U) linx_demo_fail_next_send(&ui.demo, !ui.demo.fail_next_send);
            else push(LINX_UI_ABOUT);
            break;
        case LINX_UI_RESET:
            if (ui.at.selected == 1U) {
                linx_demo_reset(&ui.demo, ui.now_ms);
                memset(ui.positions, 0, sizeof(ui.positions));
                memset(ui.remembered_image, 0, sizeof(ui.remembered_image));
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
    if (key == LINX_UI_KEY_OK_LONG) handle_back();
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
        if (now_ms - ui.voice_ms >= 8000U) {
            ui.at.page = LINX_UI_CONFIRM;
            ui.at.selected = 0U;
            refresh = true;
        } else if (now_ms - ui.last_wave_ms >= 125U) {
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
        .image_view = ui.at.image_view,
        .render_overflow = ui.render_overflow,
        .labels_used = ui.labels_used,
    };
    if (ui.at.page == LINX_UI_IMAGES || ui.at.page == LINX_UI_IMAGE_MENU || ui.at.page == LINX_UI_IMAGE_DETAIL) {
        const linx_image_ref_t *image = current_image();
        if (image) {
            result.image_id = image->image_id;
            result.turn_id = image->turn_id;
        }
    } else if (ui.at.page == LINX_UI_VOICE || ui.at.page == LINX_UI_CONFIRM) {
        result.image_id = ui.reply.image_id;
        result.turn_id = ui.reply.source_turn_id;
        result.task_id = ui.reply.task_id;
    }
    return result;
}
