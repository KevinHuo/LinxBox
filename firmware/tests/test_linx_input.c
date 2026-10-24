#include "linx_input.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    linx_input_t input;
    linx_input_init(&input, 500);
    assert(linx_input_event(&input, 3, LINX_INPUT_PRESS, 1) == LINX_INPUT_NONE);
    assert(input.held_key == -1);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 10) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 20) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 100) == LINX_INPUT_OK);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 120) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 180) == LINX_INPUT_OK);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 200) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 700) == LINX_INPUT_BACK);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 800) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 900) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 0, LINX_INPUT_PRESS, 1000) == LINX_INPUT_UP);
    assert(linx_input_event(&input, 1, LINX_INPUT_PRESS, 1001) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 1, LINX_INPUT_RELEASE, 1010) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 0, LINX_INPUT_LONG, 1500) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 0, LINX_INPUT_RELEASE, 1600) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 1, LINX_INPUT_PRESS, 1700) == LINX_INPUT_DOWN);
    assert(linx_input_event(&input, 1, LINX_INPUT_RELEASE, 1800) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 2000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 2600) == LINX_INPUT_BACK);
    /* An OK held across an automatic voice->confirmation transition is spent. */
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 7990) == LINX_INPUT_NONE);
    linx_input_barrier(&input, 8000);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 8040) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 7995) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 8060) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_LONG, 8500) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 9000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 9060) == LINX_INPUT_OK);
    /* 切页时刻本身的排队事件也失效，之后的短按仍可正常使用。 */
    linx_input_barrier(&input, 10000);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 10000) == LINX_INPUT_NONE);
    assert(input.held_key == -1);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 10001) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, (linx_input_event_t)99, 10002) == LINX_INPUT_NONE);
    assert(input.held_key == 2);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 10500) == LINX_INPUT_OK);
    assert(linx_input_event(&input, 2, LINX_INPUT_PRESS, 11000) == LINX_INPUT_NONE);
    assert(linx_input_event(&input, 2, LINX_INPUT_RELEASE, 11500) == LINX_INPUT_BACK);
    puts("LinX input: PASS (short/long exclusivity, quick taps, key identity)");
    return 0;
}
