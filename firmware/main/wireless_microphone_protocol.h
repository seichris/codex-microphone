#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIRELESS_MICROPHONE_PROTOCOL_VERSION 1U
#define WIRELESS_MICROPHONE_SAMPLE_RATE 48000U
#define WIRELESS_MICROPHONE_CHANNELS 1U
#define WIRELESS_MICROPHONE_BITS_PER_SAMPLE 16U
#define WIRELESS_MICROPHONE_SAMPLES_PER_FRAME 960U
#define WIRELESS_MICROPHONE_PCM_BYTES_PER_FRAME \
    (WIRELESS_MICROPHONE_SAMPLES_PER_FRAME * sizeof(int16_t))
#define WIRELESS_MICROPHONE_AUDIO_HEADER_LENGTH 36U
#define WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH 4096U
#define WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH \
    (WIRELESS_MICROPHONE_AUDIO_HEADER_LENGTH + WIRELESS_MICROPHONE_PCM_BYTES_PER_FRAME)

typedef struct {
    const uint8_t *session_id;
    uint32_t sequence;
    uint64_t first_sample;
    const uint8_t *pcm;
    size_t pcm_length;
} wireless_microphone_audio_frame_view_t;

/**
 * Encode one complete 20 ms PCM block. Header integers are network byte
 * order; PCM bytes remain signed little-endian samples.
 */
bool wireless_microphone_encode_audio_frame(
    uint8_t *output,
    size_t output_capacity,
    const uint8_t session_id[16],
    uint32_t sequence,
    uint64_t first_sample,
    const uint8_t *pcm,
    size_t pcm_length,
    size_t *output_length
);

/** Validate a complete binary message without copying its PCM payload. */
bool wireless_microphone_decode_audio_frame(
    const uint8_t *input,
    size_t input_length,
    wireless_microphone_audio_frame_view_t *frame
);
