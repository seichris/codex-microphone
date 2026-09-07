#include "wireless_microphone.h"

#include <ctype.h>
#include <stdatomic.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "voice_audio.h"
#include "wireless_microphone_protocol.h"
#include "wireless_certificate.h"
#include "wifi_manager.h"
#include "mbedtls/x509_crt.h"

#define WIRELESS_EVENT_CONNECTED BIT0
#define WIRELESS_EVENT_AUTHENTICATED BIT1
#define WIRELESS_EVENT_LISTENING BIT2
#define WIRELESS_EVENT_FAILED BIT3
#define WIRELESS_EVENT_STOPPED BIT4
#define WIRELESS_EVENT_CANCELED BIT5
#define WIRELESS_EVENT_WORK BIT6
#define WIRELESS_ACK_TIMEOUT_MS 1000U
#define WIRELESS_START_TIMEOUT_MS 5000U
#define WIRELESS_STOP_DRAIN_MS 250U
#define WIRELESS_SEND_TIMEOUT_MS 250U
#define WIRELESS_STREAM_STACK 8192U
#define WIRELESS_STREAM_PRIORITY 7U

static bool parse_uuid(const char *text, uint8_t output[16])
{
    if (text == NULL || output == NULL || strlen(text) != 36) return false;
    if (text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') return false;
    size_t output_index = 0;
    for (size_t index = 0; index < 36;) {
        if (text[index] == '-') { ++index; continue; }
        if (index + 1 >= 36 || output_index >= 16) return false;
        if (!isxdigit((unsigned char)text[index]) || !isxdigit((unsigned char)text[index + 1])) return false;
        unsigned int high = (unsigned int)(isdigit((unsigned char)text[index])
            ? text[index] - '0' : tolower((unsigned char)text[index]) - 'a' + 10);
        unsigned int low = (unsigned int)(isdigit((unsigned char)text[index + 1])
            ? text[index + 1] - '0' : tolower((unsigned char)text[index + 1]) - 'a' + 10);
        output[output_index++] = (uint8_t)((high << 4) | low);
        index += 2;
    }
    return output_index == 16;
}

static bool json_u64(const cJSON *item, uint64_t *value)
{
    if (item == NULL || value == NULL || !cJSON_IsNumber(item)
        || !isfinite(item->valuedouble) || item->valuedouble < 0
        || item->valuedouble >= 0x1p64) return false;
    const uint64_t converted = (uint64_t)item->valuedouble;
    if ((double)converted != item->valuedouble) return false;
    *value = converted;
    return true;
}

static bool json_u32(const cJSON *item, uint32_t *value)
{
    if (item == NULL || value == NULL || !cJSON_IsNumber(item)
        || !isfinite(item->valuedouble) || item->valuedouble < 0
        || item->valuedouble >= 0x1p32) return false;
    const uint32_t converted = (uint32_t)item->valuedouble;
    if ((double)converted != item->valuedouble) return false;
    *value = converted;
    return true;
}

static bool valid_format(const cJSON *format)
{
    if (format == NULL || !cJSON_IsObject(format)) return false;
    const cJSON *sample_rate = cJSON_GetObjectItemCaseSensitive(format, "sampleRate");
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(format, "channels");
    const cJSON *bits = cJSON_GetObjectItemCaseSensitive(format, "bitsPerSample");
    const cJSON *samples = cJSON_GetObjectItemCaseSensitive(format, "samplesPerFrame");
    uint32_t value = 0;
    return json_u32(sample_rate, &value) && value == WIRELESS_MICROPHONE_SAMPLE_RATE
        && json_u32(channels, &value) && value == WIRELESS_MICROPHONE_CHANNELS
        && json_u32(bits, &value) && value == WIRELESS_MICROPHONE_BITS_PER_SAMPLE
        && json_u32(samples, &value) && value == WIRELESS_MICROPHONE_SAMPLES_PER_FRAME;
}

static const char *TAG = "wireless_microphone";
static esp_websocket_client_handle_t s_client;
static char *s_certificate_pem;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_state_lock, s_send_lock, s_power_lock;
static bool s_enabled;
static bool s_connected, s_authenticated;
static bool s_transport_poisoned;
static esp_websocket_error_codes_t s_last_connection_error;

// This enum is the authoritative recording lifecycle. Waiter/in-flight flags
// below describe outstanding operations, not competing recording states.
typedef enum {
    SESSION_IDLE, SESSION_PREPARING, SESSION_WAIT_ARMED,
    SESSION_OPENING_CAPTURE, SESSION_STREAMING, SESSION_STOPPING,
    SESSION_FAILED, SESSION_CANCELED,
} session_phase_t;
static session_phase_t s_phase;
static uint32_t s_attempt, s_capture_token;
static bool s_start_waiter, s_stop_waiter, s_send_in_flight;
static bool s_recovery_requested, s_recovering;
static bool s_failure_notice;
static uint32_t s_failure_token;
static char s_last_session_failure[64];
static uint32_t s_failure_sequence;
static char s_session_id[37], s_request_id[97];
static uint8_t s_session_uuid[16];
static uint64_t s_generation;
static uint32_t s_next_sequence, s_last_ack_sequence;
static bool s_have_ack;
static int64_t s_started_ms, s_armed_ms, s_last_pcm_ms, s_last_ack_ms;
static bool s_wifi_power_save_saved;
static wifi_ps_type_t s_idle_wifi_power_save;

// Counts are metadata only. Monotonic timestamps and phase snapshots are
// emitted at transitions/failure, never with PCM/control JSON/task IDs.
static atomic_uint s_armed_received, s_armed_rejected, s_stream_ticks;
static atomic_uint s_read_attempts, s_send_attempts, s_send_completed;
static atomic_uint s_send_lock_timeouts;
static int64_t s_last_stream_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool lock_state(TickType_t timeout)
{
    // The state mutex protects memory only. Never take another mutex, perform
    // I/O, or call capture setters while holding it. A busy mutex is not idle.
    (void)timeout;
    return s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE;
}
static void unlock_state(void) { xSemaphoreGive(s_state_lock); }
static bool active_locked(void)
{
    return s_phase >= SESSION_PREPARING && s_phase <= SESSION_STOPPING;
}
static uint32_t current_attempt(void)
{
    if (!lock_state(0)) return 0;
    uint32_t value = s_attempt;
    unlock_state();
    return value;
}
static bool matches_locked(const cJSON *message)
{
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(message, "sessionID");
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(message, "generation");
    uint64_t parsed = 0;
    return cJSON_IsString(session) && json_u64(generation, &parsed)
        && parsed == s_generation && parsed != 0
        && strcmp(session->valuestring, s_session_id) == 0;
}

static void diagnose(uint32_t attempt, const char *stage)
{
    voice_audio_diagnostics_t audio;
    voice_audio_get_diagnostics(&audio);
    if (!lock_state(0)) return;
    if (attempt != s_attempt) { unlock_state(); return; }
    const unsigned phase = (unsigned)s_phase;
    const uint32_t token = s_capture_token, next = s_next_sequence;
    const int64_t stream_age = now_ms() - s_last_stream_ms;
    unlock_state();
    ESP_LOGW(TAG, "mic a=%lu stage=%s phase=%u token=%lu epoch=%lu src=%d ready=%d seq=%lu "
        "arm=%u reject=%u loop=%u age=%" PRId64 " read=%u send=%u/%u lock=%u "
        "codec=%lu/%lu err=%lu q=%lu/%lu drop=%lu open=%lu/%lu close=%lu",
        (unsigned long)attempt, stage, phase, (unsigned long)token,
        (unsigned long)audio.epoch, audio.source, audio.ready, (unsigned long)next,
        atomic_load(&s_armed_received), atomic_load(&s_armed_rejected),
        atomic_load(&s_stream_ticks), stream_age, atomic_load(&s_read_attempts),
        atomic_load(&s_send_completed), atomic_load(&s_send_attempts),
        atomic_load(&s_send_lock_timeouts), (unsigned long)audio.reads_completed,
        (unsigned long)audio.reads_started, (unsigned long)audio.read_errors,
        (unsigned long)audio.dequeued, (unsigned long)audio.queued,
        (unsigned long)audio.discarded, (unsigned long)audio.open_failures,
        (unsigned long)audio.open_attempts, (unsigned long)audio.closes);
}

static void fail_session_in_phase(uint32_t attempt, int expected_phase, const char *reason)
{
    if (!lock_state(0)) return;
    if (attempt != s_attempt || !active_locked()
        || (expected_phase >= 0 && (int)s_phase != expected_phase)) { unlock_state(); return; }
    const uint32_t token = s_capture_token;
    unlock_state();
    diagnose(attempt, reason); // Snapshot before terminal cleanup changes phase/gate.
    if (!lock_state(0)) return;
    if (attempt != s_attempt || !active_locked()
        || (expected_phase >= 0 && (int)s_phase != expected_phase)) { unlock_state(); return; }
    s_phase = SESSION_FAILED;
    s_failure_notice = true;
    s_failure_token = token;
    if (!s_last_session_failure[0]) {
        strlcpy(s_last_session_failure, reason, sizeof(s_last_session_failure));
        s_failure_sequence = s_next_sequence;
    }
    s_recovery_requested = true;
    unlock_state();
    voice_audio_stop_capture(token);
    xEventGroupSetBits(s_events, WIRELESS_EVENT_FAILED | WIRELESS_EVENT_WORK);
    // Never send/stop/restart from a library callback, including callbacks
    // synchronously raised inside send_bin(). The supervisor retires this
    // connection; no packet is retried into a possibly partial WS stream.
}

static void fail_session(uint32_t attempt, const char *reason)
{
    fail_session_in_phase(attempt, -1, reason);
}

static esp_err_t recording_power(bool enable, uint32_t attempt)
{
    if (s_power_lock == NULL || xSemaphoreTake(s_power_lock, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_OK;
    if (enable) {
        lock_state(0);
        const bool current = s_attempt == attempt && s_phase == SESSION_PREPARING
            && !s_recovery_requested && !s_recovering;
        unlock_state();
        if (!current) {
            xSemaphoreGive(s_power_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (enable) {
        if (!s_wifi_power_save_saved) {
            result = esp_wifi_get_ps(&s_idle_wifi_power_save);
            if (result == ESP_OK) s_wifi_power_save_saved = true;
        }
        if (result == ESP_OK) result = esp_wifi_set_ps(WIFI_PS_NONE);
    } else if (s_wifi_power_save_saved) {
        result = esp_wifi_set_ps(s_idle_wifi_power_save);
        if (result == ESP_OK) s_wifi_power_save_saved = false;
    }
    xSemaphoreGive(s_power_lock);
    return result;
}

static esp_err_t send_json_unlocked(cJSON *message)
{
    if (message == NULL) return ESP_ERR_NO_MEM;
    char *encoded = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (encoded == NULL) return ESP_ERR_NO_MEM;
    const size_t size = strlen(encoded);
    esp_err_t result = ESP_FAIL;
    if (s_client != NULL && size <= WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH
        && esp_websocket_client_is_connected(s_client)) {
        const int sent = esp_websocket_client_send_text(s_client, encoded, (int)size,
            pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS));
        if (sent == (int)size) result = ESP_OK;
        else {
            lock_state(0); s_transport_poisoned = true; unlock_state();
        }
    }
    cJSON_free(encoded);
    return result;
}
static esp_err_t send_json(cJSON *message)
{
    if (s_send_lock == NULL || xSemaphoreTake(s_send_lock,
        pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != pdTRUE) {
        cJSON_Delete(message);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = send_json_unlocked(message);
    xSemaphoreGive(s_send_lock);
    return result;
}
static cJSON *control(const char *type)
{
    cJSON *message = cJSON_CreateObject();
    if (message != NULL) {
        cJSON_AddStringToObject(message, "type", type);
        cJSON_AddNumberToObject(message, "version", WIRELESS_MICROPHONE_PROTOCOL_VERSION);
    }
    return message;
}
static cJSON *session_control(uint32_t attempt, const char *type)
{
    char session[37];
    uint64_t generation;
    if (!lock_state(0)) return NULL;
    const bool valid = s_attempt == attempt && s_generation != 0;
    memcpy(session, s_session_id, sizeof(session));
    generation = s_generation;
    unlock_state();
    if (!valid) return NULL;
    cJSON *message = control(type);
    if (message != NULL) {
        cJSON_AddStringToObject(message, "sessionID", session);
        cJSON_AddNumberToObject(message, "generation", (double)generation);
    }
    return message;
}
static esp_err_t send_hello(void)
{
    cJSON *message = control("hello");
    if (message != NULL) {
        cJSON_AddStringToObject(message, "deviceID", CONFIG_CODEX_ATTENTION_WIRELESS_DEVICE_ID);
        cJSON_AddStringToObject(message, "credential", CONFIG_CODEX_ATTENTION_WIRELESS_CREDENTIAL);
    }
    return send_json(message);
}
static bool s_hello_pending, s_commit_pending;

static void reject_connection(const char *reason)
{
    fail_session(current_attempt(), reason);
    if (lock_state(0)) {
        if (!s_recovering) s_recovery_requested = true;
        unlock_state();
    }
}

static void cancel_preparation(uint32_t attempt)
{
    if (!lock_state(0)) return;
    if (attempt != s_attempt || !active_locked()) { unlock_state(); return; }
    const uint32_t token = s_capture_token;
    s_phase = SESSION_CANCELED;
    s_recovery_requested = true;
    unlock_state();
    voice_audio_stop_capture(token);
    xEventGroupSetBits(s_events, WIRELESS_EVENT_CANCELED | WIRELESS_EVENT_WORK);
}

static void handle_control_message(const char *data, size_t length)
{
    if (data == NULL || length == 0 || length > WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH) {
        reject_connection("invalid control message"); return;
    }
    cJSON *message = cJSON_ParseWithLength(data, length);
    if (message == NULL) { reject_connection("malformed control JSON"); return; }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(message, "type");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(message, "version");
    uint32_t parsed_version = 0;
    if (!cJSON_IsString(type) || !json_u32(version, &parsed_version)
        || parsed_version != WIRELESS_MICROPHONE_PROTOCOL_VERSION) {
        cJSON_Delete(message); reject_connection("invalid control version/type"); return;
    }
    const char *name = type->valuestring;
    const uint32_t attempt = current_attempt();
    if (!strcmp(name, "capabilities")) {
        uint32_t max_frame = 0;
        if (!valid_format(cJSON_GetObjectItemCaseSensitive(message, "format"))
            || !json_u32(cJSON_GetObjectItemCaseSensitive(message, "maxFrameBytes"), &max_frame)
            || max_frame != WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH) {
            cJSON_Delete(message); reject_connection("unsupported capabilities"); return;
        }
        if (lock_state(0)) {
            if (s_connected && !s_recovering && !s_recovery_requested) {
                s_authenticated = true;
                xEventGroupSetBits(s_events, WIRELESS_EVENT_AUTHENTICATED);
            }
            unlock_state();
        }
        cJSON_Delete(message); return;
    }
    if (!lock_state(0)) { cJSON_Delete(message); return; }
    if (!s_connected || !s_authenticated || s_recovering || s_recovery_requested) {
        unlock_state(); cJSON_Delete(message); return;
    }
    if (!strcmp(name, "prepared")) {
        const cJSON *request = cJSON_GetObjectItemCaseSensitive(message, "requestID");
        // Old responses do not mutate identity or fail a successor.
        if (!cJSON_IsString(request) || strcmp(request->valuestring, s_request_id)
            || s_phase != SESSION_PREPARING) {
            unlock_state(); cJSON_Delete(message); return;
        }
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(message, "sessionID");
        uint64_t generation = 0;
        uint8_t uuid[16];
        if (!cJSON_IsString(session) || !parse_uuid(session->valuestring, uuid)
            || !json_u64(cJSON_GetObjectItemCaseSensitive(message, "generation"), &generation)
            || generation == 0
            || !valid_format(cJSON_GetObjectItemCaseSensitive(message, "format"))) {
            unlock_state(); cJSON_Delete(message); fail_session(attempt, "invalid prepared session"); return;
        }
        strlcpy(s_session_id, session->valuestring, sizeof(s_session_id));
        memcpy(s_session_uuid, uuid, sizeof(uuid));
        s_generation = generation;
        s_phase = SESSION_WAIT_ARMED;
        s_commit_pending = true;
        xEventGroupSetBits(s_events, WIRELESS_EVENT_WORK);
        unlock_state();
        diagnose(attempt, "prepared");
    } else if (!strcmp(name, "armed")) {
        atomic_fetch_add(&s_armed_received, 1);
        if (!matches_locked(message) || s_phase != SESSION_WAIT_ARMED) {
            atomic_fetch_add(&s_armed_rejected, 1);
            unlock_state();
            diagnose(attempt, "armed-rejected");
        } else {
            const uint32_t token = s_capture_token;
            s_phase = SESSION_OPENING_CAPTURE;
            unlock_state();
            const esp_err_t result = voice_audio_start_capture(VOICE_AUDIO_SOURCE_WIFI, token);
            if (result != ESP_OK) {
                diagnose(attempt, result == ESP_ERR_TIMEOUT ? "gate-lock-timeout" : "gate-unavailable-or-revoked");
                if (voice_audio_capture_token() != token) cancel_preparation(attempt);
                else fail_session(attempt, result == ESP_ERR_TIMEOUT ? "capture gate lock timeout" : "capture not ready");
            } else {
                lock_state(0);
                const bool current = s_attempt == attempt && s_phase == SESSION_OPENING_CAPTURE
                    && voice_audio_capture_token() == ((token + 2U) | 1U);
                if (current) {
                    s_phase = SESSION_STREAMING;
                    s_armed_ms = s_last_pcm_ms = s_last_ack_ms = now_ms();
                }
                unlock_state();
                if (!current) {
                    voice_audio_stop_capture(token);
                    cancel_preparation(attempt);
                }
                diagnose(attempt, current ? "gate-open" : "gate-revoked");
            }
        }
    } else if (!strcmp(name, "listening") || !strcmp(name, "ack")) {
        uint32_t sequence = 0;
        if (!matches_locked(message)
            || (s_phase != SESSION_STREAMING && s_phase != SESSION_STOPPING)) {
            unlock_state(); cJSON_Delete(message); return;
        }
        if (!json_u32(cJSON_GetObjectItemCaseSensitive(message, "sequence"), &sequence)
            || (sequence >= s_next_sequence && !(s_send_in_flight && sequence == s_next_sequence))
            || (!strcmp(name, "listening") && sequence != 0)) {
            unlock_state(); cJSON_Delete(message); fail_session(attempt, "invalid audio acknowledgement"); return;
        }
        if (!s_have_ack || sequence > s_last_ack_sequence) {
            s_have_ack = true;
            s_last_ack_sequence = sequence;
            s_last_ack_ms = now_ms();
        }
        if (!strcmp(name, "listening") && s_phase == SESSION_STREAMING)
            xEventGroupSetBits(s_events, WIRELESS_EVENT_LISTENING);
        unlock_state();
    } else if (!strcmp(name, "stopped")) {
        uint32_t final_sequence = 0;
        const bool valid = matches_locked(message) && s_phase == SESSION_STOPPING
            && json_u32(cJSON_GetObjectItemCaseSensitive(message, "finalSequence"), &final_sequence)
            && final_sequence == s_next_sequence && !s_send_in_flight;
        if (valid) xEventGroupSetBits(s_events, WIRELESS_EVENT_STOPPED);
        unlock_state();
        if (!valid) diagnose(attempt, "stopped-rejected");
    } else {
        unlock_state();
        fail_session(attempt, !strcmp(name, "error") ? "receiver rejected wireless session" : "unexpected control type");
    }
    cJSON_Delete(message);
}

// One bounded message assembler. WS fragmentation and buffer-sized DATA chunks
// are independent boundaries; PING/PONG/CLOSE never enter this accumulator.
static char s_rx[WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH + 1];
static size_t s_rx_used, s_rx_offset, s_rx_frame_length;
static bool s_rx_message, s_rx_frame;
static int s_rx_opcode;
static bool s_rx_fin;
static void reset_rx(void)
{
    s_rx_used = s_rx_offset = s_rx_frame_length = 0;
    s_rx_message = s_rx_frame = false;
}
static bool receive_control_chunk(const esp_websocket_event_data_t *event)
{
    if (event->data_len < 0 || event->payload_len < 0 || event->payload_offset < 0
        || (event->data_len && event->data_ptr == NULL)) return false;
    if (!s_rx_frame) {
        if (event->payload_offset != 0) return false;
        if (event->op_code == 1) {
            if (s_rx_message) return false;
            s_rx_message = true;
            s_rx_used = 0;
        } else if (event->op_code != 0 || !s_rx_message) return false;
        s_rx_frame = true;
        s_rx_offset = 0;
        s_rx_frame_length = (size_t)event->payload_len;
        s_rx_opcode = event->op_code;
        s_rx_fin = event->fin;
    }
    if (event->op_code != s_rx_opcode || event->fin != s_rx_fin
        || (size_t)event->payload_offset != s_rx_offset
        || (size_t)event->payload_len != s_rx_frame_length
        || (size_t)event->data_len > s_rx_frame_length - s_rx_offset
        || (size_t)event->data_len > WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH - s_rx_used
        || s_rx_frame_length - s_rx_offset > WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH - s_rx_used)
        return false;
    if (event->data_len) memcpy(s_rx + s_rx_used, event->data_ptr, (size_t)event->data_len);
    s_rx_used += (size_t)event->data_len;
    s_rx_offset += (size_t)event->data_len;
    if (s_rx_offset == s_rx_frame_length) {
        s_rx_frame = false;
        if (event->fin) {
            s_rx[s_rx_used] = 0;
            handle_control_message(s_rx, s_rx_used);
            reset_rx();
        }
    }
    return true;
}
static void websocket_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args; (void)base;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        reset_rx();
        lock_state(0);
        if (!s_recovering && !s_recovery_requested) {
            s_connected = true;
            s_authenticated = false;
            s_transport_poisoned = false;
            s_hello_pending = true;
            memset(&s_last_connection_error, 0, sizeof(s_last_connection_error));
            xEventGroupSetBits(s_events, WIRELESS_EVENT_CONNECTED | WIRELESS_EVENT_WORK);
            xEventGroupClearBits(s_events, WIRELESS_EVENT_AUTHENTICATED);
        }
        unlock_state();
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED || id == WEBSOCKET_EVENT_ERROR) {
        lock_state(0);
        if (id == WEBSOCKET_EVENT_ERROR && data != NULL)
            s_last_connection_error = ((const esp_websocket_event_data_t *)data)->error_handle;
        s_connected = s_authenticated = false;
        s_hello_pending = s_commit_pending = false;
        const uint32_t attempt = s_attempt;
        unlock_state();
        fail_session(attempt, "wireless connection lost");
        xEventGroupClearBits(s_events, WIRELESS_EVENT_CONNECTED | WIRELESS_EVENT_AUTHENTICATED);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        const esp_websocket_event_data_t *event = data;
        if (event == NULL) { reject_connection("missing WebSocket event"); return; }
        if (event->op_code == 8 || event->op_code == 9 || event->op_code == 10) return;
        if (!receive_control_chunk(event)) {
            reset_rx();
            reject_connection("invalid control frame boundaries");
        }
    }
}
static void stream_task(void *argument)
{
    (void)argument;
    uint8_t pcm[WIRELESS_MICROPHONE_PCM_BYTES_PER_FRAME];
    uint8_t packet[WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH];
    while (true) {
        atomic_fetch_add(&s_stream_ticks, 1);
        lock_state(0);
        s_last_stream_ms = now_ms();
        const uint32_t attempt = s_attempt, token = s_capture_token;
        const bool streaming = s_phase == SESSION_STREAMING && s_connected && s_authenticated;
        unlock_state();
        if (!streaming || !voice_audio_is_listening()) {
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        // The capture wait owns no transport or lifecycle mutex.
        size_t bytes = 0;
        atomic_fetch_add(&s_read_attempts, 1);
        const esp_err_t read_result = voice_audio_wireless_read_frame(pcm, sizeof(pcm), &bytes);
        if (voice_audio_capture_token() != ((token + 2U) | 1U)) continue;
        if (voice_audio_take_overflow()) { fail_session(attempt, "capture ring overflow"); continue; }
        if (read_result == ESP_ERR_TIMEOUT) continue; // Supervisor owns liveness.
        if (read_result != ESP_OK || bytes != sizeof(pcm)) {
            fail_session(attempt, "capture frame unavailable"); continue;
        }
        if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != pdTRUE) {
            atomic_fetch_add(&s_send_lock_timeouts, 1);
            fail_session(attempt, "audio send lock timeout"); continue;
        }
        uint8_t uuid[16];
        lock_state(0);
        const bool current = s_attempt == attempt && s_phase == SESSION_STREAMING
            && s_connected && s_authenticated
            && voice_audio_capture_token() == ((token + 2U) | 1U);
        const uint32_t sequence = s_next_sequence;
        memcpy(uuid, s_session_uuid, sizeof(uuid));
        if (current) s_last_pcm_ms = now_ms();
        unlock_state();
        if (!current) { xSemaphoreGive(s_send_lock); continue; }
        size_t packet_length = 0;
        if (!wireless_microphone_encode_audio_frame(packet, sizeof(packet), uuid, sequence,
            (uint64_t)sequence * WIRELESS_MICROPHONE_SAMPLES_PER_FRAME, pcm, bytes, &packet_length)) {
            xSemaphoreGive(s_send_lock);
            fail_session(attempt, "audio framing failed"); continue;
        }
        lock_state(0);
        // Reserve at the submission boundary. Stop accounts for this reserved
        // frame by waiting on the send mutex before choosing finalSequence.
        const bool submit = s_attempt == attempt && s_phase == SESSION_STREAMING
            && voice_audio_capture_token() == ((token + 2U) | 1U);
        if (submit) s_send_in_flight = true;
        unlock_state();
        if (!submit) { xSemaphoreGive(s_send_lock); continue; }
        const int64_t started = now_ms();
        atomic_fetch_add(&s_send_attempts, 1);
        errno = 0;
        const int sent = esp_websocket_client_send_bin(s_client, (const char *)packet,
            (int)packet_length, pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS));
        const int saved_errno = errno;
        lock_state(0);
        if (sent != (int)packet_length) s_transport_poisoned = true;
        if (s_attempt == attempt) {
            if (sent == (int)packet_length) {
                ++s_next_sequence;
                atomic_fetch_add(&s_send_completed, 1);
            }
            s_send_in_flight = false;
        }
        unlock_state();
        xSemaphoreGive(s_send_lock);
        if (sent != (int)packet_length) {
            char reason[64];
            snprintf(reason, sizeof(reason), "audio send r=%d errno=%d t=%" PRId64 "ms",
                sent, saved_errno, now_ms() - started);
            // ERROR may already have terminated this attempt synchronously.
            // Preserve specific transport evidence without reopening the take.
            lock_state(0);
            if (s_attempt == attempt && s_phase == SESSION_FAILED
                && !strcmp(s_last_session_failure, "wireless connection lost")) {
                strlcpy(s_last_session_failure, reason, sizeof(s_last_session_failure));
            }
            unlock_state();
            fail_session(attempt, reason);
        }
    }
}

static void check_liveness(void)
{
    lock_state(0);
    const uint32_t attempt = s_attempt;
    const session_phase_t phase = s_phase;
    const int64_t now = now_ms();
    const char *reason = NULL;
    if ((phase == SESSION_PREPARING || phase == SESSION_WAIT_ARMED)
        && now - s_started_ms > WIRELESS_START_TIMEOUT_MS) {
        reason = phase == SESSION_PREPARING ? "prepared response timeout" : "armed delivery timeout";
    } else if (phase == SESSION_STREAMING) {
        if (s_next_sequence == 0 && now - s_armed_ms > WIRELESS_ACK_TIMEOUT_MS)
            reason = "first PCM progress timeout";
        else if (now - s_last_pcm_ms > WIRELESS_ACK_TIMEOUT_MS)
            reason = "capture liveness timeout";
        else if (s_next_sequence >= 5 && now - s_last_ack_ms > WIRELESS_ACK_TIMEOUT_MS)
            reason = "audio acknowledgement timeout";
    }
    unlock_state();
    if (reason != NULL) {
        // Recheck after snapshot; a physical Stop has its own deadline.
        lock_state(0);
        const bool current = s_attempt == attempt && s_phase == phase;
        unlock_state();
        if (current) fail_session_in_phase(attempt, (int)phase, reason);
    }
}

static void service_connection_once(void)
{
    check_liveness();
    lock_state(0);
    const bool recover = s_recovery_requested && !s_recovering;
    const uint32_t attempt = s_attempt, token = s_capture_token;
    const session_phase_t phase = s_phase;
    if (recover) {
        s_recovering = true;
        s_connected = s_authenticated = false;
        s_hello_pending = s_commit_pending = false;
    }
    const bool hello = !recover && s_hello_pending;
    const bool commit = !recover && s_commit_pending && s_phase == SESSION_WAIT_ARMED;
    if (hello) s_hello_pending = false;
    if (commit) s_commit_pending = false;
    unlock_state();
    if (recover) {
        if (attempt != 0) voice_audio_stop_capture(token);
        // Restore idle policy before any potentially blocked socket operation.
        const esp_err_t power_result = recording_power(false, attempt);
        if (power_result != ESP_OK || xSemaphoreTake(s_send_lock,
                pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != pdTRUE) {
            lock_state(0); s_recovering = false; unlock_state();
            return;
        }
        // Best effort only. Failure still retires the connection, including
        // preparation timeouts before the server has issued a session UUID.
        lock_state(0);
        const bool can_cancel = !s_transport_poisoned;
        unlock_state();
        // Never append a control packet after an uncertain/partial frame write.
        cJSON *cancel = can_cancel ? session_control(attempt, "cancel") : NULL;
        if (cancel != NULL) {
            cJSON_AddStringToObject(cancel, "errorCode",
                phase == SESSION_CANCELED ? "user_canceled" : "sender_failed");
            (void)send_json_unlocked(cancel);
        }
        const esp_err_t stopped = esp_websocket_client_stop(s_client);
        xSemaphoreGive(s_send_lock);
        if (stopped != ESP_OK && stopped != ESP_ERR_INVALID_STATE) {
            lock_state(0); s_recovering = false; unlock_state();
            return;
        }
        reset_rx();
        lock_state(0);
        s_recovery_requested = s_recovering = false;
        s_connected = s_authenticated = false;
        unlock_state();
        xEventGroupClearBits(s_events, WIRELESS_EVENT_CONNECTED | WIRELESS_EVENT_AUTHENTICATED);
        if (esp_websocket_client_start(s_client) != ESP_OK) {
            lock_state(0); s_recovery_requested = true; unlock_state();
        }
        return;
    }
    if (hello && send_hello() != ESP_OK) reject_connection("hello send failed");
    if (commit) {
        if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != pdTRUE) {
            fail_session(attempt, "commit send lock timeout"); return;
        }
        lock_state(0);
        const bool current = s_attempt == attempt && s_phase == SESSION_WAIT_ARMED
            && s_connected && s_authenticated && !s_recovery_requested;
        unlock_state();
        if (current && send_json_unlocked(session_control(attempt, "commit")) != ESP_OK)
            fail_session(attempt, "commit send failed");
        xSemaphoreGive(s_send_lock);
    }
}

static void connection_task(void *argument)
{
    (void)argument;
    while (!wifi_manager_wait_connected(1000) || time(NULL) < 1704067200) {
        ESP_LOGI(TAG, "Waiting for Wi-Fi and clock synchronization before TLS");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (esp_websocket_client_start(s_client) != ESP_OK) {
        lock_state(0); s_recovery_requested = true; unlock_state();
    }
    while (true) {
        service_connection_once();
        lock_state(0);
        const bool active = active_locked();
        unlock_state();
        // Idle connections do not need a 50 Hz supervisory wakeup. Control
        // events wake this task immediately; active progress is checked at 20 ms.
        (void)xEventGroupWaitBits(s_events, WIRELESS_EVENT_WORK, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(active ? 20 : 1000));
    }
}
esp_err_t wireless_microphone_init(void)
{
#if !CONFIG_CODEX_ATTENTION_WIRELESS_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (CONFIG_CODEX_ATTENTION_WIRELESS_URL[0] == '\0'
        || CONFIG_CODEX_ATTENTION_WIRELESS_SERVER_NAME[0] == '\0'
        || CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM[0] == '\0'
        || strncmp(CONFIG_CODEX_ATTENTION_WIRELESS_URL, "wss://", 6) != 0
        || strlen(CONFIG_CODEX_ATTENTION_WIRELESS_CREDENTIAL) != 64) {
        ESP_LOGI(TAG, "Wi-Fi microphone not provisioned; USB remains available");
        return ESP_ERR_INVALID_STATE;
    }
    const size_t certificate_capacity = strlen(CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM) + 1;
    s_certificate_pem = malloc(certificate_capacity);
    if (s_certificate_pem == NULL) return ESP_ERR_NO_MEM;
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    const bool valid_certificate = wireless_certificate_decode(CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM,
        s_certificate_pem, certificate_capacity)
        && mbedtls_x509_crt_parse(&certificate, (const unsigned char *)s_certificate_pem,
            strlen(s_certificate_pem) + 1) == 0;
    mbedtls_x509_crt_free(&certificate);
    if (!valid_certificate) {
        free(s_certificate_pem);
        s_certificate_pem = NULL;
        ESP_LOGW(TAG, "Invalid pairing certificate; provision the board again");
        return ESP_ERR_INVALID_ARG;
    }
    s_events = xEventGroupCreate();
    s_state_lock = xSemaphoreCreateMutex();
    s_send_lock = xSemaphoreCreateMutex();
    s_power_lock = xSemaphoreCreateMutex();
    if (s_events == NULL || s_state_lock == NULL || s_send_lock == NULL || s_power_lock == NULL) return ESP_ERR_NO_MEM;
    const esp_websocket_client_config_t config = {
        .uri = CONFIG_CODEX_ATTENTION_WIRELESS_URL,
        .cert_common_name = CONFIG_CODEX_ATTENTION_WIRELESS_SERVER_NAME,
        .cert_pem = s_certificate_pem,
        .subprotocol = "codex-microphone.v1",
        .buffer_size = WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH,
        .task_stack = 6144,
        // The Mac closes canceled sessions. A clean CLOSE otherwise stops the
        // client task permanently, unlike a transport failure.
        .enable_close_reconnect = true,
    };
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL) return ESP_FAIL;
    esp_err_t result = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
        websocket_event_handler, NULL);
    if (result != ESP_OK) return result;
    if (xTaskCreate(stream_task, "wireless_pcm", WIRELESS_STREAM_STACK, NULL,
                    WIRELESS_STREAM_PRIORITY, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(connection_task, "wireless_connect", 4096, NULL,
                    4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_enabled = true;
    ESP_LOGI(TAG, "Wi-Fi microphone client started with bounded PCM transport");
    return ESP_OK;
#endif
}

bool wireless_microphone_is_enabled(void) { return s_enabled; }

void wireless_microphone_get_status(char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return;
    if (!s_enabled) { snprintf(output, capacity, "Wi-Fi mic: not initialized"); return; }
    if (!voice_audio_is_ready()) { snprintf(output, capacity, "Wi-Fi mic: capture not initialized"); return; }
    if (!lock_state(0)) { snprintf(output, capacity, "Wi-Fi mic: state unavailable"); return; }
    const bool connected = s_connected, authenticated = s_authenticated;
    const esp_websocket_error_codes_t error = s_last_connection_error;
    char failure[sizeof(s_last_session_failure)];
    strlcpy(failure, s_last_session_failure, sizeof(failure));
    const uint32_t sequence = s_failure_sequence;
    unlock_state();
    if (failure[0]) snprintf(output, capacity, "Wi-Fi mic %s: %s (frame %lu)",
        authenticated ? "paired; last error" : "error", failure, (unsigned long)sequence);
    else if (authenticated) snprintf(output, capacity, "Wi-Fi mic: connected and paired");
    else if (connected) snprintf(output, capacity, "Wi-Fi mic: authenticating");
    else if (!wifi_manager_is_connected()) snprintf(output, capacity, "Wi-Fi mic: waiting for Wi-Fi");
    else if (time(NULL) < 1704067200) snprintf(output, capacity, "Wi-Fi mic: waiting for clock sync");
    else if (error.esp_tls_stack_err || error.esp_tls_last_esp_err || error.esp_ws_handshake_status_code)
        snprintf(output, capacity, "Wi-Fi mic: TLS %d / ESP %d / HTTP %d", error.esp_tls_stack_err,
            (int)error.esp_tls_last_esp_err, error.esp_ws_handshake_status_code);
    else snprintf(output, capacity, "Wi-Fi mic: connecting");
}

bool wireless_microphone_is_ready(void)
{
    if (!voice_audio_is_ready() || !lock_state(0)) return false;
    const bool ready = s_connected && s_authenticated && !s_recovery_requested && !s_recovering;
    unlock_state();
    return ready;
}
bool wireless_microphone_has_active_session(void)
{
    if (s_state_lock == NULL) return false;
    if (!lock_state(0)) return true; // Unknown must not authorize USB takeover.
    const bool active = active_locked() || s_start_waiter || s_stop_waiter || s_send_in_flight
        || s_recovery_requested || s_recovering;
    unlock_state();
    return active;
}
bool wireless_microphone_has_failed(void)
{
    if (!lock_state(0)) return false;
    const bool failed = s_phase == SESSION_FAILED;
    unlock_state();
    return failed;
}
bool wireless_microphone_take_failure_for(uint32_t *capture_token)
{
    if (!lock_state(0)) return false;
    const bool pending = s_failure_notice;
    if (pending) {
        if (capture_token != NULL) *capture_token = s_failure_token;
        s_failure_notice = false;
    }
    unlock_state();
    return pending;
}
bool wireless_microphone_take_failure(void)
{
    return wireless_microphone_take_failure_for(NULL);
}

esp_err_t wireless_microphone_start_session_authorized(const char *thread_id, const char *request_id,
                                                       uint32_t capture_token)
{
    if (!s_enabled || thread_id == NULL || request_id == NULL || !thread_id[0] || !request_id[0]
        || strlen(request_id) >= sizeof(s_request_id) || !wireless_microphone_is_ready()
        || (capture_token & 1U) || voice_audio_capture_token() != capture_token)
        return ESP_ERR_INVALID_STATE;
    lock_state(0);
    if (active_locked() || s_start_waiter || s_stop_waiter || s_send_in_flight
        || s_recovery_requested || s_recovering || !s_connected || !s_authenticated) {
        unlock_state(); return ESP_ERR_INVALID_STATE;
    }
    if (++s_attempt == 0) ++s_attempt;
    const uint32_t attempt = s_attempt;
    s_capture_token = capture_token;
    s_start_waiter = true;
    s_phase = SESSION_PREPARING;
    s_started_ms = now_ms();
    s_have_ack = false;
    s_next_sequence = 0;
    s_generation = 0;
    s_session_id[0] = 0;
    s_last_session_failure[0] = 0;
    s_failure_notice = false;
    strlcpy(s_request_id, request_id, sizeof(s_request_id));
    atomic_store(&s_armed_received, 0);
    atomic_store(&s_armed_rejected, 0);
    atomic_store(&s_read_attempts, 0);
    atomic_store(&s_send_attempts, 0);
    atomic_store(&s_send_completed, 0);
    atomic_store(&s_send_lock_timeouts, 0);
    xEventGroupClearBits(s_events, WIRELESS_EVENT_LISTENING | WIRELESS_EVENT_FAILED
        | WIRELESS_EVENT_STOPPED | WIRELESS_EVENT_CANCELED);
    unlock_state();
    xEventGroupSetBits(s_events, WIRELESS_EVENT_WORK);
    esp_err_t result = recording_power(true, attempt);
    if (result != ESP_OK) fail_session(attempt, "recording power policy failed");
    else if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != pdTRUE)
        fail_session(attempt, "start send lock timeout");
    else {
        lock_state(0);
        const bool current = s_attempt == attempt && s_phase == SESSION_PREPARING
            && voice_audio_capture_token() == capture_token;
        unlock_state();
        if (current) {
            cJSON *start = control("start");
            if (start != NULL) {
                cJSON_AddStringToObject(start, "threadID", thread_id);
                cJSON_AddStringToObject(start, "requestID", request_id);
                cJSON_AddStringToObject(start, "transport", "wifi");
            }
            if (send_json_unlocked(start) != ESP_OK) fail_session(attempt, "start send failed");
        } else cancel_preparation(attempt);
        xSemaphoreGive(s_send_lock);
    }
    (void)xEventGroupWaitBits(s_events,
        WIRELESS_EVENT_LISTENING | WIRELESS_EVENT_FAILED | WIRELESS_EVENT_CANCELED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIRELESS_START_TIMEOUT_MS));
    lock_state(0);
    EventBits_t latest = xEventGroupGetBits(s_events);
    const session_phase_t deadline_phase = s_phase;
    const bool timed_out = s_attempt == attempt && active_locked() && s_phase != SESSION_STOPPING
        && !(latest & (WIRELESS_EVENT_LISTENING | WIRELESS_EVENT_FAILED | WIRELESS_EVENT_CANCELED));
    unlock_state();
    if (timed_out) fail_session_in_phase(attempt, (int)deadline_phase, "wireless start timed out");
    lock_state(0);
    latest = xEventGroupGetBits(s_events);
    const bool success = s_attempt == attempt && s_phase == SESSION_STREAMING
        && voice_audio_capture_token() == ((capture_token + 2U) | 1U)
        && (latest & WIRELESS_EVENT_LISTENING)
        && !(latest & (WIRELESS_EVENT_FAILED | WIRELESS_EVENT_CANCELED));
    const bool cancel_remaining = !success && s_attempt == attempt
        && active_locked() && s_phase != SESSION_STOPPING;
    result = success ? ESP_OK : (s_phase == SESSION_FAILED
        ? (timed_out ? ESP_ERR_TIMEOUT : ESP_FAIL) : ESP_ERR_INVALID_STATE);
    unlock_state();
    // A losing start waiter must never leave an unowned open gate, even when
    // a first-frame acknowledgement arrives exactly at the deadline.
    if (cancel_remaining) cancel_preparation(attempt);
    lock_state(0);
    s_start_waiter = false;
    unlock_state();
    return result;
}
esp_err_t wireless_microphone_start_session(const char *thread_id, const char *request_id)
{
    return wireless_microphone_start_session_authorized(thread_id, request_id, voice_audio_capture_token());
}

esp_err_t wireless_microphone_stop_session(void)
{
    if (!s_enabled || !lock_state(0)) return ESP_ERR_INVALID_STATE;
    if (!active_locked() || s_stop_waiter) { unlock_state(); return ESP_ERR_INVALID_STATE; }
    const uint32_t attempt = s_attempt, token = s_capture_token;
    if (s_phase != SESSION_STREAMING) {
        unlock_state(); cancel_preparation(attempt); return ESP_OK;
    }
    s_phase = SESSION_STOPPING;
    s_stop_waiter = true;
    const bool start_pending = s_start_waiter;
    unlock_state();
    voice_audio_stop_capture(token);
    if (start_pending) xEventGroupSetBits(s_events, WIRELESS_EVENT_CANCELED | WIRELESS_EVENT_WORK);
    esp_err_t result = ESP_FAIL;
    if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) == pdTRUE) {
        lock_state(0);
        const bool current = s_attempt == attempt && s_phase == SESSION_STOPPING;
        const uint32_t final_sequence = s_next_sequence;
        unlock_state();
        if (current) {
            cJSON *stop = session_control(attempt, "stop");
            if (stop != NULL) cJSON_AddNumberToObject(stop, "finalSequence", final_sequence);
            result = send_json_unlocked(stop);
        }
        xSemaphoreGive(s_send_lock);
    }
    if (result == ESP_OK) {
        const EventBits_t bits = xEventGroupWaitBits(s_events, WIRELESS_EVENT_STOPPED | WIRELESS_EVENT_FAILED,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(WIRELESS_STOP_DRAIN_MS));
        result = (bits & WIRELESS_EVENT_STOPPED) && !(bits & WIRELESS_EVENT_FAILED) ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    if (result == ESP_OK) {
        result = recording_power(false, attempt);
        if (result == ESP_OK) {
            lock_state(0);
            if (s_attempt == attempt && s_phase == SESSION_STOPPING) s_phase = SESSION_IDLE;
            else result = ESP_FAIL;
            unlock_state();
        }
    }
    if (result != ESP_OK) fail_session(attempt, "stop acknowledgement or cleanup failed");
    lock_state(0); s_stop_waiter = false; unlock_state();
    return result;
}
