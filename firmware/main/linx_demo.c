#include "linx_demo.h"

#include <limits.h>
#include <string.h>

#define INITIAL_RUN_MS 8000U
#define REPLY_RUN_MS 4000U
#define COMPUTER_RECEIPT_MS 500U
#define AGENT_RECEIPT_MS 1000U
#define ACCEPTED_NOTICE_MS 250U

static linx_task_t *mutable_task(linx_demo_t *demo, uint32_t task_id)
{
    if (demo == NULL || task_id < 1U || task_id > LINX_TASK_COUNT) {
        return NULL;
    }
    return &demo->tasks[task_id - 1U];
}

const linx_task_t *linx_demo_task(const linx_demo_t *demo, uint32_t task_id)
{
    if (demo == NULL || task_id < 1U || task_id > LINX_TASK_COUNT) {
        return NULL;
    }
    return &demo->tasks[task_id - 1U];
}

static linx_turn_t *mutable_turn(linx_task_t *task, uint32_t turn_id)
{
    if (task == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < task->turn_count; ++i) {
        if (task->turns[i].id == turn_id) {
            return &task->turns[i];
        }
    }
    return NULL;
}

const linx_turn_t *linx_demo_turn(const linx_demo_t *demo, uint32_t task_id, uint32_t turn_id)
{
    const linx_task_t *task = linx_demo_task(demo, task_id);
    if (task == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < task->turn_count; ++i) {
        if (task->turns[i].id == turn_id) {
            return &task->turns[i];
        }
    }
    return NULL;
}

const linx_turn_t *linx_demo_history(const linx_demo_t *demo, uint32_t task_id, size_t newest_index)
{
    const linx_task_t *task = linx_demo_task(demo, task_id);
    if (task == NULL || newest_index >= task->turn_count) {
        return NULL;
    }
    return &task->turns[task->turn_count - 1U - newest_index];
}

const linx_turn_t *linx_demo_latest_turn(const linx_demo_t *demo, uint32_t task_id)
{
    return linx_demo_history(demo, task_id, 0U);
}

static linx_turn_t *append_turn(linx_demo_t *demo, linx_task_t *task)
{
    if (task->turn_count == LINX_HISTORY_CAPACITY) {
        memmove(&task->turns[0], &task->turns[1],
                (LINX_HISTORY_CAPACITY - 1U) * sizeof(task->turns[0]));
        --task->turn_count;
        task->history_trimmed = true;
    }
    linx_turn_t *turn = &task->turns[task->turn_count++];
    memset(turn, 0, sizeof(*turn));
    turn->id = demo->next_turn_id++;
    turn->number = task->next_turn_number++;
    return turn;
}

static void load_initial(linx_demo_t *demo, uint64_t now_ms)
{
    static const char *const titles[] = {"登录校验修复", "支付接口调整", "LinX 暖白界面"};
    memset(demo->tasks, 0, sizeof(demo->tasks));
    demo->wall_ms = now_ms;
    demo->active_ms = 0U;
    demo->online = true;
    demo->fail_next_send = false;
    for (size_t i = 0; i < LINX_TASK_COUNT; ++i) {
        linx_task_t *task = &demo->tasks[i];
        task->id = (uint32_t)i + 1U;
        task->title = titles[i];
        task->agent = i == 1U ? "Claude Code" : "Codex";
        task->state = i == 0U ? LINX_TASK_RUNNING : (i == 1U ? LINX_TASK_WAITING : LINX_TASK_FINISHED);
        task->next_turn_number = 1U;
        linx_turn_t *turn = append_turn(demo, task);
        turn->user_text = linx_content_initial_user(task->id);
        turn->complete = i != 0U;
        if (i == 1U) {
            turn->content_id = LINX_CONTENT_PAYMENT_INITIAL;
        } else if (i == 2U) {
            turn->content_id = LINX_CONTENT_DESIGN_INITIAL;
            turn->unread = true;
            turn->image_count = LINX_IMAGE_COUNT;
            for (size_t image = 0; image < LINX_IMAGE_COUNT; ++image) {
                turn->image_ids[image] = (uint32_t)image + 1U;
            }
        }
    }
    ++demo->revision;
}

void linx_demo_init(linx_demo_t *demo, uint64_t now_ms)
{
    if (demo == NULL) {
        return;
    }
    memset(demo, 0, sizeof(*demo));
    demo->scene_id = 1U;
    demo->next_turn_id = 1U;
    demo->next_command_id = 1U;
    load_initial(demo, now_ms);
}

void linx_demo_reset(linx_demo_t *demo, uint64_t now_ms)
{
    if (demo == NULL) {
        return;
    }
    ++demo->scene_id;
    load_initial(demo, now_ms);
}

