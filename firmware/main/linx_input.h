#ifndef LINX_INPUT_H
#define LINX_INPUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { LINX_INPUT_PRESS, LINX_INPUT_RELEASE, LINX_INPUT_LONG } linx_input_event_t;
typedef enum { LINX_INPUT_NONE, LINX_INPUT_UP, LINX_INPUT_DOWN,
               LINX_INPUT_OK, LINX_INPUT_BACK } linx_input_action_t;

typedef struct {
    int held_key;
    uint64_t pressed_ms;
    uint64_t valid_after_ms;
    uint32_t long_ms;
    bool long_sent;
} linx_input_t;

void linx_input_init(linx_input_t *input, uint32_t long_ms);
/* A page change invalidates any held gesture and older queued events. */
void linx_input_barrier(linx_input_t *input, uint64_t transition_ms);
/* key: 0 UP, 1 DOWN, 2 OK; one press produces at most one action. */
linx_input_action_t linx_input_event(linx_input_t *input, unsigned key,
                                    linx_input_event_t event, uint64_t now_ms);

#endif
