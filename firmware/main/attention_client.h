#pragma once

// Application error: keep an HTTP authentication failure distinct from an
// ESP-IDF transport/client state error.
#define ATTENTION_ERR_UNAUTHORIZED 0x20001

#include "attention_model.h"
#include "esp_err.h"

esp_err_t attention_client_fetch(attention_snapshot_t *snapshot);
esp_err_t attention_client_fetch_detail(const char *thread_id, attention_detail_t *detail);
esp_err_t attention_client_fetch_desktop_state(attention_desktop_state_t *state);
esp_err_t attention_client_focus(
    const char *thread_id,
    const char *request_id,
    attention_desktop_state_t *state
);
esp_err_t attention_client_voice(
    const char *thread_id,
    const char *command,
    const char *request_id,
    attention_desktop_state_t *state
);
