#include "linx_demo.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t send_to(linx_demo_t *demo, uint32_t task_id, uint64_t now_ms)
{
    linx_reply_context_t context;
    assert(linx_demo_reply_context(demo, task_id, 0U, 0U, &context));
    uint32_t id = linx_demo_send(demo, &context, now_ms);
    assert(id != 0U);
    return id;
}

static void test_initial_and_unread(void)
{
    linx_demo_t demo;
    linx_demo_init(&demo, 100U);
    assert(demo.online && demo.scene_id == 1U);
    assert(linx_demo_task(&demo, 0U) == NULL);
    assert(linx_demo_task(&demo, 4U) == NULL);
    assert(linx_demo_history(&demo, LINX_TASK_DESIGN, 8U) == NULL);
    assert(linx_demo_has_unread(&demo, LINX_TASK_DESIGN));
    const linx_turn_t *design = linx_demo_latest_turn(&demo, LINX_TASK_DESIGN);
    uint32_t original = design->id;
    linx_demo_tick(&demo, 20000U);
    assert(!linx_demo_latest_turn(&demo, LINX_TASK_LOGIN)->complete);
    linx_demo_enter_task(&demo, LINX_TASK_LOGIN, 20000U);
    linx_demo_tick(&demo, 27999U);
    assert(!linx_demo_latest_turn(&demo, LINX_TASK_LOGIN)->complete);
    linx_demo_tick(&demo, 28000U);
    assert(linx_demo_latest_turn(&demo, LINX_TASK_LOGIN)->complete);
    assert(linx_demo_has_unread(&demo, LINX_TASK_LOGIN));
    linx_demo_mark_read(&demo, LINX_TASK_DESIGN, original);
    assert(!linx_demo_has_unread(&demo, LINX_TASK_DESIGN));
    assert(linx_demo_has_unread(&demo, LINX_TASK_LOGIN));
    assert(linx_demo_reply_gate(&demo, 99U) == LINX_REPLY_INVALID);
}

static void test_send_and_fixed_picture_identity(void)
{
    linx_demo_t demo;
    linx_demo_init(&demo, 0U);
    linx_reply_context_t context;
    uint32_t source = linx_demo_latest_turn(&demo, LINX_TASK_DESIGN)->id;
    assert(linx_demo_reply_context(&demo, LINX_TASK_DESIGN, source, LINX_IMAGE_RESULT, &context));
    assert(!linx_demo_reply_context(&demo, LINX_TASK_PAYMENT, 0U, LINX_IMAGE_RESULT, &context));
    assert(linx_demo_reply_context(&demo, LINX_TASK_DESIGN, source, LINX_IMAGE_RESULT, &context));
    uint32_t command = linx_demo_send(&demo, &context, 0U);
    assert(command != 0U);
    assert(linx_demo_send(&demo, &context, 1U) == 0U);
    assert(linx_demo_reply_gate(&demo, LINX_TASK_DESIGN) == LINX_REPLY_PENDING);
    assert(linx_demo_task(&demo, LINX_TASK_DESIGN)->turn_count == 1U);
    linx_demo_tick(&demo, 499U);
    assert(linx_demo_command(&demo, LINX_TASK_DESIGN, command)->state == LINX_COMMAND_SENDING);
    linx_demo_tick(&demo, 500U);
    assert(linx_demo_command(&demo, LINX_TASK_DESIGN, command)->state == LINX_COMMAND_COMPUTER_RECEIVED);
    linx_demo_tick(&demo, 1000U);
    assert(linx_demo_command(&demo, LINX_TASK_DESIGN, command)->state == LINX_COMMAND_AGENT_ACCEPTED);
    assert(linx_demo_task(&demo, LINX_TASK_DESIGN)->turn_count == 2U);
    assert(linx_demo_reply_gate(&demo, LINX_TASK_DESIGN) == LINX_REPLY_ALLOWED);
    linx_demo_tick(&demo, 1250U);
    assert(linx_demo_command(&demo, LINX_TASK_DESIGN, command)->state == LINX_COMMAND_RUNNING);
    linx_demo_tick(&demo, 5000U);
    const linx_turn_t *reply = linx_demo_latest_turn(&demo, LINX_TASK_DESIGN);
    assert(reply->complete && reply->unread);
    assert(reply->command.context.source_turn_id == source);
    assert(reply->command.context.image_id == LINX_IMAGE_RESULT);
    assert(reply->command.id == command);
    assert(reply->image_count == 0U);
    assert(strstr(reply->user_text, "这张图") != NULL);
    assert(linx_demo_task(&demo, LINX_TASK_PAYMENT)->turn_count == 1U);
    linx_demo_mark_read(&demo, LINX_TASK_DESIGN, source);
    assert(linx_demo_has_unread(&demo, LINX_TASK_DESIGN));
    linx_image_ref_t images[3];
    assert(linx_demo_images(&demo, LINX_TASK_DESIGN, 0U, images, 3U) == 3U);
    assert(images[1].image_id == LINX_IMAGE_RESULT && images[1].turn_id == source);
    assert(linx_demo_images(&demo, LINX_TASK_DESIGN, reply->id, NULL, 0U) == 0U);
    assert(linx_demo_images(&demo, LINX_TASK_DESIGN, source, images, 1U) == 3U);
    linx_demo_mark_read(&demo, LINX_TASK_DESIGN, reply->id);
    assert(!linx_demo_has_unread(&demo, LINX_TASK_DESIGN));
}

