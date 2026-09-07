#include "voice_audio.h"

#include <stdatomic.h>
#include <string.h>
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MICROPHONE_GAIN_DB 30.0F
#define CAPTURE_FRAME_BYTES 1920U
#define CAPTURE_RING_FRAMES 25U // 500 ms, bounded tolerance for transient Wi-Fi stalls
#define CAPTURE_TASK_STACK 6144U
#define CAPTURE_TASK_PRIORITY 8U

typedef struct {
    uint8_t pcm[CAPTURE_FRAME_BYTES];
} capture_frame_t;

static const char *TAG = "voice_audio";
static esp_codec_dev_handle_t s_microphone;
static QueueHandle_t s_capture_queue;
static StaticQueue_t s_capture_queue_control;
static uint8_t *s_capture_storage;
static atomic_bool s_ready;
static atomic_uint s_reads_started, s_reads_completed, s_read_errors;
static atomic_uint s_frames_queued, s_frames_discarded, s_frames_dequeued;
static atomic_uint s_open_attempts, s_open_failures, s_close_count;
static SemaphoreHandle_t s_queue_lock;
static SemaphoreHandle_t s_frame_ready;
static SemaphoreHandle_t s_codec_lock;
static TaskHandle_t s_capture_task;
static atomic_bool s_host_muted;
static atomic_bool s_capture_overflow;
static atomic_int s_source;
// Bit 0 is the PCM gate; upper bits are its generation. Keeping both in one
// atomic word lets a concurrent stop invalidate an opening CAS atomically.
static atomic_uint s_capture_epoch;
static capture_frame_t s_usb_pending;
static size_t s_usb_pending_offset;
static bool s_usb_pending_valid;

static bool capture_listening(void)
{
    return (atomic_load(&s_capture_epoch) & 1U) != 0;
}

// Caller owns s_queue_lock. Waking a waiter is also required on stop/source
// changes so it can recheck the gate rather than wait for nonexistent audio.
static void reset_capture_queue_locked(void)
{
    xQueueReset(s_capture_queue);
    s_usb_pending_offset = 0;
    s_usb_pending_valid = false;
    if (s_frame_ready != NULL) xSemaphoreGive(s_frame_ready);
}

static bool receive_frame(uint8_t *pcm, TickType_t timeout, voice_audio_source_t source)
{
    if (pcm == NULL || s_capture_queue == NULL || s_queue_lock == NULL || s_frame_ready == NULL) return false;
    const TickType_t started = xTaskGetTickCount();
    const unsigned int epoch = atomic_load(&s_capture_epoch);
    TickType_t remaining = timeout;
    while (true) {
        if (xSemaphoreTake(s_queue_lock, remaining) != pdTRUE) return false;
        if (!capture_listening() || atomic_load(&s_source) != (int)source
            || atomic_load(&s_capture_epoch) != epoch) {
            xSemaphoreGive(s_queue_lock);
            return false;
        }
        const bool received = xQueueReceive(s_capture_queue, pcm, 0) == pdTRUE;
        xSemaphoreGive(s_queue_lock);
        if (received) { atomic_fetch_add(&s_frames_dequeued, 1); return true; }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) return false;
        remaining = timeout - elapsed;
        // Never wait for PCM while holding the mutex needed by the producer.
        // A binary notification avoids polling and tolerates spurious wakes.
        if (xSemaphoreTake(s_frame_ready, remaining) != pdTRUE) return false;
        const TickType_t waited = xTaskGetTickCount() - started;
        remaining = waited < timeout ? timeout - waited : 0;
    }
}

