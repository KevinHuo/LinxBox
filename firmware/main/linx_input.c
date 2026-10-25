#include "linx_input.h"

void linx_input_init(linx_input_t *input, uint32_t double_ms)
{
    *input = (linx_input_t){ .held_key = -1, .double_ms = double_ms };
}

void linx_input_barrier(linx_input_t *input, uint64_t transition_ms)
{
    input->held_key = -1;
    input->pending_ok = false;
    input->long_consumed = false;
    input->valid_after_ms = transition_ms;
    input->barrier_active = true;
}

void linx_input_set_back_mode(linx_input_t *input, linx_back_mode_t mode, uint64_t at_ms)
{
    input->back_mode = mode == LINX_BACK_DOUBLE_CLICK ? mode : LINX_BACK_LONG_PRESS;
    linx_input_barrier(input, at_ms);
}

linx_input_action_t linx_input_tick(linx_input_t *input, uint64_t now_ms)
{
    if (input->back_mode != LINX_BACK_DOUBLE_CLICK) return LINX_INPUT_NONE;
    if (!input->pending_ok || now_ms <= input->released_ms ||
        now_ms - input->released_ms <= input->double_ms) return LINX_INPUT_NONE;
    input->pending_ok = false;
    return LINX_INPUT_OK;
}

linx_input_action_t linx_input_event(linx_input_t *input, unsigned key,
                                    linx_input_event_t event, uint64_t now_ms)
{
    if (key > 2 || (input->barrier_active && now_ms <= input->valid_after_ms)) return LINX_INPUT_NONE;
    if (event == LINX_INPUT_PRESS) {
        if (input->held_key >= 0) return LINX_INPUT_NONE;
        input->held_key = (int)key;
        input->long_consumed = false;
        if (key == 2) return linx_input_tick(input, now_ms);
        input->pending_ok = false;
        return key == 0 ? LINX_INPUT_UP : LINX_INPUT_DOWN;
    }
    if (input->held_key != (int)key) return LINX_INPUT_NONE;
    if (event == LINX_INPUT_LONG && key == 2) {
        if (input->long_consumed) return LINX_INPUT_NONE;
        input->long_consumed = true;
        input->pending_ok = false;
        return input->back_mode == LINX_BACK_LONG_PRESS ? LINX_INPUT_BACK : LINX_INPUT_NONE;
    }
    if (event == LINX_INPUT_RELEASE) {
        input->held_key = -1;
        if (key != 2 || input->long_consumed) return LINX_INPUT_NONE;
        if (input->back_mode == LINX_BACK_LONG_PRESS) return LINX_INPUT_OK;
        if (input->pending_ok && now_ms >= input->released_ms &&
            now_ms - input->released_ms <= input->double_ms) {
            input->pending_ok = false;
            return LINX_INPUT_BACK;
        }
        linx_input_action_t action = linx_input_tick(input, now_ms);
        input->released_ms = now_ms;
        input->pending_ok = true;
        return action;
    }
    return LINX_INPUT_NONE;
}