static void test_queue_and_large_ticks(void)
{
    linx_demo_t demo;
    linx_demo_init(&demo, 0U);
    linx_demo_enter_task(&demo, LINX_TASK_LOGIN, 0U);
    uint32_t command = send_to(&demo, LINX_TASK_LOGIN, 100U);
    linx_demo_tick(&demo, 1100U);
    assert(linx_demo_command(&demo, LINX_TASK_LOGIN, command)->state == LINX_COMMAND_QUEUED);
    assert(linx_demo_task(&demo, LINX_TASK_LOGIN)->turn_count == 1U);
    assert(linx_demo_reply_gate(&demo, LINX_TASK_LOGIN) == LINX_REPLY_PENDING);
    linx_demo_tick(&demo, 8000U);
    assert(linx_demo_command(&demo, LINX_TASK_LOGIN, command)->state == LINX_COMMAND_AGENT_ACCEPTED);
    assert(linx_demo_task(&demo, LINX_TASK_LOGIN)->turn_count == 2U);
    uint32_t second = send_to(&demo, LINX_TASK_LOGIN, 8000U);
    linx_demo_tick(&demo, 9000U);
    assert(linx_demo_command(&demo, LINX_TASK_LOGIN, second)->state == LINX_COMMAND_QUEUED);
    linx_demo_tick(&demo, 20000U);
    assert(linx_demo_command(&demo, LINX_TASK_LOGIN, command)->state == LINX_COMMAND_FINISHED);
    assert(linx_demo_command(&demo, LINX_TASK_LOGIN, second)->state == LINX_COMMAND_FINISHED);
    assert(linx_demo_task(&demo, LINX_TASK_LOGIN)->turn_count == 3U);
    assert(linx_demo_task(&demo, LINX_TASK_LOGIN)->state == LINX_TASK_FINISHED);
    assert(linx_demo_command(&demo, LINX_TASK_LOGIN, second)->accepted_ms == 12000U);
}

