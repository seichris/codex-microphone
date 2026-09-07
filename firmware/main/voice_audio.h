#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define VOICE_AUDIO_SAMPLE_RATE 48000U

typedef enum {
    VOICE_AUDIO_SOURCE_USB = 0,
    VOICE_AUDIO_SOURCE_WIFI,
} voice_audio_source_t;

typedef struct {
    bool ready;
    uint32_t epoch;
    int source;
    uint32_t reads_started, reads_completed, read_errors;
    uint32_t queued, dequeued, discarded, open_attempts, open_failures, closes;
} voice_audio_diagnostics_t;

esp_err_t voice_audio_init(void);
bool voice_audio_is_ready(void);
/** Immediately revoke capture before UI/queue/network waits; returns a closed token. */
uint32_t voice_audio_revoke_capture(void);
uint32_t voice_audio_capture_token(void);
/** Atomically select/reset/open only the authorization retained at the gesture. */
esp_err_t voice_audio_start_capture(voice_audio_source_t source, uint32_t token);
/** Revoke only this take; safe even after a successor has started. */
void voice_audio_stop_capture(uint32_t token);
void voice_audio_get_diagnostics(voice_audio_diagnostics_t *out);
esp_err_t voice_audio_read(uint8_t *buffer, size_t length, size_t *bytes_read);
esp_err_t voice_audio_wireless_read_frame(uint8_t *buffer, size_t length, size_t *bytes_read);
void voice_audio_set_listening(bool listening);
void voice_audio_set_host_muted(bool muted);
void voice_audio_set_source(voice_audio_source_t source);
voice_audio_source_t voice_audio_source(void);
bool voice_audio_take_overflow(void);
bool voice_audio_is_listening(void);
bool voice_audio_try_lock_codec(uint32_t timeout_ms);
void voice_audio_unlock_codec(void);