static bool task_running(const linx_task_t *task)
{
    return task->state == LINX_TASK_RUNNING;
}

static void accept_pending(linx_demo_t *demo, linx_task_t *task, uint64_t at_ms)
{
    linx_turn_t *turn = append_turn(demo, task);
    turn->user_text = task->pending.text;
    turn->command = task->pending;
    turn->command.state = LINX_COMMAND_AGENT_ACCEPTED;
    turn->command.accepted_ms = at_ms;
    memset(&task->pending, 0, sizeof(task->pending));
    task->state = LINX_TASK_RUNNING;
    task->run_started_ms = at_ms;
    task->run_duration_ms = REPLY_RUN_MS;
    task->running_turn_id = turn->id;
    ++demo->revision;
}

static void complete_run(linx_demo_t *demo, linx_task_t *task)
{
    linx_turn_t *turn = mutable_turn(task, task->running_turn_id);
    if (turn == NULL) {
        return;
    }
    if (turn->command.id == LINX_NO_ID) {
        turn->content_id = LINX_CONTENT_LOGIN_INITIAL;
    } else {
        switch (task->id) {
        case LINX_TASK_LOGIN: turn->content_id = LINX_CONTENT_LOGIN_FOLLOWUP; break;
        case LINX_TASK_PAYMENT: turn->content_id = LINX_CONTENT_PAYMENT_FOLLOWUP; break;
        default: turn->content_id = LINX_CONTENT_DESIGN_FOLLOWUP; break;
        }
        turn->command.state = LINX_COMMAND_FINISHED;
    }
    turn->complete = true;
    turn->unread = true;
    task->state = LINX_TASK_FINISHED;
    task->running_turn_id = LINX_NO_ID;
    ++demo->revision;
}

/* Work from event timestamps rather than frame count: a delayed UI tick has the
 * same result as many small ticks, including a queued command after a reply. */
static uint64_t next_event(const linx_task_t *task)
{
    uint64_t next = UINT64_MAX;
    if (task->running_turn_id != LINX_NO_ID) {
        next = task->run_started_ms + task->run_duration_ms;
        const linx_turn_t *running = NULL;
        for (size_t i = 0; i < task->turn_count; ++i) {
            if (task->turns[i].id == task->running_turn_id) {
                running = &task->turns[i];
                break;
            }
        }
        if (running != NULL && running->command.state == LINX_COMMAND_AGENT_ACCEPTED) {
            uint64_t transition = running->command.accepted_ms + ACCEPTED_NOTICE_MS;
            if (transition < next) {
                next = transition;
            }
        }
    }
    uint64_t pending_at = UINT64_MAX;
    if (task->pending.state == LINX_COMMAND_SENDING) {
        pending_at = task->pending.started_ms + COMPUTER_RECEIPT_MS;
    } else if (task->pending.state == LINX_COMMAND_COMPUTER_RECEIVED) {
        pending_at = task->pending.started_ms + AGENT_RECEIPT_MS;
    } else if (task->pending.state == LINX_COMMAND_QUEUED && !task_running(task)) {
        pending_at = task->pending.started_ms + AGENT_RECEIPT_MS;
    }
    return pending_at < next ? pending_at : next;
}

static void process_event(linx_demo_t *demo, linx_task_t *task, uint64_t at_ms)
{
    if (task->running_turn_id != LINX_NO_ID) {
        linx_turn_t *turn = mutable_turn(task, task->running_turn_id);
        if (turn != NULL && turn->command.state == LINX_COMMAND_AGENT_ACCEPTED &&
            at_ms >= turn->command.accepted_ms + ACCEPTED_NOTICE_MS) {
            turn->command.state = LINX_COMMAND_RUNNING;
            ++demo->revision;
        }
        if (at_ms >= task->run_started_ms + task->run_duration_ms) {
            complete_run(demo, task);
        }
    }
    if (task->pending.state == LINX_COMMAND_SENDING &&
        at_ms >= task->pending.started_ms + COMPUTER_RECEIPT_MS) {
        task->pending.state = task->pending.fail_before_receipt ?
                              LINX_COMMAND_FAILED : LINX_COMMAND_COMPUTER_RECEIVED;
        ++demo->revision;
    }
    if (task->pending.state == LINX_COMMAND_COMPUTER_RECEIVED &&
        at_ms >= task->pending.started_ms + AGENT_RECEIPT_MS) {
        if (task_running(task)) {
            task->pending.state = LINX_COMMAND_QUEUED;
            ++demo->revision;
        } else {
            accept_pending(demo, task, at_ms);
        }
    } else if (task->pending.state == LINX_COMMAND_QUEUED && !task_running(task)) {
        accept_pending(demo, task, at_ms);
    }
}