static void test_failure_retry_cancel(void)
{
    linx_demo_t demo;
    linx_demo_init(&demo, 0U);
    linx_reply_context_t context;
    assert(linx_demo_reply_context(&demo, LINX_TASK_DESIGN, 0U, LINX_IMAGE_RESULT, &context));
    linx_demo_fail_next_send(&demo, true);
    uint32_t id = linx_demo_send(&demo, &context, 0U);
    assert(!demo.fail_next_send);
    linx_demo_tick(&demo, 500U);
    assert(linx_demo_reply_gate(&demo, LINX_TASK_DESIGN) == LINX_REPLY_FAILED);
    assert(linx_demo_task(&demo, LINX_TASK_DESIGN)->turn_count == 1U);
    assert(linx_demo_send(&demo, &context, 600U) == 0U);
    assert(!linx_demo_retry(&demo, LINX_TASK_DESIGN, id + 1U, 600U));
    assert(linx_demo_retry(&demo, LINX_TASK_DESIGN, id, 600U));
    assert(!linx_demo_retry(&demo, LINX_TASK_DESIGN, id, 601U));
    linx_demo_tick(&demo, 5600U);
    assert(linx_demo_task(&demo, LINX_TASK_DESIGN)->turn_count == 2U);
    const linx_command_t *done = linx_demo_command(&demo, LINX_TASK_DESIGN, id);
    assert(done->state == LINX_COMMAND_FINISHED && done->context.image_id == LINX_IMAGE_RESULT);
    assert(done->context.source_turn_id == context.source_turn_id);
    linx_demo_fail_next_send(&demo, true);
    uint32_t cancelled = send_to(&demo, LINX_TASK_PAYMENT, 6000U);
    linx_demo_tick(&demo, 6500U);
    assert(linx_demo_cancel_failed(&demo, LINX_TASK_PAYMENT, cancelled));
    assert(linx_demo_command(&demo, LINX_TASK_PAYMENT, cancelled) == NULL);
    assert(linx_demo_reply_gate(&demo, LINX_TASK_PAYMENT) == LINX_REPLY_ALLOWED);
    assert(linx_demo_task(&demo, LINX_TASK_PAYMENT)->turn_count == 1U);
}

static void test_offline_pause_and_no_autoretry(void)
{
    linx_demo_t demo;
    linx_demo_init(&demo, 0U);
    uint32_t failed = send_to(&demo, LINX_TASK_PAYMENT, 0U);
    linx_demo_set_online(&demo, false, 200U);
    assert(linx_demo_command(&demo, LINX_TASK_PAYMENT, failed)->state == LINX_COMMAND_FAILED);
    linx_demo_tick(&demo, 50000U);
    linx_demo_set_online(&demo, true, 50000U);
    linx_demo_tick(&demo, 60000U);
    assert(linx_demo_command(&demo, LINX_TASK_PAYMENT, failed)->state == LINX_COMMAND_FAILED);
    assert(linx_demo_retry(&demo, LINX_TASK_PAYMENT, failed, 60000U));
    linx_demo_tick(&demo, 60500U);
    linx_demo_set_online(&demo, false, 60600U);
    assert(linx_demo_command(&demo, LINX_TASK_PAYMENT, failed)->state == LINX_COMMAND_COMPUTER_RECEIVED);
    linx_demo_tick(&demo, 99999U);
    assert(linx_demo_task(&demo, LINX_TASK_PAYMENT)->turn_count == 1U);
    linx_demo_set_online(&demo, true, 100000U);
    linx_demo_tick(&demo, 100400U);
    assert(linx_demo_command(&demo, LINX_TASK_PAYMENT, failed)->state == LINX_COMMAND_AGENT_ACCEPTED);
    linx_demo_set_online(&demo, false, 101000U);
    linx_demo_tick(&demo, 200000U);
    assert(!linx_demo_latest_turn(&demo, LINX_TASK_PAYMENT)->complete);
    linx_demo_set_online(&demo, true, 200000U);
    linx_demo_tick(&demo, 203399U);
    assert(!linx_demo_latest_turn(&demo, LINX_TASK_PAYMENT)->complete);
    linx_demo_tick(&demo, 203400U);
    assert(linx_demo_latest_turn(&demo, LINX_TASK_PAYMENT)->complete);
    /* Offline confirmation stores one failed draft, without a history entry. */
    linx_demo_set_online(&demo, false, 204000U);
    uint32_t offline = send_to(&demo, LINX_TASK_PAYMENT, 204000U);
    assert(linx_demo_command(&demo, LINX_TASK_PAYMENT, offline)->state == LINX_COMMAND_FAILED);
    assert(linx_demo_task(&demo, LINX_TASK_PAYMENT)->turn_count == 2U);
}

