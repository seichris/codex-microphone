#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialize the Waveshare speaker and the attention chime worker.
 *
 * Audio is optional at runtime. A failure to initialize the codec should not
 * prevent the display, buttons, or bridge polling from starting.
 */
esp_err_t attention_audio_init(void);

/**
 * @brief Request one attention chime.
 *
 * Requests are coalesced while a chime is playing so a burst of new threads
 * does not turn into an overlapping or unbounded queue of sounds.
 */
void attention_audio_notify(void);

/** Suppress queued chimes while voice capture is preparing or finishing. */
void attention_audio_set_suppressed(bool suppressed);
