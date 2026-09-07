#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "attention_model.h"

typedef void (*attention_ui_open_callback_t)(const char *thread_id, void *context);
typedef void (*attention_ui_focus_callback_t)(const char *thread_id, void *context);

void attention_ui_init(
    attention_ui_open_callback_t open_callback,
    attention_ui_focus_callback_t focus_callback,
    void *context
);
void attention_ui_render(const attention_snapshot_t *snapshot);
// Called under the display lock before polling starts; never allocates a snapshot.
void attention_ui_show_startup_status(const char *message, bool failed);
void attention_ui_set_wireless_status(const char *message);
bool attention_ui_select_next(void);
bool attention_ui_activate_selected(void);
bool attention_ui_get_selected_id(char *output, size_t output_size);
bool attention_ui_get_voice_target_id(char *output, size_t output_size);
void attention_ui_set_voice_state(const char *thread_id, attention_voice_state_t state);
void attention_ui_fixed_focus_failed(void);
void attention_ui_show_list(void);
void attention_ui_show_settings(void);
void attention_ui_show_detail_loading(const char *thread_id);
void attention_ui_render_detail(const attention_detail_t *detail);
void attention_ui_show_detail_error(const char *thread_id, const char *message);
bool attention_ui_is_detail_visible(void);
bool attention_ui_is_settings_visible(void);
bool attention_ui_is_detail_for(const char *thread_id);