void linx_demo_tick(linx_demo_t *demo, uint64_t now_ms)
{
    if (demo == NULL || now_ms < demo->wall_ms) {
        return;
    }
    uint64_t target = demo->active_ms + (demo->online ? now_ms - demo->wall_ms : 0U);
    demo->wall_ms = now_ms;
    if (!demo->online) {
        return;
    }
    for (;;) {
        uint64_t next = UINT64_MAX;
        size_t selected = 0U;
        for (size_t i = 0; i < LINX_TASK_COUNT; ++i) {
            uint64_t at_ms = next_event(&demo->tasks[i]);
            if (at_ms < next) {
                next = at_ms;
                selected = i;
            }
        }
        if (next == UINT64_MAX || next > target) {
            break;
        }
        /* Queued acceptance after a finished task belongs to the finish time,
         * never the earlier time at which it first entered the queue. */
        if (next < demo->active_ms) {
            next = demo->active_ms;
        }
        demo->active_ms = next;
        process_event(demo, &demo->tasks[selected], next);
    }
    demo->active_ms = target;
}

void linx_demo_enter_task(linx_demo_t *demo, uint32_t task_id, uint64_t now_ms)
{
    linx_demo_tick(demo, now_ms);
    linx_task_t *task = mutable_task(demo, task_id);
    if (task != NULL && task_id == LINX_TASK_LOGIN && !task->initial_started) {
        task->initial_started = true;
        task->run_started_ms = demo->active_ms;
        task->run_duration_ms = INITIAL_RUN_MS;
        task->running_turn_id = task->turns[0].id;
        ++demo->revision;
    }
}

void linx_demo_set_online(linx_demo_t *demo, bool online, uint64_t now_ms)
{
    linx_demo_tick(demo, now_ms);
    if (demo == NULL || demo->online == online) {
        return;
    }
    demo->online = online;
    if (!online) {
        for (size_t i = 0; i < LINX_TASK_COUNT; ++i) {
            if (demo->tasks[i].pending.state == LINX_COMMAND_SENDING) {
                demo->tasks[i].pending.state = LINX_COMMAND_FAILED;
            }
        }
    }
    ++demo->revision;
}

void linx_demo_fail_next_send(linx_demo_t *demo, bool enabled)
{
    if (demo != NULL && demo->fail_next_send != enabled) {
        demo->fail_next_send = enabled;
        ++demo->revision;
    }
}

bool linx_demo_has_unread(const linx_demo_t *demo, uint32_t task_id)
{
    const linx_task_t *task = linx_demo_task(demo, task_id);
    if (task != NULL) {
        for (size_t i = 0; i < task->turn_count; ++i) {
            if (task->turns[i].unread) {
                return true;
            }
        }
    }
    return false;
}

void linx_demo_mark_read(linx_demo_t *demo, uint32_t task_id, uint32_t turn_id)
{
    linx_turn_t *turn = mutable_turn(mutable_task(demo, task_id), turn_id);
    if (turn != NULL && turn->complete && turn->unread) {
        turn->unread = false;
        ++demo->revision;
    }
}

static bool valid_context(const linx_demo_t *demo, const linx_reply_context_t *context)
{
    if (demo == NULL || context == NULL || context->scene_id != demo->scene_id) {
        return false;
    }
    const linx_turn_t *turn = linx_demo_turn(demo, context->task_id, context->source_turn_id);
    if (turn == NULL) {
        return false;
    }
    if (context->image_id == LINX_NO_ID) {
        return true;
    }
    for (size_t i = 0; i < turn->image_count; ++i) {
        if (turn->image_ids[i] == context->image_id) {
            return true;
        }
    }
    return false;
}

bool linx_demo_reply_context(const linx_demo_t *demo, uint32_t task_id,
                             uint32_t source_turn_id, uint32_t image_id,
                             linx_reply_context_t *context)
{
    if (context == NULL || demo == NULL) {
        return false;
    }
    const linx_turn_t *turn = source_turn_id == LINX_NO_ID ?
        linx_demo_latest_turn(demo, task_id) : linx_demo_turn(demo, task_id, source_turn_id);
    if (turn == NULL) {
        return false;
    }
    linx_reply_context_t candidate = {demo->scene_id, task_id, turn->id, image_id};
    if (!valid_context(demo, &candidate)) {
        return false;
    }
    *context = candidate;
    return true;
}

