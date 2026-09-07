#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t wireless_microphone_init(void);
bool wireless_microphone_is_enabled(void);
bool wireless_microphone_is_ready(void);
bool wireless_microphone_has_active_session(void);
bool wireless_microphone_has_failed(void);
bool wireless_microphone_take_failure(void);
/** Consumes only the UI notice, not terminal state; identifies its physical take. */
bool wireless_microphone_take_failure_for(uint32_t *capture_token);
void wireless_microphone_get_status(char *output, size_t capacity);
esp_err_t wireless_microphone_start_session(const char *thread_id, const char *request_id);
/** Use the capture authorization retained at the physical gesture, before focus. */
esp_err_t wireless_microphone_start_session_authorized(const char *thread_id, const char *request_id,
                                                       uint32_t capture_token);
esp_err_t wireless_microphone_stop_session(void);
