#include "voice_control.h"

#include <string.h>

static void copy_thread_id(char *destination, const char *source, size_t size)
{
    size_t length = strlen(source);
    if (length >= size) length = size - 1;
    memmove(destination, source, length);
    destination[length] = '\0';
}

void voice_control_init(voice_control_t *control)
{
    if (control == NULL) return;
    memset(control, 0, sizeof(*control));
    control->state = ATTENTION_VOICE_READY;
}

voice_control_action_t voice_control_begin_toggle(voice_control_t *control, const char *thread_id)
{
    if (control == NULL) return VOICE_CONTROL_ACTION_NONE;
    // The second gesture belongs to the active session, even if a refresh
    // removed its card, Settings is open, or another task is now selected.
    if (control->thread_id[0] != '\0' && (control->state == ATTENTION_VOICE_FOCUSING
        || control->state == ATTENTION_VOICE_STARTING
        || control->state == ATTENTION_VOICE_LISTENING)) {
        control->state = ATTENTION_VOICE_MUTED;
        return VOICE_CONTROL_ACTION_MUTE;
    }
    if (thread_id == NULL || thread_id[0] == '\0') return VOICE_CONTROL_ACTION_NONE;

    copy_thread_id(control->thread_id, thread_id, sizeof(control->thread_id));
    control->state = ATTENTION_VOICE_FOCUSING;
    return VOICE_CONTROL_ACTION_FOCUS;
}

voice_control_action_t voice_control_focus_result(
    voice_control_t *control,
    bool success,
    const char *acknowledged_thread_id
)
{
    if (control == NULL) return VOICE_CONTROL_ACTION_NONE;
    if (control->state != ATTENTION_VOICE_FOCUSING) {
        return VOICE_CONTROL_ACTION_NONE;
    }
    if (!success || acknowledged_thread_id == NULL
        || strcmp(control->thread_id, acknowledged_thread_id) != 0) {
        control->state = ATTENTION_VOICE_ERROR;
        return VOICE_CONTROL_ACTION_NONE;
    }
    control->state = ATTENTION_VOICE_STARTING;
    return VOICE_CONTROL_ACTION_START;
}

void voice_control_voice_result(voice_control_t *control, bool success, bool listening)
{
    if (control == NULL) return;
    control->state = success
        ? (listening ? ATTENTION_VOICE_LISTENING : ATTENTION_VOICE_MUTED)
        : ATTENTION_VOICE_ERROR;
}

void voice_control_reconcile(
    voice_control_t *control,
    const char *thread_id,
    attention_voice_state_t state
)
{
    if (control == NULL) return;
    if (thread_id != NULL && thread_id[0] != '\0') {
        copy_thread_id(control->thread_id, thread_id, sizeof(control->thread_id));
    }
    control->state = state;
}

bool voice_control_stop_from_remote(voice_control_t *control, uint64_t poll_started_at_us,
    bool remote_available, const char *remote_thread_id, attention_voice_state_t remote_state)
{
    if (control == NULL || control->state != ATTENTION_VOICE_LISTENING
        || poll_started_at_us <= control->recording_started_at_us) return false;
    if (remote_available && remote_thread_id != NULL
        && strcmp(control->thread_id, remote_thread_id) == 0
        && remote_state == ATTENTION_VOICE_LISTENING) return false;
    control->state = ATTENTION_VOICE_MUTED;
    return true;
}

bool voice_control_expire(voice_control_t *control, uint64_t now_us)
{
    if (control == NULL || control->state != ATTENTION_VOICE_LISTENING
        || now_us < control->recording_started_at_us
        || now_us - control->recording_started_at_us < 60000000ULL) return false;
    control->state = ATTENTION_VOICE_MUTED;
    return true;
}
