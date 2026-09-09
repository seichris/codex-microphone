#pragma once
#include "uac_config.h"
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
#if SPEAK_CHANNEL_NUM
    ITF_NUM_AUDIO_STREAMING_SPK,
#endif
#if MIC_CHANNEL_NUM
    ITF_NUM_AUDIO_STREAMING_MIC,
#endif
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};
