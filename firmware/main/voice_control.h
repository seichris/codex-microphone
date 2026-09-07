#pragma once

#include <stdbool.h>
#include "attention_model.h"

typedef enum {
    VOICE_CONTROL_ACTION_NONE = 0,
    VOICE_CONTROL_ACTION_FOCUS,
    VOICE_CONTROL_ACTION_START,
    VOICE_CONTROL_ACTION_MUTE,
} voice_control_action_t;

typedef struct {
    attention_voice_state_t state;
    uint64_t recording_started_at_us;
    char thread_id[ATTENTION_ID_MAX];
} voice_control_t;

void voice_control_init(voice_control_t *control);
// Active sessions stop using their retained thread_id, regardless of selection.
// An idle session requires a nonempty selected thread_id to start.
voice_control_action_t voice_control_begin_toggle(voice_control_t *control, const char *thread_id);
voice_control_action_t voice_control_focus_result(
    voice_control_t *control,
    bool success,
    const char *acknowledged_thread_id
);
void voice_control_voice_result(voice_control_t *control, bool success, bool listening);
void voice_control_reconcile(
    voice_control_t *control,
    const char *thread_id,
    attention_voice_state_t state
);

// Only a poll begun after the recording acknowledgement may stop that recording.
bool voice_control_stop_from_remote(voice_control_t *control, uint64_t poll_started_at_us,
    bool remote_available, const char *remote_thread_id, attention_voice_state_t remote_state);
bool voice_control_expire(voice_control_t *control, uint64_t now_us);
