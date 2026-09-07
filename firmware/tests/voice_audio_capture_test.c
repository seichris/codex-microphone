// Deterministic scheduler/codec shim; compile the actual production translation
// unit, not a reimplementation of receive_frame or its gate logic.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_stubs/platform.h"
#include "../main/voice_audio.c"

struct fake_queue { unsigned capacity, count, head; size_t width; uint8_t data[10][1920]; };
static struct fake_queue queue_storage;
static struct fake_semaphore semaphores[4];
static unsigned semaphore_count;
static TickType_t ticks;
static void (*on_wait)(void);
static void (*during_read)(void);
static void (*during_reset)(void);
static void (*capture_entry)(void *);
static jmp_buf capture_exit;
static unsigned codec_reads;
static unsigned blocking_waits;

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    assert(semaphore_count < 4);
    SemaphoreHandle_t s = &semaphores[semaphore_count++];
    *s = (struct fake_semaphore){ .available = 1, .mutex = true }; return s;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    SemaphoreHandle_t s = xSemaphoreCreateMutex(); s->available = 0; s->mutex = false; return s;
}
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout) {
    if (!s->available && timeout && !s->mutex) {
        // A wait while holding the producer mutex is a deadlock, not a fix.
        assert(s_queue_lock->available);
        ++blocking_waits;
        if (on_wait) {
            void (*action)(void) = on_wait; on_wait = NULL;
            ticks += timeout < 20 ? timeout : 20; action();
        } else { ticks += timeout; }
    }
    if (!s->available) return pdFALSE;
    s->available = 0; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t s) { s->available = 1; return pdTRUE; }
QueueHandle_t xQueueCreate(unsigned capacity, size_t width) {
    assert(capacity <= 10 && width == 1920);
    queue_storage = (struct fake_queue){ .capacity = capacity, .width = width }; return &queue_storage;
}
int xQueueReset(QueueHandle_t q) { q->head = 0; q->count = 0;
    if (during_reset) { void (*action)(void) = during_reset; during_reset = NULL; action(); }
    return pdTRUE;
}
int xQueueSend(QueueHandle_t q, const void *data, TickType_t timeout) {
    assert(timeout == 0);
    if (q->count == q->capacity) return pdFALSE;
    memcpy(q->data[(q->head + q->count) % q->capacity], data, q->width); ++q->count; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *data, TickType_t timeout) {
    assert(timeout == 0);
    if (!q->count) return pdFALSE;
    memcpy(data, q->data[q->head], q->width); q->head = (q->head + 1) % q->capacity; --q->count; return pdTRUE;
}
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle) {
    (void)name; (void)stack; (void)arg; (void)priority; capture_entry = entry; *handle = (void *)1; return pdPASS;
}
esp_err_t bsp_audio_init(const i2s_std_config_t *config) { (void)config; return ESP_OK; }
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void) { return (void *)1; }
int esp_codec_dev_open(esp_codec_dev_handle_t dev, const esp_codec_dev_sample_info_t *format) { (void)dev; (void)format; return ESP_CODEC_DEV_OK; }
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t dev, float gain) { (void)dev; (void)gain; return ESP_CODEC_DEV_OK; }
int esp_codec_dev_read(esp_codec_dev_handle_t dev, void *data, int length) {
    (void)dev;
    if (codec_reads++) longjmp(capture_exit, 1);
    memset(data, 0x35, (size_t)length);
    if (during_read) { void (*action)(void) = during_read; during_read = NULL; action(); }
    return ESP_CODEC_DEV_OK;
}
static void capture_one(void) {
    codec_reads = 0;
    if (setjmp(capture_exit) == 0) capture_entry(NULL);
    // The fake scheduler interrupts at entry to the second blocking codec read.
    s_codec_lock->available = 1;
}
static void stop(void) { voice_audio_set_listening(false); }
static void restart(void) { voice_audio_set_listening(false); voice_audio_set_listening(true); }
static void start(void) { voice_audio_set_listening(true); }
static void switch_source(void) { voice_audio_set_source(VOICE_AUDIO_SOURCE_USB); }
static void setup(void) {
    semaphore_count = 0; ticks = 0; blocking_waits = 0; on_wait = NULL; during_read = NULL; during_reset = NULL;
    assert(voice_audio_init() == ESP_OK);
    voice_audio_set_source(VOICE_AUDIO_SOURCE_WIFI); voice_audio_set_listening(true);
}
static void expect_silence(const uint8_t *data, size_t size) { for (size_t i = 0; i < size; ++i) assert(data[i] == 0); }

