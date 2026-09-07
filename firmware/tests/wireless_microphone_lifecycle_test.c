// Test production callbacks with deterministic RTOS/network/parsed-JSON shims.
// TLS, cJSON parsing, and actual radio timing still require integration tests.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wireless_stubs/platform.h"
#include "../main/wireless_microphone.c"

static struct fake_semaphore locks[2];
static unsigned lock_count;
static EventBits_t events;
static int64_t time_us;
static bool gate;
static bool close_during_read;
static bool fail_read;
static unsigned transient_read_timeouts;
static bool acknowledge_stop;
static unsigned binary_sends, cancels;
static cJSON *fixture;
static jmp_buf stream_exit;

static char *copy_string(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); assert(p); memcpy(p, s, n); return p; }
size_t strlcpy(char *dst, const char *src, size_t cap) { size_t n = strlen(src); if (cap) { size_t m = n < cap - 1 ? n : cap - 1; memcpy(dst, src, m); dst[m] = 0; } return n; }
cJSON *cJSON_CreateObject(void) { cJSON *v = calloc(1, sizeof(*v)); assert(v); v->type = 1; return v; }
void cJSON_Delete(cJSON *v) { if (!v) return; cJSON_Delete(v->child); cJSON_Delete(v->next); free(v->valuestring); free(v); }
void cJSON_free(void *p) { free(p); }
static cJSON *add_field(cJSON *obj, const char *key) { cJSON *v = cJSON_CreateObject(); v->key = key; v->next = obj->child; obj->child = v; return v; }
void cJSON_AddStringToObject(cJSON *o, const char *k, const char *s) { cJSON *v = add_field(o, k); v->type = 2; v->valuestring = copy_string(s); }
void cJSON_AddNumberToObject(cJSON *o, const char *k, double n) { cJSON *v = add_field(o, k); v->type = 3; v->valuedouble = n; }
const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *o, const char *k) { if (o) for (cJSON *v = o->child; v; v = v->next) if (!strcmp(k, v->key)) return v; return NULL; }
bool cJSON_IsString(const cJSON *v) { return v && v->type == 2; }
bool cJSON_IsNumber(const cJSON *v) { return v && v->type == 3; }
bool cJSON_IsObject(const cJSON *v) { return v && v->type == 1; }
cJSON *cJSON_ParseWithLength(const char *s, size_t n) { assert(n == 7 && !memcmp(s, "fixture", n)); cJSON *v = fixture; fixture = NULL; return v; }
char *cJSON_PrintUnformatted(const cJSON *v) { const cJSON *type = cJSON_GetObjectItemCaseSensitive(v, "type"); assert(type); return copy_string(type->valuestring); }
static void deliver(const char *type, uint32_t sequence) {
    assert(!fixture); fixture = cJSON_CreateObject();
    cJSON_AddStringToObject(fixture, "type", type);
    cJSON_AddNumberToObject(fixture, "version", 1);
    cJSON_AddStringToObject(fixture, "sessionID", s_session_id);
    cJSON_AddNumberToObject(fixture, "generation", (double)s_generation);
    cJSON_AddNumberToObject(fixture, "sequence", sequence);
    cJSON_AddNumberToObject(fixture, "finalSequence", sequence);
    handle_control_message("fixture", 7);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { assert(lock_count < 2); locks[lock_count] = (struct fake_semaphore){1, true}; return &locks[lock_count++]; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) { (void)ticks; if (!s->available) return pdFALSE; s->available = 0; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s) { s->available = 1; return pdTRUE; }
EventGroupHandle_t xEventGroupCreate(void) { events = 0; return &events; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t b) { return *e |= b; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t b) { EventBits_t old = *e; *e &= ~b; return old; }
EventBits_t xEventGroupGetBits(EventGroupHandle_t e) { return *e; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t b, int clear, int all, TickType_t ticks) { (void)b; (void)clear; (void)all; time_us += ticks * 1000; return *e; }
int64_t esp_timer_get_time(void) { return time_us; }
void vTaskDelay(TickType_t ticks) { (void)ticks; longjmp(stream_exit, 1); }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle) { (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return pdPASS; }
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *c) { (void)c; return (void *)1; }
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t c, int id, void (*f)(void *, esp_event_base_t, int32_t, void *), void *a) { (void)c; (void)id; (void)f; (void)a; return ESP_OK; }
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t c) { (void)c; return ESP_OK; }
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t c) { (void)c; return true; }
int esp_websocket_client_send_text(esp_websocket_client_handle_t c, const char *data, int n, TickType_t ticks) {
    (void)c; (void)ticks;
    if (!strcmp(data, "cancel")) ++cancels;
    if (!strcmp(data, "stop") && acknowledge_stop) {
        deliver("ack", s_next_sequence - 1);
        deliver("listening", 0);
        deliver("stopped", s_next_sequence);
    }
    return n;
}
int esp_websocket_client_send_bin(esp_websocket_client_handle_t c, const char *data, int n, TickType_t ticks) {
    (void)c; (void)data; (void)ticks; ++binary_sends;
    // Simulate an ACK callback before send_bin returns on the stream task.
    deliver("ack", s_next_sequence);
    s_streaming = false; return n;
}
void voice_audio_set_listening(bool enabled) { gate = enabled; }
void voice_audio_set_source(voice_audio_source_t source) { (void)source; }
bool voice_audio_is_listening(void) { return gate; }
bool voice_audio_take_overflow(void) { return false; }
esp_err_t voice_audio_wireless_read_frame(uint8_t *pcm, size_t n, size_t *bytes) {
    memset(pcm, 0x35, n); *bytes = n;
    if (close_during_read) gate = false;
    if (close_during_read || fail_read || transient_read_timeouts) {
        if (transient_read_timeouts) --transient_read_timeouts;
        time_us += 30000; // The real capture helper bounds each empty wait.
        *bytes = 0; return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
bool wireless_microphone_encode_audio_frame(uint8_t *out, size_t cap, const uint8_t id[16], uint32_t seq, uint64_t sample, const uint8_t *pcm, size_t n, size_t *len) {
    (void)id; (void)seq; (void)sample; (void)pcm; assert(cap >= n + 36); memset(out, 0, cap); *len = n + 36; return true;
}
static void setup(void) {
    lock_count = 0; time_us = 100000; cancels = 0; binary_sends = 0;
    gate = true; close_during_read = false; fail_read = false; acknowledge_stop = false; transient_read_timeouts = 0;
    s_connected = true; s_authenticated = true; s_streaming = true; s_armed = true;
    s_pending_start = false; s_cancel_requested = false; s_failed_session = false;
    s_stopping = false; s_send_in_flight = false; s_have_ack = false;
    s_generation = 1; s_next_sequence = 5; s_last_ack_ms = 0;
    strcpy(s_session_id, "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE");
    assert(wireless_microphone_init() == ESP_OK);
}
static void stream_once(void) { if (setjmp(stream_exit) == 0) stream_task(NULL); }
int main(void) {
    setup(); acknowledge_stop = true;
    assert(wireless_microphone_stop_session() == ESP_OK);
    assert(!gate && !wireless_microphone_has_active_session() && !wireless_microphone_has_failed() && cancels == 0);
    assert(!(events & (WIRELESS_EVENT_LISTENING | WIRELESS_EVENT_FAILED)));
    puts("PASS stop drains valid late ACK/listening without reopening or failing");
    setup(); deliver("armed", 0);
    assert(s_next_sequence == 5 && gate);
    puts("PASS duplicate armed preserves live sequence");
    setup(); s_armed = false; s_streaming = false; s_pending_start = true; s_cancel_requested = true; gate = false;
    deliver("armed", 0); assert(!gate && !s_armed && s_next_sequence == 5);
    puts("PASS late armed cannot reopen canceled start");
    setup(); s_stopping = true; s_armed = false; gate = false;
    deliver("armed", 0); assert(!gate && !s_armed && s_next_sequence == 5);
    puts("PASS late armed cannot reopen stopping session");
    setup(); s_pending_start = true; s_armed = false; s_streaming = false; gate = false;
    deliver("armed", 0); assert(gate && s_armed && s_next_sequence == 0);
    puts("PASS current pending session still arms normally");
    setup(); deliver("ack", 4); assert(s_last_ack_ms == 100);
    time_us = 200000; deliver("ack", 4); deliver("ack", 3); assert(s_last_ack_ms == 100 && !s_failed_session);
    puts("PASS duplicate/regressive ACKs cannot refresh liveness");
    setup(); deliver("ack", 5); assert(wireless_microphone_has_failed() && !gate);
    puts("PASS unsent sequence ACK fails closed");
    setup(); s_next_sequence = 0; stream_once();
    assert(binary_sends == 1 && s_next_sequence == 1 && !s_failed_session && !s_send_in_flight);
    puts("PASS in-flight ACK before send return is accepted");
    setup(); close_during_read = true; stream_once();
    assert(!s_failed_session && cancels == 0 && binary_sends == 0);
    puts("PASS physical gate closure cancels pending read without failing stop");
    setup(); transient_read_timeouts = 1; s_next_sequence = 0; stream_once();
    assert(binary_sends == 1 && s_next_sequence == 1 && !s_failed_session);
    puts("PASS discarded pre-arm read may span two periods without aborting startup");
    setup(); fail_read = true; s_next_sequence = 0; stream_once();
    assert(s_failed_session && !gate && binary_sends == 0);
    assert(time_us > 500000 && time_us <= 530000);
    puts("PASS missing first PCM fails closed within existing ACK liveness budget");
    setup(); esp_websocket_event_data_t pong = { .op_code = 0xA, .fin = true };
    websocket_event_handler(NULL, NULL, WEBSOCKET_EVENT_DATA, &pong);
    assert(!s_failed_session && gate);
    puts("PASS empty WebSocket pong is not parsed as microphone JSON");
    setup(); esp_websocket_event_data_t partial = { .op_code = 1, .fin = true, .data_ptr = "x", .data_len = 1, .payload_len = 2 };
    websocket_event_handler(NULL, NULL, WEBSOCKET_EVENT_DATA, &partial);
    assert(s_failed_session && !gate);
    puts("PASS incomplete control payload fails closed before JSON parsing");
    setup(); assert(wireless_microphone_stop_session() == ESP_ERR_TIMEOUT);
    assert(s_failed_session && !gate && cancels == 1);
    puts("PASS stop timeout latches failure before clearing session state");
    return 0;
}
