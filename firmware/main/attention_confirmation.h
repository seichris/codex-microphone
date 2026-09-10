#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ATTENTION_CONFIRM_IDLE, ATTENTION_CONFIRM_RELEASE, ATTENTION_CONFIRM_PRESS,
    ATTENTION_CONFIRM_HOLD, ATTENTION_CONFIRM_ACCEPTED, ATTENTION_CONFIRM_CANCELLED,
    ATTENTION_CONFIRM_EXPIRED
} attention_confirmation_phase_t;

typedef struct {
    attention_confirmation_phase_t phase;
    uint64_t deadline_ms, since_ms;
    bool release_started;
} attention_confirmation_t;

void attention_confirmation_begin(attention_confirmation_t *state, uint64_t now_ms);
void attention_confirmation_update(attention_confirmation_t *state, uint64_t now_ms, bool boot_down, bool cancel);
bool attention_confirmation_pending(const attention_confirmation_t *state);
