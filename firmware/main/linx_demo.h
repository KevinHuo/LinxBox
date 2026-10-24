#ifndef LINX_DEMO_H
#define LINX_DEMO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "linx_content.h"

#define LINX_TASK_COUNT 3U
#define LINX_HISTORY_CAPACITY 8U
#define LINX_TASK_LOGIN 1U
#define LINX_TASK_PAYMENT 2U
#define LINX_TASK_DESIGN 3U
#define LINX_NO_ID 0U

typedef enum {
    LINX_TASK_RUNNING,
    LINX_TASK_WAITING,
    LINX_TASK_FINISHED
} linx_task_state_t;

typedef enum {
    LINX_COMMAND_NONE,
    LINX_COMMAND_SENDING,
    LINX_COMMAND_COMPUTER_RECEIVED,
    LINX_COMMAND_QUEUED,
    LINX_COMMAND_AGENT_ACCEPTED,
    LINX_COMMAND_RUNNING,
    LINX_COMMAND_FINISHED,
    LINX_COMMAND_FAILED
} linx_command_state_t;

typedef struct {
    uint32_t scene_id;
    uint32_t task_id;
    uint32_t source_turn_id;
    uint32_t image_id;
} linx_reply_context_t;

typedef struct {
    uint32_t id;
    linx_reply_context_t context;
    linx_command_state_t state;
    const char *text;
    uint64_t started_ms;
    uint64_t accepted_ms;
    bool fail_before_receipt;
} linx_command_t;

typedef struct {
    uint32_t id;
    uint32_t number;
    const char *user_text;
    linx_content_id_t content_id;
    uint32_t image_ids[LINX_IMAGE_COUNT];
    size_t image_count;
    bool unread;
    bool complete;
    linx_command_t command;
} linx_turn_t;

typedef struct {
    uint32_t id;
    const char *title;
    const char *agent;
    linx_task_state_t state;
    linx_turn_t turns[LINX_HISTORY_CAPACITY];
    size_t turn_count;
    uint32_t next_turn_number;
    bool history_trimmed;
    bool initial_started;
    uint64_t run_started_ms;
    uint64_t run_duration_ms;
    uint32_t running_turn_id;
    /* This slot is reserved until accepted or explicitly cancelled, including failure. */
    linx_command_t pending;
} linx_task_t;

typedef struct {
    linx_task_t tasks[LINX_TASK_COUNT];
    uint32_t scene_id;
    uint32_t next_turn_id;
    uint32_t next_command_id;
    uint32_t revision;
    uint64_t wall_ms;
    uint64_t active_ms;
    bool online;
    bool fail_next_send;
} linx_demo_t;

typedef enum {
    LINX_REPLY_ALLOWED,
    LINX_REPLY_PENDING,
    LINX_REPLY_FAILED,
    LINX_REPLY_INVALID
} linx_reply_gate_t;

typedef struct {
    uint32_t image_id;
    uint32_t turn_id;
    uint32_t turn_number;
} linx_image_ref_t;

/* All mutation is serialized by the UI owner. No allocations, platform calls or I/O. */
void linx_demo_init(linx_demo_t *demo, uint64_t now_ms);
void linx_demo_reset(linx_demo_t *demo, uint64_t now_ms);
void linx_demo_tick(linx_demo_t *demo, uint64_t now_ms);
void linx_demo_set_online(linx_demo_t *demo, bool online, uint64_t now_ms);
void linx_demo_fail_next_send(linx_demo_t *demo, bool enabled);
void linx_demo_enter_task(linx_demo_t *demo, uint32_t task_id, uint64_t now_ms);
const linx_task_t *linx_demo_task(const linx_demo_t *demo, uint32_t task_id);
const linx_turn_t *linx_demo_turn(const linx_demo_t *demo, uint32_t task_id, uint32_t turn_id);
const linx_turn_t *linx_demo_latest_turn(const linx_demo_t *demo, uint32_t task_id);
/* Index zero is the newest turn. An incomplete accepted turn still has its user message. */
const linx_turn_t *linx_demo_history(const linx_demo_t *demo, uint32_t task_id, size_t newest_index);
bool linx_demo_has_unread(const linx_demo_t *demo, uint32_t task_id);
void linx_demo_mark_read(linx_demo_t *demo, uint32_t task_id, uint32_t turn_id);

/* source_turn_id=0 captures latest turn. Images must belong to the captured turn. */
bool linx_demo_reply_context(const linx_demo_t *demo, uint32_t task_id,
                             uint32_t source_turn_id, uint32_t image_id,
                             linx_reply_context_t *context);
linx_reply_gate_t linx_demo_reply_gate(const linx_demo_t *demo, uint32_t task_id);
/* Returns a stable command ID, or zero if stale/invalid/already pending. */
uint32_t linx_demo_send(linx_demo_t *demo, const linx_reply_context_t *context, uint64_t now_ms);
bool linx_demo_retry(linx_demo_t *demo, uint32_t task_id, uint32_t command_id, uint64_t now_ms);
bool linx_demo_cancel_failed(linx_demo_t *demo, uint32_t task_id, uint32_t command_id);
const linx_command_t *linx_demo_command(const linx_demo_t *demo, uint32_t task_id, uint32_t command_id);
/* A zero turn filter enumerates every retained attachment, newest turn first. */
size_t linx_demo_images(const linx_demo_t *demo, uint32_t task_id, uint32_t turn_filter,
                        linx_image_ref_t *out, size_t capacity);
const char *linx_demo_task_status(const linx_task_t *task);
const char *linx_demo_command_status(linx_command_state_t state);

#endif
