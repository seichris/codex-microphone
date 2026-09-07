#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "wireless_microphone_protocol.h"

int main(void)
{
    uint8_t session[16];
    for (size_t index = 0; index < sizeof(session); ++index) session[index] = (uint8_t)index;
    uint8_t pcm[WIRELESS_MICROPHONE_PCM_BYTES_PER_FRAME];
    for (size_t index = 0; index < sizeof(pcm); ++index) pcm[index] = (uint8_t)(index * 13U);
    uint8_t encoded[WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH];
    size_t encoded_length = 0;
    assert(wireless_microphone_encode_audio_frame(encoded, sizeof(encoded), session, 7, 6720, pcm, sizeof(pcm), &encoded_length));
    assert(encoded_length == sizeof(encoded));
    assert(encoded[24] == 0 && encoded[25] == 0 && encoded[26] == 0 && encoded[27] == 7);

    wireless_microphone_audio_frame_view_t frame;
    assert(wireless_microphone_decode_audio_frame(encoded, sizeof(encoded), &frame));
    assert(frame.sequence == 7 && frame.first_sample == 6720 && frame.pcm_length == sizeof(pcm));
    assert(memcmp(frame.session_id, session, sizeof(session)) == 0);
    assert(memcmp(frame.pcm, pcm, sizeof(pcm)) == 0);

    encoded[4] = 2;
    assert(!wireless_microphone_decode_audio_frame(encoded, sizeof(encoded), &frame));
    assert(!wireless_microphone_encode_audio_frame(encoded, sizeof(encoded) - 1, session, 0, 0, pcm, sizeof(pcm), &encoded_length));
    return 0;
}