linx_reply_gate_t linx_demo_reply_gate(const linx_demo_t *demo, uint32_t task_id)
{
    const linx_task_t *task = linx_demo_task(demo, task_id);
    if (task == NULL) {
        return LINX_REPLY_INVALID;
    }
    if (task->pending.state == LINX_COMMAND_FAILED) {
        return LINX_REPLY_FAILED;
    }
    return task->pending.id == LINX_NO_ID ? LINX_REPLY_ALLOWED : LINX_REPLY_PENDING;
}

static void start_send(linx_demo_t *demo, linx_command_t *command)
{
    command->state = demo->online ? LINX_COMMAND_SENDING : LINX_COMMAND_FAILED;
    command->started_ms = demo->active_ms;
    command->fail_before_receipt = demo->fail_next_send;
    demo->fail_next_send = false;
    ++demo->revision;
}

uint32_t linx_demo_send(linx_demo_t *demo, const linx_reply_context_t *context, uint64_t now_ms)
{
    linx_demo_tick(demo, now_ms);
    if (!valid_context(demo, context) || linx_demo_reply_gate(demo, context->task_id) != LINX_REPLY_ALLOWED) {
        return LINX_NO_ID;
    }
    linx_demo_enter_task(demo, context->task_id, now_ms);
    linx_task_t *task = mutable_task(demo, context->task_id);
    task->pending.id = demo->next_command_id++;
    task->pending.context = *context;
    task->pending.text = linx_content_transcript(context->task_id, context->image_id);
    start_send(demo, &task->pending);
    return task->pending.id;
}

bool linx_demo_retry(linx_demo_t *demo, uint32_t task_id, uint32_t command_id, uint64_t now_ms)
{
    linx_demo_tick(demo, now_ms);
    linx_task_t *task = mutable_task(demo, task_id);
    if (task == NULL || command_id == LINX_NO_ID || task->pending.id != command_id ||
        task->pending.state != LINX_COMMAND_FAILED) {
        return false;
    }
    start_send(demo, &task->pending);
    return true;
}

bool linx_demo_cancel_failed(linx_demo_t *demo, uint32_t task_id, uint32_t command_id)
{
    linx_task_t *task = mutable_task(demo, task_id);
    if (task == NULL || command_id == LINX_NO_ID || task->pending.id != command_id ||
        task->pending.state != LINX_COMMAND_FAILED) {
        return false;
    }
    memset(&task->pending, 0, sizeof(task->pending));
    ++demo->revision;
    return true;
}

const linx_command_t *linx_demo_command(const linx_demo_t *demo, uint32_t task_id, uint32_t command_id)
{
    const linx_task_t *task = linx_demo_task(demo, task_id);
    if (task == NULL || command_id == LINX_NO_ID) {
        return NULL;
    }
    if (task->pending.id == command_id) {
        return &task->pending;
    }
    for (size_t i = 0; i < task->turn_count; ++i) {
        if (task->turns[i].command.id == command_id) {
            return &task->turns[i].command;
        }
    }
    return NULL;
}

size_t linx_demo_images(const linx_demo_t *demo, uint32_t task_id, uint32_t turn_filter,
                        linx_image_ref_t *out, size_t capacity)
{
    const linx_task_t *task = linx_demo_task(demo, task_id);
    if (task == NULL) {
        return 0U;
    }
    size_t count = 0U;
    for (size_t t = task->turn_count; t > 0U; --t) {
        const linx_turn_t *turn = &task->turns[t - 1U];
        if (turn_filter != LINX_NO_ID && turn_filter != turn->id) {
            continue;
        }
        for (size_t i = 0; i < turn->image_count; ++i) {
            if (out != NULL && count < capacity) {
                out[count] = (linx_image_ref_t){turn->image_ids[i], turn->id, turn->number};
            }
            ++count;
        }
    }
    return count;
}

const char *linx_demo_task_status(const linx_task_t *task)
{
    if (task == NULL) {
        return "";
    }
    switch (task->state) {
    case LINX_TASK_RUNNING: return "运行中";
    case LINX_TASK_WAITING: return "等你回复";
    default: return "本轮结束";
    }
}

const char *linx_demo_command_status(linx_command_state_t state)
{
    switch (state) {
    case LINX_COMMAND_SENDING: return "模拟发送中";
    case LINX_COMMAND_COMPUTER_RECEIVED: return "模拟电脑已收到";
    case LINX_COMMAND_QUEUED: return "已排队";
    case LINX_COMMAND_AGENT_ACCEPTED: return "模拟 Agent 已接收";
    case LINX_COMMAND_RUNNING: return "模拟 Agent 运行中";
    case LINX_COMMAND_FINISHED: return "本轮结束";
    case LINX_COMMAND_FAILED: return "模拟发送失败";
    default: return "尚未发送";
    }
}
