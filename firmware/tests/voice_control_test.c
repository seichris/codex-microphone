#include <assert.h>
#include <string.h>
#include "voice_control.h"

int main(void)
{
    voice_control_t control;
    voice_control_init(&control);
    assert(voice_control_begin_toggle(&control, "task-one") == VOICE_CONTROL_ACTION_FOCUS);
    assert(voice_control_focus_result(&control, true, "task-one") == VOICE_CONTROL_ACTION_START);
    voice_control_voice_result(&control, true, true);
    control.recording_started_at_us = 1000;
    // A stale failure from before the start acknowledgement cannot cancel new audio.
    assert(!voice_control_stop_from_remote(&control, 999, false, NULL, ATTENTION_VOICE_UNKNOWN));
    assert(!voice_control_stop_from_remote(&control, 1001, true, "task-one", ATTENTION_VOICE_LISTENING));
    assert(voice_control_stop_from_remote(&control, 1002, true, "task-one", ATTENTION_VOICE_READY));
    assert(control.state == ATTENTION_VOICE_MUTED);
    voice_control_voice_result(&control, true, true);
    assert(voice_control_stop_from_remote(&control, 1003, true, "task-two", ATTENTION_VOICE_LISTENING));
    voice_control_voice_result(&control, true, true);
    assert(voice_control_stop_from_remote(&control, 1004, false, NULL, ATTENTION_VOICE_UNKNOWN));
    voice_control_voice_result(&control, true, true);
    assert(!voice_control_expire(&control, 1000 + 59999999));
    assert(voice_control_expire(&control, 1000 + 60000000));
    // A physical stop while the start request is pending must not reopen recording.
    assert(voice_control_begin_toggle(&control, "task-one") == VOICE_CONTROL_ACTION_FOCUS);
    assert(voice_control_begin_toggle(&control, "task-one") == VOICE_CONTROL_ACTION_MUTE);
    assert(voice_control_focus_result(&control, true, "task-one") == VOICE_CONTROL_ACTION_NONE);
    assert(control.state == ATTENTION_VOICE_MUTED);
    // UI filtering, Settings, and a changed selection must never strand Stop
    // or redirect a recording's completion to another task.
    const attention_voice_state_t active_states[] = {
        ATTENTION_VOICE_FOCUSING, ATTENTION_VOICE_STARTING, ATTENTION_VOICE_LISTENING,
    };
    const char *selections[] = { NULL, "", "task-two" };
    for (unsigned i = 0; i < sizeof(active_states) / sizeof(active_states[0]); ++i) {
        for (unsigned j = 0; j < sizeof(selections) / sizeof(selections[0]); ++j) {
            voice_control_init(&control);
            strcpy(control.thread_id, "recorded-task");
            control.state = active_states[i];
            assert(voice_control_begin_toggle(&control, selections[j]) == VOICE_CONTROL_ACTION_MUTE);
            assert(strcmp(control.thread_id, "recorded-task") == 0);
            assert(control.state == ATTENTION_VOICE_MUTED);
            assert(voice_control_focus_result(&control, true, "recorded-task") == VOICE_CONTROL_ACTION_NONE);
        }
    }
    voice_control_init(&control);
    assert(voice_control_begin_toggle(&control, NULL) == VOICE_CONTROL_ACTION_NONE);
    assert(voice_control_begin_toggle(&control, "") == VOICE_CONTROL_ACTION_NONE);
    assert(voice_control_begin_toggle(&control, "next-task") == VOICE_CONTROL_ACTION_FOCUS);
    return 0;
}