static void capture_task(void *argument)
{
    (void)argument;
    capture_frame_t frame;
    while (true) {
        if (s_microphone == NULL || s_codec_lock == NULL
            || xSemaphoreTake(s_codec_lock, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const unsigned int epoch_at_read = atomic_load(&s_capture_epoch);
        const int source_at_read = atomic_load(&s_source);
        const bool listening_at_read = capture_listening();
        atomic_fetch_add(&s_reads_started, 1);
        const int codec_result = esp_codec_dev_read(s_microphone, frame.pcm, (int)sizeof(frame.pcm));
        atomic_fetch_add(&s_reads_completed, 1);
        xSemaphoreGive(s_codec_lock);
        if (codec_result != ESP_CODEC_DEV_OK) {
            atomic_fetch_add(&s_read_errors, 1);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        // An in-flight codec read must never cross a gate/session boundary.
        if (!listening_at_read || !capture_listening()) {
            atomic_fetch_add(&s_frames_discarded, 1);
            continue;
        }
        if (s_capture_queue == NULL || s_queue_lock == NULL
            || xSemaphoreTake(s_queue_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
            atomic_store(&s_capture_overflow, true);
            continue;
        }
        const bool still_selected = capture_listening()
            && atomic_load(&s_source) == source_at_read
            && atomic_load(&s_capture_epoch) == epoch_at_read;
        const bool queued = still_selected
            && xQueueSend(s_capture_queue, &frame, 0) == pdTRUE;
        xSemaphoreGive(s_queue_lock);
        if (queued) {
            atomic_fetch_add(&s_frames_queued, 1);
            xSemaphoreGive(s_frame_ready);
        } else if (!still_selected) atomic_fetch_add(&s_frames_discarded, 1);
        if (still_selected && !queued) atomic_store(&s_capture_overflow, true);
    }
}

esp_err_t voice_audio_init(void)
{
    atomic_store(&s_ready, false);
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO
        ),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    esp_err_t result = bsp_audio_init(&i2s_config);
    if (result != ESP_OK) return result;

    s_microphone = bsp_audio_codec_microphone_init();
    if (s_microphone == NULL) return ESP_ERR_NOT_FOUND;

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = VOICE_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    int codec_result = esp_codec_dev_open(s_microphone, &format);
    if (codec_result != ESP_CODEC_DEV_OK) return ESP_FAIL;
    codec_result = esp_codec_dev_set_in_gain(s_microphone, MICROPHONE_GAIN_DB);
    if (codec_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Could not set microphone gain: %d", codec_result);
    }

    // PCM must not consume the internal heap needed by radio/TLS/DMA. The
    // queue control block stays internal; only its 48,000-byte payload is PSRAM.
    s_capture_storage = heap_caps_malloc(CAPTURE_RING_FRAMES * sizeof(capture_frame_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_capture_storage == NULL) return ESP_ERR_NO_MEM;
    s_capture_queue = xQueueCreateStatic(CAPTURE_RING_FRAMES, sizeof(capture_frame_t),
        s_capture_storage, &s_capture_queue_control);
    s_queue_lock = xSemaphoreCreateMutex();
    s_frame_ready = xSemaphoreCreateBinary();
    s_codec_lock = xSemaphoreCreateMutex();
    if (s_capture_queue == NULL || s_queue_lock == NULL || s_codec_lock == NULL || s_frame_ready == NULL) return ESP_ERR_NO_MEM;
    atomic_store(&s_host_muted, false);
    atomic_store(&s_capture_overflow, false);
    atomic_store(&s_source, VOICE_AUDIO_SOURCE_USB);
    atomic_store(&s_capture_epoch, 0);
    s_usb_pending_offset = 0;
    s_usb_pending_valid = false;
    if (xTaskCreate(capture_task, "voice_capture", CAPTURE_TASK_STACK, NULL,
                    CAPTURE_TASK_PRIORITY, &s_capture_task) != pdPASS) {
        s_capture_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_ready, true);
    ESP_LOGI(TAG, "ES7210 microphone ready at %u Hz; one-reader capture ring online", VOICE_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t voice_audio_read(uint8_t *buffer, size_t length, size_t *bytes_read)
{
    if (buffer == NULL || bytes_read == NULL) return ESP_ERR_INVALID_ARG;
    *bytes_read = length;
    const unsigned int epoch = atomic_load(&s_capture_epoch);
    if (s_microphone == NULL || !capture_listening()
        || atomic_load(&s_source) != VOICE_AUDIO_SOURCE_USB) {
        memset(buffer, 0, length);
        return s_microphone == NULL ? ESP_ERR_INVALID_STATE : ESP_OK;
    }

    if (s_capture_queue == NULL || s_queue_lock == NULL
        || xSemaphoreTake(s_queue_lock, 0) != pdTRUE) {
        memset(buffer, 0, length);
        return ESP_OK;
    }
    if (!capture_listening() || atomic_load(&s_source) != VOICE_AUDIO_SOURCE_USB) {
        memset(buffer, 0, length);
        xSemaphoreGive(s_queue_lock);
        return ESP_OK;
    }
    size_t offset = 0;
    while (offset < length) {
        if (!s_usb_pending_valid || s_usb_pending_offset >= sizeof(s_usb_pending.pcm)) {
            s_usb_pending_offset = 0;
            s_usb_pending_valid = xQueueReceive(s_capture_queue, &s_usb_pending, 0) == pdTRUE;
            if (!s_usb_pending_valid) {
                memset(buffer + offset, 0, length - offset);
                break;
            }
        }
        const size_t available = sizeof(s_usb_pending.pcm) - s_usb_pending_offset;
        const size_t amount = available < length - offset ? available : length - offset;
        memcpy(buffer + offset, s_usb_pending.pcm + s_usb_pending_offset, amount);
        s_usb_pending_offset += amount;
        offset += amount;
    }
    xSemaphoreGive(s_queue_lock);
    if (atomic_load(&s_host_muted) || !capture_listening()
        || atomic_load(&s_source) != VOICE_AUDIO_SOURCE_USB
        || atomic_load(&s_capture_epoch) != epoch) memset(buffer, 0, length);
    return ESP_OK;
}

esp_err_t voice_audio_wireless_read_frame(uint8_t *buffer, size_t length, size_t *bytes_read)
{
    if (buffer == NULL || bytes_read == NULL) return ESP_ERR_INVALID_ARG;
    *bytes_read = 0;
    const unsigned int epoch = atomic_load(&s_capture_epoch);
    if (length < CAPTURE_FRAME_BYTES || !capture_listening()
        || atomic_load(&s_source) != VOICE_AUDIO_SOURCE_WIFI) {
        if (length > 0) memset(buffer, 0, length < CAPTURE_FRAME_BYTES ? length : CAPTURE_FRAME_BYTES);
        return ESP_ERR_INVALID_STATE;
    }
    // Receive directly into the caller's frame: no extra 1,920-byte stack copy.
    if (!receive_frame(buffer, pdMS_TO_TICKS(30), VOICE_AUDIO_SOURCE_WIFI)) {
        memset(buffer, 0, CAPTURE_FRAME_BYTES);
        return ESP_ERR_TIMEOUT;
    }
    if (!capture_listening() || atomic_load(&s_source) != VOICE_AUDIO_SOURCE_WIFI
        || atomic_load(&s_capture_epoch) != epoch) {
        memset(buffer, 0, CAPTURE_FRAME_BYTES);
        return ESP_ERR_INVALID_STATE;
    }
    *bytes_read = CAPTURE_FRAME_BYTES;
    return ESP_OK;
}

bool voice_audio_is_ready(void)
{
    return atomic_load(&s_ready);
}

uint32_t voice_audio_capture_token(void)
{
    return atomic_load(&s_capture_epoch);
}

uint32_t voice_audio_revoke_capture(void)
{
    unsigned epoch = atomic_load(&s_capture_epoch);
    unsigned next;
    do { next = (epoch + 2U) & ~1U; }
    while (!atomic_compare_exchange_weak(&s_capture_epoch, &epoch, next));
    atomic_fetch_add(&s_close_count, 1);
    atomic_store(&s_capture_overflow, false);
    if (s_frame_ready != NULL) xSemaphoreGive(s_frame_ready);
    return next;
}

void voice_audio_stop_capture(uint32_t token)
{
    // Cleanup owns only this authorization, never a later physical gesture.
    unsigned epoch = atomic_load(&s_capture_epoch);
    while (epoch == token || epoch == ((token + 2U) | 1U)) {
        if (atomic_compare_exchange_weak(&s_capture_epoch, &epoch,
                                         (epoch + 2U) & ~1U)) {
            atomic_fetch_add(&s_close_count, 1);
            if (s_frame_ready != NULL) xSemaphoreGive(s_frame_ready);
            return;
        }
    }
}

esp_err_t voice_audio_start_capture(voice_audio_source_t source, uint32_t token)
{
    atomic_fetch_add(&s_open_attempts, 1);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if ((token & 1U) || !voice_audio_is_ready()
        || (source != VOICE_AUDIO_SOURCE_USB && source != VOICE_AUDIO_SOURCE_WIFI)
        || s_capture_queue == NULL || s_queue_lock == NULL || s_frame_ready == NULL) {
        atomic_fetch_add(&s_open_failures, 1);
        return result;
    }
    if (xSemaphoreTake(s_queue_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        atomic_fetch_add(&s_open_failures, 1);
        return ESP_ERR_TIMEOUT;
    }
    // The token was captured BEFORE focus/preparation, not at this callback.
    // A Stop before entering this function invalidates it just as a Stop
    // during reset does. Never retry a failed CAS with a fresh authorization.
    if (atomic_load(&s_capture_epoch) == token) {
        atomic_store(&s_source, source);
        reset_capture_queue_locked();
        atomic_store(&s_capture_overflow, false);
        unsigned expected = token;
        if (atomic_compare_exchange_strong(&s_capture_epoch, &expected,
                                            (token + 2U) | 1U)) result = ESP_OK;
    }
    xSemaphoreGive(s_queue_lock);
    if (result != ESP_OK) atomic_fetch_add(&s_open_failures, 1);
    return result;
}

void voice_audio_get_diagnostics(voice_audio_diagnostics_t *out)
{
    if (out == NULL) return;
    *out = (voice_audio_diagnostics_t){
        .ready = voice_audio_is_ready(), .epoch = atomic_load(&s_capture_epoch),
        .source = atomic_load(&s_source),
        .reads_started = atomic_load(&s_reads_started),
        .reads_completed = atomic_load(&s_reads_completed),
        .read_errors = atomic_load(&s_read_errors),
        .queued = atomic_load(&s_frames_queued),
        .dequeued = atomic_load(&s_frames_dequeued),
        .discarded = atomic_load(&s_frames_discarded),
        .open_attempts = atomic_load(&s_open_attempts),
        .open_failures = atomic_load(&s_open_failures),
        .closes = atomic_load(&s_close_count),
    };
}

void voice_audio_set_listening(bool listening)
{
    // Compatibility for synchronous callers. Asynchronous start paths MUST
    // use start_capture with the token retained at the physical gesture.
    if (listening) {
        if (!capture_listening())
            (void)voice_audio_start_capture(voice_audio_source(), voice_audio_capture_token());
        return;
    }
    const uint32_t closed = voice_audio_revoke_capture();
    if (s_capture_queue != NULL && s_queue_lock != NULL
        && xSemaphoreTake(s_queue_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        // A delayed reset must not clear a successor's frames.
        if (atomic_load(&s_capture_epoch) == closed) reset_capture_queue_locked();
        xSemaphoreGive(s_queue_lock);
    }
}

void voice_audio_set_host_muted(bool muted)
{
    atomic_store(&s_host_muted, muted);
}

void voice_audio_set_source(voice_audio_source_t source)
{
    if (source != VOICE_AUDIO_SOURCE_USB && source != VOICE_AUDIO_SOURCE_WIFI) return;
    if (s_capture_queue == NULL || s_queue_lock == NULL
        || xSemaphoreTake(s_queue_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        voice_audio_set_listening(false);
        return;
    }
    if (atomic_load(&s_source) != (int)source) {
        atomic_fetch_add(&s_capture_epoch, 2U);
        atomic_store(&s_source, source);
        reset_capture_queue_locked();
    }
    xSemaphoreGive(s_queue_lock);
}

voice_audio_source_t voice_audio_source(void)
{
    return (voice_audio_source_t)atomic_load(&s_source);
}

bool voice_audio_take_overflow(void)
{
    return atomic_exchange(&s_capture_overflow, false);
}

bool voice_audio_is_listening(void)
{
    if (!capture_listening()) return false;
    return voice_audio_source() == VOICE_AUDIO_SOURCE_WIFI || !atomic_load(&s_host_muted);
}

bool voice_audio_try_lock_codec(uint32_t timeout_ms)
{
    return s_codec_lock != NULL
        && xSemaphoreTake(s_codec_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void voice_audio_unlock_codec(void)
{
    if (s_codec_lock != NULL) xSemaphoreGive(s_codec_lock);
}
