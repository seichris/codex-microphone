#include "attention_confirmation.h"

bool attention_confirmation_pending(const attention_confirmation_t *s)
{
    return s->phase == ATTENTION_CONFIRM_RELEASE || s->phase == ATTENTION_CONFIRM_PRESS || s->phase == ATTENTION_CONFIRM_HOLD;
}

void attention_confirmation_begin(attention_confirmation_t *s, uint64_t now_ms)
{
    *s = (attention_confirmation_t){ .phase = ATTENTION_CONFIRM_RELEASE, .deadline_ms = now_ms + 60000 };
}

void attention_confirmation_update(attention_confirmation_t *s, uint64_t now_ms, bool boot_down, bool cancel)
{
    if (!attention_confirmation_pending(s)) return;
    if (cancel) { s->phase = ATTENTION_CONFIRM_CANCELLED; return; }
    if (now_ms >= s->deadline_ms) { s->phase = ATTENTION_CONFIRM_EXPIRED; return; }
    if (s->phase == ATTENTION_CONFIRM_RELEASE) {
        if (boot_down) { s->release_started = false; return; }
        if (!s->release_started) { s->release_started = true; s->since_ms = now_ms; }
        if (now_ms - s->since_ms >= 50) s->phase = ATTENTION_CONFIRM_PRESS;
    } else if (s->phase == ATTENTION_CONFIRM_PRESS && boot_down) {
        s->since_ms = now_ms;
        s->phase = ATTENTION_CONFIRM_HOLD;
    } else if (s->phase == ATTENTION_CONFIRM_HOLD) {
        if (!boot_down) s->phase = ATTENTION_CONFIRM_PRESS;
        else if (now_ms - s->since_ms >= 1500) s->phase = ATTENTION_CONFIRM_ACCEPTED;
    }
}
