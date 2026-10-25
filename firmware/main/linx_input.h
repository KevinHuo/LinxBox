#ifndef LINX_INPUT_H
#define LINX_INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { LINX_BACK_LONG_PRESS, LINX_BACK_DOUBLE_CLICK } linx_back_mode_t;

typedef enum { LINX_INPUT_PRESS, LINX_INPUT_RELEASE, LINX_INPUT_LONG } linx_input_event_t;
typedef enum { LINX_INPUT_NONE, LINX_INPUT_UP, LINX_INPUT_DOWN,
               LINX_INPUT_OK, LINX_INPUT_BACK } linx_input_action_t;

typedef struct {
    int held_key;
    uint64_t released_ms;
    uint64_t valid_after_ms;
    uint32_t double_ms;
    linx_back_mode_t back_mode;
    bool pending_ok;
    bool long_consumed;
    bool barrier_active;
} linx_input_t;

void linx_input_init(linx_input_t *input, uint32_t double_ms);
/* 模式切换清除手势，并拒绝切换时刻及更早的排队事件。 */
void linx_input_set_back_mode(linx_input_t *input, linx_back_mode_t mode, uint64_t at_ms);
/* 切页清除手势，并使此前排队事件失效。 */
void linx_input_barrier(linx_input_t *input, uint64_t transition_ms);
/* 双击窗口包含截止时刻，单击只在窗口过期后派发。 */
linx_input_action_t linx_input_tick(linx_input_t *input, uint64_t now_ms);
/* key: 0 UP, 1 DOWN, 2 OK；长按模式立即确认，双击模式等待完整两击。 */
linx_input_action_t linx_input_event(linx_input_t *input, unsigned key,
                                    linx_input_event_t event, uint64_t now_ms);

#endif
