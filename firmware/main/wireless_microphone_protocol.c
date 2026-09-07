#include "wireless_microphone_protocol.h"

#include <string.h>

static const uint8_t AUDIO_MAGIC[4] = { 0x43, 0x4D, 0x49, 0x43 }; /* CMIC */

static void put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void put_u64(uint8_t *output, uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) output[7 - (shift / 8)] = (uint8_t)(value >> shift);
}

static uint16_t get_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static uint32_t get_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16)
        | ((uint32_t)input[2] << 8) | input[3];
}

static uint64_t get_u64(const uint8_t *input)
{
    uint64_t value = 0;
    for (size_t index = 0; index < sizeof(value); ++index) value = (value << 8) | input[index];
    return value;
}

bool wireless_microphone_encode_audio_frame(
    uint8_t *output,
    size_t output_capacity,
    const uint8_t session_id[16],
    uint32_t sequence,
    uint64_t first_sample,
    const uint8_t *pcm,
    size_t pcm_length,
    size_t *output_length
)
{
    if (output == NULL || session_id == NULL || pcm == NULL || output_length == NULL
        || pcm_length != WIRELESS_MICROPHONE_PCM_BYTES_PER_FRAME
        || output_capacity < WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH) {
        return false;
    }

    memcpy(output, AUDIO_MAGIC, sizeof(AUDIO_MAGIC));
    output[4] = WIRELESS_MICROPHONE_PROTOCOL_VERSION;
    output[5] = 0;
    put_u16(output + 6, WIRELESS_MICROPHONE_AUDIO_HEADER_LENGTH);
    memcpy(output + 8, session_id, 16);
    put_u32(output + 24, sequence);
    put_u64(output + 28, first_sample);
    memcpy(output + WIRELESS_MICROPHONE_AUDIO_HEADER_LENGTH, pcm, pcm_length);
    *output_length = WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH;
    return true;
}

bool wireless_microphone_decode_audio_frame(
    const uint8_t *input,
    size_t input_length,
    wireless_microphone_audio_frame_view_t *frame
)
{
    if (input == NULL || frame == NULL || input_length != WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH
        || memcmp(input, AUDIO_MAGIC, sizeof(AUDIO_MAGIC)) != 0
        || input[4] != WIRELESS_MICROPHONE_PROTOCOL_VERSION || input[5] != 0
        || get_u16(input + 6) != WIRELESS_MICROPHONE_AUDIO_HEADER_LENGTH) {
        return false;
    }

    frame->session_id = input + 8;
    frame->sequence = get_u32(input + 24);
    frame->first_sample = get_u64(input + 28);
    frame->pcm = input + WIRELESS_MICROPHONE_AUDIO_HEADER_LENGTH;
    frame->pcm_length = WIRELESS_MICROPHONE_PCM_BYTES_PER_FRAME;
    return true;
}
