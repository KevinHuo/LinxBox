#include "linx_input.h"
#include <assert.h>
#include <stdio.h>

static void test_single_double_and_triple(void)
{
    linx_input_t input;
    linx_input_init(&input, 300);
    input.back_mode = LINX_BACK_DOUBLE_CLICK;
    assert(linx_input_tick(&input, 0) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 20) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 100) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 99) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 100) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 399) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 400) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 401) == LINX_INPUT_OK);
    assert(linx_input_tick(&input, 402) == LINX_INPUT_NONE);

    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 700) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 800) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 1000) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 1100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1100) == LINX_INPUT_BACK);
    assert(linx_input_tick(&input, 1101) == LINX_INPUT_NONE);
    /* 连续三击只消费前两击，第三击独立等待，不重复返回。 */
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 1150) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1200) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 1501) == LINX_INPUT_OK);

    /* 第一击可从零时刻开始，不能靠时间戳是否为零判断待决。 */
    linx_input_init(&input, 300);
    input.back_mode = LINX_BACK_DOUBLE_CLICK;
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 0) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 0) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 300) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 301) == LINX_INPUT_OK);
}

static void test_long_and_expired_taps(void)
{
    linx_input_t input;
    linx_input_init(&input, 300);
    input.back_mode = LINX_BACK_DOUBLE_CLICK;
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 20) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 520) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 900) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 1000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1200) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 1501) == LINX_INPUT_NONE);

    /* 下一次按下已过窗口时先派发旧单击，新点按继续独立计时。 */
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 2000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 2100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 2401) == LINX_INPUT_OK);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 2450) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 2751) == LINX_INPUT_OK);

    /* 第二击跨过截止点：RELEASE 先派发旧单击，再等待新的单击。 */
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 3000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 3100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 3300) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 3401) == LINX_INPUT_OK);
    assert(linx_input_tick(&input, 3701) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 3702) == LINX_INPUT_OK);

    /* 第二击一直按住不能把已经到期的第一击变成双击。 */
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 4000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 4100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 4300) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 4401) == LINX_INPUT_OK);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 4800) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 4900) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 5201) == LINX_INPUT_NONE);
}

static void test_directions_and_invalid_events(void)
{
    linx_input_t input;
    linx_input_init(&input, 300);
    input.back_mode = LINX_BACK_DOUBLE_CLICK;
    assert(linx_input_event(&input, 3, LINX_INPUT_PRESS, 1) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 10) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 15) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, (linx_input_event_t)99, 18) == LINX_INPUT_NONE);
    assert(input.held_key == -1);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 20) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, (linx_input_event_t)99, 22) == LINX_INPUT_NONE);
    assert(input.held_key == 2);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 25) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 1, LINX_INPUT_RELEASE, 30) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 0, LINX_INPUT_PRESS, 150) == LINX_INPUT_UP);
    assert(linx_input_event(&input, 1, LINX_INPUT_PRESS, 151) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 1, LINX_INPUT_RELEASE, 160) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 0, LINX_INPUT_LONG, 650) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 0, LINX_INPUT_RELEASE, 700) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 1000) == LINX_INPUT_NONE);

    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 1100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1200) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 1, LINX_INPUT_PRESS, 1600) == LINX_INPUT_DOWN);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 1601) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 1, LINX_INPUT_RELEASE, 1700) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 2000) == LINX_INPUT_NONE);
}

static void test_page_barriers(void)
{
    linx_input_t input;
    linx_input_init(&input, 300);
    input.back_mode = LINX_BACK_DOUBLE_CLICK;
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 7990) == LINX_INPUT_NONE);
    linx_input_barrier(&input, 8000);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 8040) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 7995) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 8060) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 8500) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 8600) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 9000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 9060) == LINX_INPUT_NONE);
    linx_input_barrier(&input, 9100);
    assert(linx_input_tick(&input, 10000) == LINX_INPUT_NONE);

    /* 切页时刻本身的排队事件也失效，之后的点按仍可使用。 */
    linx_input_barrier(&input, 10000);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 10000) == LINX_INPUT_NONE);
    assert(input.held_key == -1);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 10001) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 10050) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 10351) == LINX_INPUT_OK);

    /* 过期单击使页面切换时，已按下的下一击也不得落入新页面。 */
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 11000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 11050) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 11351) == LINX_INPUT_OK);
    linx_input_barrier(&input, 11400);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 11450) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 12000) == LINX_INPUT_NONE);
}

static void test_default_long_mode(void)
{
    linx_input_t input;
    linx_input_init(&input, 300);
    assert(input.back_mode == LINX_BACK_LONG_PRESS);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 20) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 60) == LINX_INPUT_OK);
    assert(linx_input_tick(&input, 400) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 500) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 1000) == LINX_INPUT_BACK);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 1100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1200) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 1600) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 1700) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1750) == LINX_INPUT_OK);
    /* 两次短按分别确认；长按方向键仍只有最初的导航动作。 */
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 1800) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1850) == LINX_INPUT_OK);
    assert(linx_input_event(&input, 0, LINX_INPUT_PRESS, 1900) == LINX_INPUT_UP);
    assert(linx_input_event(&input, 0, LINX_INPUT_LONG, 2400) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 0, LINX_INPUT_RELEASE, 2500) == LINX_INPUT_NONE);
}

static void test_mode_switch_barriers(void)
{
    linx_input_t input;
    linx_input_init(&input, 300);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 20) == LINX_INPUT_NONE);
    linx_input_set_back_mode(&input, LINX_BACK_DOUBLE_CLICK, 50);
    assert(input.back_mode == LINX_BACK_DOUBLE_CLICK && input.held_key == -1 && !input.pending_ok);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 60) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 50) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 150) == LINX_INPUT_NONE);
    linx_input_set_back_mode(&input, LINX_BACK_LONG_PRESS, 200);
    assert(input.back_mode == LINX_BACK_LONG_PRESS && !input.pending_ok);
    assert(linx_input_tick(&input, 1000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 190) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 1100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1150) == LINX_INPUT_OK);
    linx_input_set_back_mode(&input, (linx_back_mode_t)99, 1200);
    assert(input.back_mode == LINX_BACK_LONG_PRESS && input.valid_after_ms == 1200);
    linx_input_set_back_mode(&input, (linx_back_mode_t)-1, 1300);
    assert(input.back_mode == LINX_BACK_LONG_PRESS && input.valid_after_ms == 1300);
    linx_input_init(&input, 300);
    linx_input_set_back_mode(&input, LINX_BACK_DOUBLE_CLICK, 0);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 0) == LINX_INPUT_NONE);
    assert(input.held_key == -1);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 2) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 3) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 304) == LINX_INPUT_OK);
}

static void test_double_long_consumes_whole_gesture(void)
{
    linx_input_t input;
    linx_input_init(&input, 300);
    linx_input_set_back_mode(&input, LINX_BACK_DOUBLE_CLICK, 1);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 100) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 150) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 200) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 700) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 800) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 850) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 1200) == LINX_INPUT_NONE);
    /* 下一次正常短按仍可使用，不能把已消费的长按算为第一击。 */
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 1300) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 1350) == LINX_INPUT_NONE);
    assert(linx_input_tick(&input, 1651) == LINX_INPUT_OK);
}

int main(void)
{
    test_default_long_mode();
    test_mode_switch_barriers();
    test_double_long_consumes_whole_gesture();
    test_single_double_and_triple();
    test_long_and_expired_taps();
    test_directions_and_invalid_events();
    test_page_barriers();
    puts("LinX input: PASS (default long back, configurable double back, consumed holds, switch/page barriers)");
    return 0;
}