static void test_reset_and_history_cap(void)
{
    linx_demo_t demo;
    linx_demo_init(&demo, 0U);
    linx_reply_context_t old;
    assert(linx_demo_reply_context(&demo, LINX_TASK_DESIGN, 0U, LINX_IMAGE_RESULT, &old));
    uint32_t id = linx_demo_send(&demo, &old, 0U);
    uint32_t old_scene = demo.scene_id;
    linx_demo_reset(&demo, 100U);
    assert(demo.scene_id != old_scene);
    assert(linx_demo_send(&demo, &old, 101U) == 0U);
    assert(linx_demo_command(&demo, LINX_TASK_DESIGN, id) == NULL);
    linx_demo_tick(&demo, 50000U);
    assert(linx_demo_task(&demo, LINX_TASK_DESIGN)->turn_count == 1U);
    assert(!linx_demo_task(&demo, LINX_TASK_LOGIN)->initial_started);
    assert(demo.online && !demo.fail_next_send);
    uint64_t now = 50000U;
    uint32_t previous_turn = 0U;
    for (size_t i = 0; i < 25U; ++i) {
        uint32_t command = send_to(&demo, LINX_TASK_DESIGN, now);
        assert(command > id);
        now += 5000U;
        linx_demo_tick(&demo, now);
        const linx_turn_t *latest = linx_demo_latest_turn(&demo, LINX_TASK_DESIGN);
        assert(latest->complete && latest->number == i + 2U && latest->id > previous_turn);
        previous_turn = latest->id;
    }
    const linx_task_t *task = linx_demo_task(&demo, LINX_TASK_DESIGN);
    assert(task->history_trimmed && task->turn_count == LINX_HISTORY_CAPACITY);
    assert(linx_demo_history(&demo, LINX_TASK_DESIGN, 7U)->number == 19U);
    assert(linx_demo_history(&demo, LINX_TASK_DESIGN, 8U) == NULL);
    assert(linx_demo_images(&demo, LINX_TASK_DESIGN, 0U, NULL, 0U) == 0U);
    uint64_t active = demo.active_ms;
    linx_demo_tick(&demo, 1U);
    assert(demo.active_ms == active);
}

static void test_tick_partition_invariance(void)
{
    linx_demo_t coarse;
    linx_demo_t fine;
    linx_demo_init(&coarse, 0U);
    linx_demo_init(&fine, 0U);
    linx_demo_enter_task(&coarse, LINX_TASK_LOGIN, 0U);
    linx_demo_enter_task(&fine, LINX_TASK_LOGIN, 0U);
    send_to(&coarse, LINX_TASK_LOGIN, 0U);
    send_to(&fine, LINX_TASK_LOGIN, 0U);
    send_to(&coarse, LINX_TASK_PAYMENT, 0U);
    send_to(&fine, LINX_TASK_PAYMENT, 0U);
    linx_demo_fail_next_send(&coarse, true);
    linx_demo_fail_next_send(&fine, true);
    send_to(&coarse, LINX_TASK_DESIGN, 0U);
    send_to(&fine, LINX_TASK_DESIGN, 0U);
    linx_demo_tick(&coarse, 20000U);
    for (uint64_t now = 17U; now < 20000U; now += 17U) {
        linx_demo_tick(&fine, now);
    }
    linx_demo_tick(&fine, 20000U);
    assert(memcmp(&coarse, &fine, sizeof(coarse)) == 0);
}

static void test_content_boundaries(void)
{
    assert(linx_content_get(LINX_CONTENT_COUNT) == NULL);
    assert(linx_content_image(99U) == NULL);
    const linx_content_t *design = linx_content_get(LINX_CONTENT_DESIGN_INITIAL);
    assert(strlen(design->body) < 4096U);
    assert(strstr(design->body, "暖白方向已经确定。") == design->body);
    assert(strstr(design->body, "需要你决定的事") != NULL);
    assert(strstr(design->body, "不能确认小屏体验已经合适。") != NULL);
    assert(strstr(linx_content_get(LINX_CONTENT_LOGIN_INITIAL)->summary, "尚未验证") != NULL);
    for (size_t i = 0; i < linx_content_text_count(); ++i) {
        assert(linx_content_text_at(i) != NULL);
        assert(strlen(linx_content_text_at(i)) < 4096U);
    }
    assert(linx_content_text_at(linx_content_text_count()) == NULL);
    assert(strcmp(linx_content_initial_user(0U), "") == 0);
    assert(strcmp(linx_content_initial_user(4U), "") == 0);
    assert(strcmp(linx_content_transcript(0U, 0U), "") == 0);
    assert(strcmp(linx_content_transcript(4U, LINX_IMAGE_RESULT), "") == 0);
    assert(strcmp(linx_content_transcript(LINX_TASK_DESIGN, 0U),
                  "保留暖白，把任务状态字放大。") == 0);
    printf("Long reply: %zu UTF-8 bytes; model: %zu bytes\n", strlen(design->body), sizeof(linx_demo_t));
}