int main(void) {
    uint8_t pcm[1920]; size_t bytes;
    setup(); on_wait = capture_one;
    assert(voice_audio_wireless_read_frame(pcm, sizeof(pcm), &bytes) == ESP_OK);
    assert(bytes == sizeof(pcm) && pcm[0] == 0x35 && blocking_waits == 1 && ticks == 20);
    puts("PASS empty ring waits for first PCM without holding producer mutex");
    for (unsigned i = 0; i < 100; ++i) {
        on_wait = capture_one;
        assert(voice_audio_wireless_read_frame(pcm, sizeof(pcm), &bytes) == ESP_OK);
    }
    puts("PASS consecutive reads wait for subsequent 20 ms frames");
    setup(); memset(pcm, 0xFF, sizeof(pcm));
    assert(voice_audio_wireless_read_frame(pcm, sizeof(pcm), &bytes) == ESP_ERR_TIMEOUT);
    assert(bytes == 0 && ticks == 30); expect_silence(pcm, sizeof(pcm));
    puts("PASS missing PCM retains bounded timeout and reports no delivered bytes");
    setup(); ticks = UINT32_MAX - 10; on_wait = capture_one;
    assert(voice_audio_wireless_read_frame(pcm, sizeof(pcm), &bytes) == ESP_OK);
    puts("PASS wait deadline tolerates tick wraparound");
    setup(); on_wait = stop;
    assert(voice_audio_wireless_read_frame(pcm, sizeof(pcm), &bytes) != ESP_OK);
    assert(bytes == 0); expect_silence(pcm, sizeof(pcm));
    puts("PASS stop wakes pending read and returns no PCM");
    setup(); on_wait = restart;
    assert(voice_audio_wireless_read_frame(pcm, sizeof(pcm), &bytes) != ESP_OK);
    assert(bytes == 0);
    puts("PASS old waiter cannot consume a restarted session");
    setup(); on_wait = switch_source;
    assert(voice_audio_wireless_read_frame(pcm, sizeof(pcm), &bytes) != ESP_OK);
    assert(bytes == 0);
    puts("PASS source change wakes pending Wi-Fi read");
    setup(); voice_audio_set_listening(false); during_read = start; capture_one();
    assert(queue_storage.count == 0);
    puts("PASS codec read begun before arming is discarded");
    setup(); during_read = restart; capture_one();
    assert(queue_storage.count == 0);
    capture_one(); assert(queue_storage.count == 1);
    puts("PASS codec read spanning stop/start is discarded, fresh read retained");
    setup(); capture_one(); voice_audio_set_source(VOICE_AUDIO_SOURCE_USB);
    assert(queue_storage.count == 0);
    memset(pcm, 0xFF, sizeof(pcm));
    assert(voice_audio_read(pcm, sizeof(pcm), &bytes) == ESP_OK); expect_silence(pcm, sizeof(pcm));
    puts("PASS Wi-Fi PCM cannot leak into USB after source change");
    setup(); voice_audio_set_source(VOICE_AUDIO_SOURCE_USB); capture_one();
    assert(voice_audio_read(pcm, 96, &bytes) == ESP_OK && pcm[0] == 0x35);
    voice_audio_set_listening(false); voice_audio_set_listening(true);
    assert(voice_audio_read(pcm, 96, &bytes) == ESP_OK); expect_silence(pcm, 96);
    puts("PASS stop clears partially consumed USB frame");
    setup(); for (unsigned i = 0; i < 11; ++i) capture_one();
    assert(voice_audio_take_overflow()); assert(!voice_audio_take_overflow());
    puts("PASS ring overflow remains latched and bounded");
    setup(); voice_audio_set_listening(false); during_reset = stop;
    voice_audio_set_listening(true);
    assert(!voice_audio_is_listening());
    puts("PASS stop between reset and gate opening wins atomically");
    return 0;
}
