#include "attention_audio.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "voice_audio.h"

#define CHIME_SAMPLE_RATE 48000U
#define CHIME_FIRST_TONE_HZ 880U
#define CHIME_SECOND_TONE_HZ 1175U
#define CHIME_FIRST_TONE_MS 110U
#define CHIME_GAP_MS 40U
#define CHIME_SECOND_TONE_MS 150U
#define CHIME_AMPLITUDE 12000
#define CHIME_VOLUME 60
#define CHIME_QUEUE_LENGTH 1
#define CHIME_TASK_STACK 4096
#define CHIME_TASK_PRIORITY 4
#define SINE_TABLE_SIZE 32U

#define CHIME_FIRST_SAMPLES ((CHIME_SAMPLE_RATE * CHIME_FIRST_TONE_MS) / 1000U)
#define CHIME_GAP_SAMPLES ((CHIME_SAMPLE_RATE * CHIME_GAP_MS) / 1000U)
#define CHIME_SECOND_SAMPLES ((CHIME_SAMPLE_RATE * CHIME_SECOND_TONE_MS) / 1000U)
#define CHIME_SAMPLE_COUNT (CHIME_FIRST_SAMPLES + CHIME_GAP_SAMPLES + CHIME_SECOND_SAMPLES)

static const char *TAG = "attention_audio";

/* One period of a 32-sample sine wave, scaled to the int16 range. */
static const int16_t SINE_TABLE[SINE_TABLE_SIZE] = {
    0, 6393, 12539, 18204, 23170, 27245, 30273, 32137,
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
    0, -6393, -12539, -18204, -23170, -27245, -30273, -32137,
    -32767, -32137, -30273, -27245, -23170, -18204, -12539, -6393,
};

static esp_codec_dev_handle_t s_speaker;
static QueueHandle_t s_chime_queue;
static int16_t s_chime_pcm[CHIME_SAMPLE_COUNT];
static atomic_bool s_suppressed;

static void fill_tone(int16_t *destination, size_t sample_count, uint32_t frequency_hz)
{
    const uint32_t phase_step = (uint32_t)(((uint64_t)frequency_hz * SINE_TABLE_SIZE << 16) / CHIME_SAMPLE_RATE);
    uint32_t phase = 0;
    const size_t fade_samples = (CHIME_SAMPLE_RATE * 8U) / 1000U;

    for (size_t index = 0; index < sample_count; ++index) {
        int32_t amplitude = CHIME_AMPLITUDE;
        if (index < fade_samples) {
            amplitude = (int32_t)(((uint32_t)CHIME_AMPLITUDE * index) / fade_samples);
        } else if (sample_count - index <= fade_samples) {
            amplitude = (int32_t)(((uint32_t)CHIME_AMPLITUDE * (sample_count - index)) / fade_samples);
        }

        const int32_t scaled = (int32_t)SINE_TABLE[(phase >> 16) & (SINE_TABLE_SIZE - 1U)] * amplitude;
        destination[index] = (int16_t)(scaled / 32767);
        phase += phase_step;
    }
}

static void build_chime(void)
{
    fill_tone(s_chime_pcm, CHIME_FIRST_SAMPLES, CHIME_FIRST_TONE_HZ);
    for (size_t index = 0; index < CHIME_GAP_SAMPLES; ++index) {
        s_chime_pcm[CHIME_FIRST_SAMPLES + index] = 0;
    }
    fill_tone(
        s_chime_pcm + CHIME_FIRST_SAMPLES + CHIME_GAP_SAMPLES,
        CHIME_SECOND_SAMPLES,
        CHIME_SECOND_TONE_HZ
    );
}

static void play_chime(void)
{
    // The microphone and speaker share the codec/I2S clock. A queued attention
    // sound must not open the speaker while a dictation source owns capture.
    if (s_speaker == NULL || atomic_load(&s_suppressed) || voice_audio_is_listening()) return;
    if (!voice_audio_try_lock_codec(20)) return;
    if (atomic_load(&s_suppressed) || voice_audio_is_listening()) {
        voice_audio_unlock_codec();
        return;
    }

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = CHIME_SAMPLE_RATE,
        .mclk_multiple = 0,
    };

    int result = esp_codec_dev_open(s_speaker, &format);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Could not open speaker: %d", result);
        voice_audio_unlock_codec();
        return;
    }

    result = esp_codec_dev_set_out_vol(s_speaker, CHIME_VOLUME);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Could not set speaker volume: %d", result);
        (void)esp_codec_dev_close(s_speaker);
        voice_audio_unlock_codec();
        return;
    }

    // A long-press may have opened the capture gate while the speaker was
    // being configured. Re-check immediately before the blocking write so a
    // queued notification cannot become microphone crosstalk.
    if (atomic_load(&s_suppressed) || voice_audio_is_listening()) {
        (void)esp_codec_dev_close(s_speaker);
        voice_audio_unlock_codec();
        return;
    }

    result = esp_codec_dev_write(s_speaker, s_chime_pcm, sizeof(s_chime_pcm));
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Could not play attention chime: %d", result);
    }
    (void)esp_codec_dev_close(s_speaker);
    voice_audio_unlock_codec();
}

static void chime_task(void *argument)
{
    (void)argument;
    uint8_t request;

    while (true) {
        if (xQueueReceive(s_chime_queue, &request, portMAX_DELAY) == pdTRUE) {
            play_chime();
        }
    }
}

esp_err_t attention_audio_init(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    if (s_speaker == NULL) {
        ESP_LOGW(TAG, "Speaker codec unavailable; attention sounds disabled");
        return ESP_ERR_NOT_FOUND;
    }

    build_chime();

    s_chime_queue = xQueueCreate(CHIME_QUEUE_LENGTH, sizeof(uint8_t));
    if (s_chime_queue == NULL) {
        ESP_LOGW(TAG, "Could not create attention chime queue");
        esp_codec_dev_delete(s_speaker);
        s_speaker = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(chime_task, "attention_chime", CHIME_TASK_STACK, NULL, CHIME_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Could not create attention chime task");
        vQueueDelete(s_chime_queue);
        s_chime_queue = NULL;
        esp_codec_dev_delete(s_speaker);
        s_speaker = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Attention speaker ready");
    return ESP_OK;
}

void attention_audio_notify(void)
{
    if (s_chime_queue == NULL || atomic_load(&s_suppressed) || voice_audio_is_listening()) return;

    const uint8_t request = 1;
    (void)xQueueOverwrite(s_chime_queue, &request);
}

void attention_audio_set_suppressed(bool suppressed)
{
    atomic_store(&s_suppressed, suppressed);
}