static void test_invalid_requests_preserve_state(void)
{
    linx_demo_t demo;
    linx_demo_init(&demo, 0U);
    linx_demo_t before = demo;
    linx_reply_context_t context = {demo.scene_id, LINX_TASK_DESIGN, 999U, 0U};
    assert(linx_demo_task(NULL, LINX_TASK_LOGIN) == NULL);
    assert(linx_demo_turn(NULL, LINX_TASK_LOGIN, 1U) == NULL);
    assert(linx_demo_turn(&demo, 0U, 1U) == NULL);
    assert(linx_demo_turn(&demo, LINX_TASK_DESIGN, 999U) == NULL);
    assert(linx_demo_history(&demo, 4U, 0U) == NULL);
    assert(linx_demo_latest_turn(NULL, LINX_TASK_LOGIN) == NULL);
    assert(!linx_demo_has_unread(&demo, 4U));
    assert(!linx_demo_has_unread(NULL, LINX_TASK_LOGIN));
    assert(!linx_demo_reply_context(NULL, LINX_TASK_DESIGN, 0U, 0U, &context));
    assert(!linx_demo_reply_context(&demo, LINX_TASK_DESIGN, 0U, 0U, NULL));
    assert(!linx_demo_reply_context(&demo, 4U, 0U, 0U, &context));
    assert(!linx_demo_reply_context(&demo, LINX_TASK_DESIGN, 999U, 0U, &context));
    assert(context.source_turn_id == 999U);
    assert(linx_demo_send(&demo, &context, 0U) == LINX_NO_ID);
    assert(linx_demo_send(&demo, NULL, 0U) == LINX_NO_ID);
    assert(linx_demo_send(NULL, &context, 0U) == LINX_NO_ID);
    assert(!linx_demo_retry(&demo, 0U, 1U, 0U));
    assert(!linx_demo_retry(&demo, LINX_TASK_DESIGN, 0U, 0U));
    assert(!linx_demo_cancel_failed(&demo, 4U, 1U));
    assert(!linx_demo_cancel_failed(&demo, LINX_TASK_DESIGN, 0U));
    assert(!linx_demo_cancel_failed(&demo, LINX_TASK_DESIGN, 1U));
    assert(linx_demo_command(&demo, 4U, 1U) == NULL);
    assert(linx_demo_command(&demo, LINX_TASK_DESIGN, 0U) == NULL);
    assert(linx_demo_command(&demo, LINX_TASK_DESIGN, 999U) == NULL);
    assert(linx_demo_images(&demo, 0U, 0U, NULL, 0U) == 0U);
    linx_demo_mark_read(&demo, 4U, 1U);
    linx_demo_mark_read(&demo, LINX_TASK_DESIGN, 999U);
    linx_demo_mark_read(&demo, LINX_TASK_LOGIN,
                        linx_demo_latest_turn(&demo, LINX_TASK_LOGIN)->id);
    linx_demo_enter_task(&demo, 4U, 0U);
    assert(memcmp(&demo, &before, sizeof(demo)) == 0);

    linx_demo_init(NULL, 0U);
    linx_demo_reset(NULL, 0U);
    linx_demo_tick(NULL, 0U);
    linx_demo_enter_task(NULL, LINX_TASK_LOGIN, 0U);
    linx_demo_set_online(NULL, false, 0U);
    linx_demo_fail_next_send(NULL, true);
    linx_demo_mark_read(NULL, LINX_TASK_LOGIN, 1U);
    assert(!linx_demo_retry(NULL, LINX_TASK_LOGIN, 1U, 0U));
    assert(!linx_demo_cancel_failed(NULL, LINX_TASK_LOGIN, 1U));
}

