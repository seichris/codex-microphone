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

esp_err_t voice_audio_init(void);
esp_err_t voice_audio_read(uint8_t *buffer, size_t length, size_t *bytes_read);
esp_err_t voice_audio_wireless_read_frame(uint8_t *buffer, size_t length, size_t *bytes_read);
void voice_audio_set_listening(bool listening);
void voice_audio_set_host_muted(bool muted);
void voice_audio_set_source(voice_audio_source_t source);
voice_audio_source_t voice_audio_source(void);
bool voice_audio_take_overflow(void);
bool voice_audio_is_listening(void);

/** Serialize microphone codec access with the shared speaker codec. */
bool voice_audio_try_lock_codec(uint32_t timeout_ms);
void voice_audio_unlock_codec(void);
