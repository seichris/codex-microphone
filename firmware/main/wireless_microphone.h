#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wireless_microphone_init(void);
bool wireless_microphone_is_enabled(void);
bool wireless_microphone_is_ready(void);
bool wireless_microphone_has_active_session(void);
/** Return true after a transport/session failure until the next start attempt. */
bool wireless_microphone_has_failed(void);

/** Start one authenticated WSS session and wait for the first audio ack. */
esp_err_t wireless_microphone_start_session(const char *thread_id, const char *request_id);

/** Close the local PCM gate first, then boundedly drain the remote session. */
esp_err_t wireless_microphone_stop_session(void);