static void test_idempotent_controls_and_image_capacity(void)
{
    linx_demo_t demo;
    linx_demo_init(&demo, 0U);
    uint32_t revision = demo.revision;
    linx_demo_set_online(&demo, true, 0U);
    linx_demo_fail_next_send(&demo, false);
    assert(demo.revision == revision);
    linx_demo_fail_next_send(&demo, true);
    revision = demo.revision;
    linx_demo_fail_next_send(&demo, true);
    assert(demo.revision == revision);
    linx_demo_fail_next_send(&demo, false);
    linx_demo_enter_task(&demo, LINX_TASK_LOGIN, 0U);
    revision = demo.revision;
    linx_demo_enter_task(&demo, LINX_TASK_LOGIN, 0U);
    assert(demo.revision == revision);

    uint32_t source = linx_demo_latest_turn(&demo, LINX_TASK_DESIGN)->id;
    linx_demo_mark_read(&demo, LINX_TASK_DESIGN, source);
    revision = demo.revision;
    linx_demo_mark_read(&demo, LINX_TASK_DESIGN, source);
    assert(demo.revision == revision);
    linx_image_ref_t images[2] = {{99U, 98U, 97U}, {96U, 95U, 94U}};
    assert(linx_demo_images(&demo, LINX_TASK_DESIGN, source, images, 0U) == LINX_IMAGE_COUNT);
    assert(images[0].image_id == 99U && images[1].image_id == 96U);
    assert(linx_demo_images(&demo, LINX_TASK_DESIGN, source, images, 1U) == LINX_IMAGE_COUNT);
    assert(images[0].image_id == LINX_IMAGE_TASK_LIST && images[0].turn_id == source);
    assert(images[1].image_id == 96U && images[1].turn_id == 95U);

    uint32_t command = send_to(&demo, LINX_TASK_PAYMENT, 0U);
    assert(!linx_demo_cancel_failed(&demo, LINX_TASK_PAYMENT, command));
    assert(linx_demo_command(&demo, LINX_TASK_PAYMENT, command)->state == LINX_COMMAND_SENDING);
    linx_demo_set_online(&demo, false, 0U);
    assert(!linx_demo_cancel_failed(&demo, LINX_TASK_PAYMENT, command + 1U));
    assert(linx_demo_command(&demo, LINX_TASK_PAYMENT, command)->state == LINX_COMMAND_FAILED);
}

static void test_public_status_text(void)
{
    static const char *const states[] = {
        "尚未发送", "模拟发送中", "模拟电脑已收到", "已排队",
        "模拟 Agent 已接收", "模拟 Agent 运行中", "本轮结束", "模拟发送失败"
    };
    for (size_t i = 0U; i < sizeof(states) / sizeof(states[0]); i++) {
        assert(strcmp(linx_demo_command_status((linx_command_state_t)i), states[i]) == 0);
    }
    assert(strcmp(linx_demo_command_status((linx_command_state_t)99), "尚未发送") == 0);
    assert(strcmp(linx_demo_task_status(NULL), "") == 0);
    linx_demo_t demo;
    linx_demo_init(&demo, 0U);
    assert(strcmp(linx_demo_task_status(linx_demo_task(&demo, LINX_TASK_LOGIN)), "运行中") == 0);
    assert(strcmp(linx_demo_task_status(linx_demo_task(&demo, LINX_TASK_PAYMENT)), "等你回复") == 0);
    assert(strcmp(linx_demo_task_status(linx_demo_task(&demo, LINX_TASK_DESIGN)), "本轮结束") == 0);
}

int main(void)
{
    test_initial_and_unread();
    test_send_and_fixed_picture_identity();
    test_queue_and_large_ticks();
    test_failure_retry_cancel();
    test_offline_pause_and_no_autoretry();
    test_reset_and_history_cap();
    test_tick_partition_invariance();
    test_content_boundaries();
    test_invalid_requests_preserve_state();
    test_idempotent_controls_and_image_capacity();
    test_public_status_text();
    puts("LinX demo model tests: PASS (11 scenarios)");
    return 0;
}
