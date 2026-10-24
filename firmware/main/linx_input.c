#include "linx_input.h"

void linx_input_init(linx_input_t *input, uint32_t long_ms)
{
    *input = (linx_input_t){ .held_key = -1, .long_ms = long_ms };
}

void linx_input_barrier(linx_input_t *input, uint64_t transition_ms)
{
    input->held_key = -1;
    input->long_sent = false;
    input->valid_after_ms = transition_ms;
}

linx_input_action_t linx_input_event(linx_input_t *input, unsigned key,
                                    linx_input_event_t event, uint64_t now_ms)
{
    if (key > 2 || (input->valid_after_ms && now_ms <= input->valid_after_ms)) return LINX_INPUT_NONE;
    if (event == LINX_INPUT_PRESS) {
        if (input->held_key >= 0) return LINX_INPUT_NONE;
        input->held_key = (int)key;
        input->pressed_ms = now_ms;
        input->long_sent = false;
        return key == 0 ? LINX_INPUT_UP : key == 1 ? LINX_INPUT_DOWN : LINX_INPUT_NONE;
    }
    if (input->held_key != (int)key) return LINX_INPUT_NONE;
    if (event == LINX_INPUT_LONG) {
        if (key != 2 || input->long_sent) return LINX_INPUT_NONE;
        input->long_sent = true;
        return LINX_INPUT_BACK;
    }
    if (event == LINX_INPUT_RELEASE) {
        input->held_key = -1;
        if (key != 2 || input->long_sent) return LINX_INPUT_NONE;
        return now_ms - input->pressed_ms >= input->long_ms ? LINX_INPUT_BACK : LINX_INPUT_OK;
    }
    return LINX_INPUT_NONE;
}
