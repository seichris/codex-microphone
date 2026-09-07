#include "wireless_microphone.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "voice_audio.h"
#include "wireless_microphone_protocol.h"

#define WIRELESS_EVENT_CONNECTED BIT0
#define WIRELESS_EVENT_AUTHENTICATED BIT1
#define WIRELESS_EVENT_LISTENING BIT2
#define WIRELESS_EVENT_FAILED BIT3
#define WIRELESS_EVENT_STOPPED BIT4
#define WIRELESS_ACK_TIMEOUT_MS 500U
#define WIRELESS_START_TIMEOUT_MS 5000U
#define WIRELESS_STOP_DRAIN_MS 250U
#define WIRELESS_SEND_TIMEOUT_MS 100U
#define WIRELESS_STREAM_STACK 8192U
#define WIRELESS_STREAM_PRIORITY 7U

static const char *TAG = "wireless_microphone";
static esp_websocket_client_handle_t s_client;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_state_lock;
static SemaphoreHandle_t s_send_lock;
static bool s_enabled;
static bool s_connected;
static bool s_authenticated;
static bool s_streaming;
static bool s_armed;
static bool s_stopping;
static bool s_send_in_flight;
static bool s_have_ack;
static uint32_t s_last_ack_sequence;
static bool s_pending_start;
static bool s_cancel_requested;
static bool s_failed_session;
static char s_session_id[37];
static char s_request_id[97];
static char s_canceled_request_id[97];
static uint8_t s_session_uuid[16];
static uint64_t s_generation;
static uint32_t s_next_sequence;
static int64_t s_last_ack_ms;

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

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

static bool message_session_matches(const cJSON *message, bool require_armed)
{
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(message, "sessionID");
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(message, "generation");
    uint64_t parsed_generation = 0;
    if (!cJSON_IsString(session) || !json_u64(generation, &parsed_generation)
        || parsed_generation != s_generation || strcmp(session->valuestring, s_session_id) != 0) {
        return false;
    }
    return !require_armed || s_armed || s_stopping;
}

static bool lock_state(TickType_t ticks)
{
    return s_state_lock != NULL && xSemaphoreTake(s_state_lock, ticks) == pdTRUE;
}

static void unlock_state(void)
{
    if (s_state_lock != NULL) xSemaphoreGive(s_state_lock);
}

static esp_err_t send_json_unlocked(cJSON *message)
{
    if (message == NULL) return ESP_ERR_INVALID_STATE;
    if (s_client == NULL) {
        cJSON_Delete(message);
        return ESP_ERR_INVALID_STATE;
    }
    char *encoded = cJSON_PrintUnformatted(message);
    if (encoded == NULL) {
        cJSON_Delete(message);
        return ESP_ERR_NO_MEM;
    }
    const size_t length = strlen(encoded);
    esp_err_t result = ESP_FAIL;
    if (length <= WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH && esp_websocket_client_is_connected(s_client)) {
        const int sent = esp_websocket_client_send_text(s_client, encoded, (int)length,
                                                        pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS));
        result = sent == (int)length ? ESP_OK : ESP_FAIL;
    }
    cJSON_free(encoded);
    cJSON_Delete(message);
    return result;
}

static esp_err_t send_json(cJSON *message)
{
    if (s_send_lock == NULL || xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != pdTRUE) {
        cJSON_Delete(message);
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = send_json_unlocked(message);
    xSemaphoreGive(s_send_lock);
    return result;
}

static esp_err_t send_hello(void)
{
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(message, "type", "hello");
    cJSON_AddNumberToObject(message, "version", WIRELESS_MICROPHONE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(message, "deviceID", CONFIG_CODEX_ATTENTION_WIRELESS_DEVICE_ID);
    cJSON_AddStringToObject(message, "credential", CONFIG_CODEX_ATTENTION_WIRELESS_CREDENTIAL);
    return send_json(message);
}

static esp_err_t send_start(const char *thread_id, const char *request_id)
{
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(message, "type", "start");
    cJSON_AddNumberToObject(message, "version", WIRELESS_MICROPHONE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(message, "requestID", request_id);
    cJSON_AddStringToObject(message, "threadID", thread_id);
    cJSON_AddStringToObject(message, "transport", "wifi");
    return send_json(message);
}

static esp_err_t send_commit(void)
{
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(message, "type", "commit");
    cJSON_AddNumberToObject(message, "version", WIRELESS_MICROPHONE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(message, "sessionID", s_session_id);
    cJSON_AddNumberToObject(message, "generation", (double)s_generation);
    return send_json(message);
}

static esp_err_t send_cancel(void)
{
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(message, "type", "cancel");
    cJSON_AddNumberToObject(message, "version", WIRELESS_MICROPHONE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(message, "sessionID", s_session_id);
    cJSON_AddNumberToObject(message, "generation", (double)s_generation);
    cJSON_AddStringToObject(message, "errorCode", "user_canceled");
    return send_json(message);
}

static void mark_pending_start_canceled(void)
{
    if (lock_state(pdMS_TO_TICKS(20))) {
        if (s_pending_start && !s_armed && !s_streaming) {
            strlcpy(s_canceled_request_id, s_request_id, sizeof(s_canceled_request_id));
            s_cancel_requested = true;
        }
        unlock_state();
    }
}

static void stop_local_stream(void)
{
    if (lock_state(pdMS_TO_TICKS(20))) {
        s_streaming = false;
        s_armed = false;
        s_stopping = false;
        s_send_in_flight = false;
        s_pending_start = false;
        unlock_state();
    }
    voice_audio_set_listening(false);
    voice_audio_set_source(VOICE_AUDIO_SOURCE_USB);
}

static void fail_session(const char *reason)
{
    ESP_LOGW(TAG, "Wireless session failed: %s", reason == NULL ? "unknown" : reason);
    bool should_cancel_remote = false;
    if (lock_state(pdMS_TO_TICKS(20))) {
        if (s_streaming || s_armed || s_pending_start || s_stopping) s_failed_session = true;
        should_cancel_remote = s_session_id[0] != '\0' && s_generation != 0
            && s_connected && s_authenticated;
        unlock_state();
    }
    stop_local_stream();
    // A local failure must also terminate the receiver-side session. Otherwise
    // a subsequent physical gesture would reuse a server still stuck in the
    // previous listening phase. This runs after local gating is closed, and
    // never retries through the failure path if the cancel send itself fails.
    if (should_cancel_remote && send_cancel() != ESP_OK) {
        ESP_LOGW(TAG, "Could not cancel failed wireless session at receiver");
    }
    if (s_events != NULL) xEventGroupSetBits(s_events, WIRELESS_EVENT_FAILED);
}

static void handle_control_message(const char *data, size_t length)
{
    if (data == NULL || length == 0 || length > WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH) { fail_session("invalid control message"); return; }
    char *copy = calloc(1, length + 1);
    if (copy == NULL) { fail_session("control allocation failed"); return; }
    memcpy(copy, data, length);
    cJSON *message = cJSON_ParseWithLength(copy, length);
    free(copy);
    if (message == NULL) { fail_session("malformed control JSON"); return; }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(message, "type");
    const char *type_text = cJSON_IsString(type) ? type->valuestring : NULL;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(message, "version");
    uint32_t parsed_version = 0;
    if (type_text == NULL || !json_u32(version, &parsed_version)
        || parsed_version != WIRELESS_MICROPHONE_PROTOCOL_VERSION) {
        cJSON_Delete(message); fail_session("control version or type missing"); return;
    }

    if (strcmp(type_text, "capabilities") == 0) {
        const cJSON *format = cJSON_GetObjectItemCaseSensitive(message, "format");
        const cJSON *max_frame = cJSON_GetObjectItemCaseSensitive(message, "maxFrameBytes");
        uint32_t max_frame_bytes = 0;
        if (!valid_format(format) || !json_u32(max_frame, &max_frame_bytes)
            || max_frame_bytes != WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH) {
            cJSON_Delete(message); fail_session("unsupported capabilities"); return;
        }
        if (lock_state(pdMS_TO_TICKS(20))) { s_authenticated = true; unlock_state(); }
        xEventGroupSetBits(s_events, WIRELESS_EVENT_AUTHENTICATED);
    } else if (strcmp(type_text, "prepared") == 0) {
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(message, "sessionID");
        const cJSON *generation = cJSON_GetObjectItemCaseSensitive(message, "generation");
        const cJSON *request = cJSON_GetObjectItemCaseSensitive(message, "requestID");
        uint64_t parsed_generation = 0;
        uint8_t parsed_session_uuid[16];
        bool pending_start = false;
        bool cancel_requested = false;
        if (lock_state(pdMS_TO_TICKS(20))) {
            pending_start = s_pending_start;
            cancel_requested = s_cancel_requested;
            unlock_state();
        }
        if (!cJSON_IsString(session) || !cJSON_IsString(request)
            || !json_u64(generation, &parsed_generation) || parsed_generation == 0
            || !parse_uuid(session->valuestring, parsed_session_uuid)) {
            cJSON_Delete(message); fail_session("invalid prepared session"); return;
        }
        bool request_matches_current = false;
        bool request_matches_canceled = false;
        if (lock_state(pdMS_TO_TICKS(20))) {
            request_matches_current = strcmp(request->valuestring, s_request_id) == 0;
            request_matches_canceled = s_canceled_request_id[0] != '\0'
                && strcmp(request->valuestring, s_canceled_request_id) == 0;
            unlock_state();
        }
        if ((!pending_start && !cancel_requested)
            || (!request_matches_current && !request_matches_canceled)) {
            // A delayed response for an earlier request must not tear down a
            // newer connection/session. It is safe to ignore it because the
            // receiver will close the old prepared session when its cancel
            // request arrives (or when its own preparation deadline expires).
            cJSON_Delete(message);
            return;
        }
        strlcpy(s_session_id, session->valuestring, sizeof(s_session_id));
        memcpy(s_session_uuid, parsed_session_uuid, sizeof(s_session_uuid));
        s_generation = parsed_generation;
        if (cancel_requested && request_matches_canceled) {
            if (send_cancel() != ESP_OK) fail_session("cancel send failed");
            if (lock_state(pdMS_TO_TICKS(20))) {
                s_cancel_requested = false;
                s_canceled_request_id[0] = '\0';
                unlock_state();
            }
            cJSON_Delete(message);
            return;
        }
        if (send_commit() != ESP_OK) { cJSON_Delete(message); fail_session("commit send failed"); return; }
    } else if (strcmp(type_text, "armed") == 0) {
        if (!message_session_matches(message, false)) {
            cJSON_Delete(message); fail_session("stale armed session"); return;
        }
        if (!lock_state(pdMS_TO_TICKS(20))) {
            cJSON_Delete(message); fail_session("arm state unavailable"); return;
        }
        // Retransmitted or late armed replies must never restart PCM or reset
        // sequence accounting after a local stop/cancel has won.
        if (s_armed || s_stopping || !s_pending_start || s_cancel_requested || s_failed_session) {
            unlock_state();
            cJSON_Delete(message);
            return;
        }
        s_armed = true;
        s_streaming = true;
        s_next_sequence = 0;
        s_have_ack = false;
        s_last_ack_ms = now_ms();
        voice_audio_set_source(VOICE_AUDIO_SOURCE_WIFI);
        voice_audio_set_listening(true);
        unlock_state();
    } else if (strcmp(type_text, "listening") == 0) {
        if (!message_session_matches(message, true)) {
            cJSON_Delete(message); fail_session("stale listening session"); return;
        }
        if (lock_state(pdMS_TO_TICKS(20))) {
            if (!s_stopping) xEventGroupSetBits(s_events, WIRELESS_EVENT_LISTENING);
            unlock_state();
        }
    } else if (strcmp(type_text, "ack") == 0) {
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(message, "sequence");
        uint32_t acknowledged_sequence = 0;
        if (!message_session_matches(message, true) || !json_u32(sequence, &acknowledged_sequence)) {
            cJSON_Delete(message); fail_session("stale or invalid acknowledgement"); return;
        }
        if (lock_state(pdMS_TO_TICKS(20))) {
            if (acknowledged_sequence >= s_next_sequence
                && !(s_send_in_flight && acknowledged_sequence == s_next_sequence)) {
                unlock_state();
                cJSON_Delete(message); fail_session("acknowledgement is ahead of audio"); return;
            }
            // Only forward progress refreshes liveness. Repeated old ACKs
            // must not keep a stalled receiver alive indefinitely.
            if (!s_have_ack || acknowledged_sequence > s_last_ack_sequence) {
                s_have_ack = true;
                s_last_ack_sequence = acknowledged_sequence;
                s_last_ack_ms = now_ms();
            }
            unlock_state();
        }
    } else if (strcmp(type_text, "stopped") == 0) {
        const cJSON *final_sequence = cJSON_GetObjectItemCaseSensitive(message, "finalSequence");
        uint32_t parsed_final_sequence = 0;
        if (!message_session_matches(message, false) || !json_u32(final_sequence, &parsed_final_sequence)
            || parsed_final_sequence != s_next_sequence) {
            cJSON_Delete(message); fail_session("invalid stopped session"); return;
        }
        stop_local_stream();
        xEventGroupSetBits(s_events, WIRELESS_EVENT_STOPPED);
    } else if (strcmp(type_text, "error") == 0) {
        fail_session("receiver rejected wireless session");
    }
    cJSON_Delete(message);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        if (lock_state(pdMS_TO_TICKS(20))) {
            s_connected = true;
            s_authenticated = false;
            unlock_state();
        }
        xEventGroupSetBits(s_events, WIRELESS_EVENT_CONNECTED);
        // Keep a failure latched until the next physical start attempt. A
        // fast WebSocket reconnect must not make the board look as if the
        // previous stream recovered underneath the voice state machine.
        xEventGroupClearBits(s_events, WIRELESS_EVENT_AUTHENTICATED);
        if (send_hello() != ESP_OK) fail_session("hello send failed");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED
               || event_id == WEBSOCKET_EVENT_ERROR) {
        if (lock_state(pdMS_TO_TICKS(20))) {
            if (s_streaming || s_armed || s_pending_start || s_stopping) s_failed_session = true;
            s_connected = false;
            s_authenticated = false;
            s_cancel_requested = false;
            s_canceled_request_id[0] = '\0';
            unlock_state();
        }
        stop_local_stream();
        xEventGroupClearBits(s_events, WIRELESS_EVENT_CONNECTED | WIRELESS_EVENT_AUTHENTICATED);
        xEventGroupSetBits(s_events, WIRELESS_EVENT_FAILED);
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        const esp_websocket_event_data_t *event = (const esp_websocket_event_data_t *)event_data;
        if (event == NULL) { fail_session("missing WebSocket event"); return; }
        // ESP-IDF also dispatches payload-free ping/pong/close DATA events.
        // They are transport controls, not malformed microphone JSON.
        if (event->op_code == 0x9 || event->op_code == 0xA || event->op_code == 0x8) return;
        if (event->op_code != 0x1 || event->data_ptr == NULL
            || event->data_len <= 0 || event->payload_offset != 0 || !event->fin
            || event->data_len != event->payload_len) {
            fail_session("incomplete or unsupported control frame");
        } else {
            handle_control_message(event->data_ptr, (size_t)event->data_len);
        }
    }
}

static void stream_task(void *argument)
{
    (void)argument;
    uint8_t pcm[WIRELESS_MICROPHONE_PCM_BYTES_PER_FRAME];
    uint8_t packet[WIRELESS_MICROPHONE_MAX_AUDIO_MESSAGE_LENGTH];
    while (true) {
        if (s_send_lock == NULL || xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        bool streaming;
        uint32_t sequence;
        if (!lock_state(pdMS_TO_TICKS(20))) {
            xSemaphoreGive(s_send_lock);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        streaming = s_streaming && s_armed && !s_stopping && s_connected && s_authenticated
            && voice_audio_is_listening();
        sequence = s_next_sequence;
        unlock_state();
        if (!streaming) {
            xSemaphoreGive(s_send_lock);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t bytes_read = 0;
        const esp_err_t read_result = voice_audio_wireless_read_frame(
            pcm, sizeof(pcm), &bytes_read
        );
        // The physical button closes the audio gate before it waits for this
        // send mutex. A canceled read is therefore a normal stop, not a broken
        // capture device. Never submit a just-read block after gate closure.
        if (!voice_audio_is_listening()) {
            xSemaphoreGive(s_send_lock);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (voice_audio_take_overflow()) {
            xSemaphoreGive(s_send_lock);
            fail_session("capture ring overflow");
            continue;
        }
        if (read_result == ESP_ERR_TIMEOUT) {
            // An empty ring is not a codec failure. The first clean frame can
            // span two capture periods when a pre-arm codec read is discarded.
            // Reuse the existing ACK liveness budget instead of fabricating
            // silence, enlarging the ring, or failing on a single short wait.
            if (!lock_state(pdMS_TO_TICKS(20))) {
                xSemaphoreGive(s_send_lock);
                fail_session("capture state unavailable");
                continue;
            }
            const bool capture_timed_out = now_ms() - s_last_ack_ms > WIRELESS_ACK_TIMEOUT_MS;
            unlock_state();
            xSemaphoreGive(s_send_lock);
            if (capture_timed_out) fail_session("capture liveness timeout");
            continue;
        }
        if (read_result != ESP_OK || bytes_read != sizeof(pcm)) {
            xSemaphoreGive(s_send_lock);
            fail_session("capture frame unavailable");
            continue;
        }
        size_t packet_length = 0;
        if (!wireless_microphone_encode_audio_frame(packet, sizeof(packet), s_session_uuid,
                sequence, (uint64_t)sequence * WIRELESS_MICROPHONE_SAMPLES_PER_FRAME,
                pcm, sizeof(pcm), &packet_length)) {
            xSemaphoreGive(s_send_lock);
            fail_session("audio framing failed");
            continue;
        }
        if (!lock_state(pdMS_TO_TICKS(20))) {
            xSemaphoreGive(s_send_lock);
            fail_session("send state unavailable");
            continue;
        }
        s_send_in_flight = true;
        unlock_state();
        if (esp_websocket_client_send_bin(s_client, (const char *)packet, (int)packet_length,
                                          pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != (int)packet_length) {
            xSemaphoreGive(s_send_lock);
            fail_session("audio send failed");
            continue;
        }
        if (lock_state(pdMS_TO_TICKS(20))) {
            s_next_sequence++;
            s_send_in_flight = false;
            const bool ack_timed_out = s_next_sequence >= 5 && now_ms() - s_last_ack_ms > WIRELESS_ACK_TIMEOUT_MS;
            unlock_state();
            xSemaphoreGive(s_send_lock);
            if (ack_timed_out) fail_session("audio acknowledgement timeout");
        } else {
            xSemaphoreGive(s_send_lock);
            fail_session("sent frame state unavailable");
        }
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
    s_events = xEventGroupCreate();
    s_state_lock = xSemaphoreCreateMutex();
    s_send_lock = xSemaphoreCreateMutex();
    if (s_events == NULL || s_state_lock == NULL || s_send_lock == NULL) return ESP_ERR_NO_MEM;
    const esp_websocket_client_config_t config = {
        .uri = CONFIG_CODEX_ATTENTION_WIRELESS_URL,
        .cert_common_name = CONFIG_CODEX_ATTENTION_WIRELESS_SERVER_NAME,
        .cert_pem = CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM,
        .subprotocol = "codex-microphone.v1",
        .buffer_size = WIRELESS_MICROPHONE_MAX_CONTROL_MESSAGE_LENGTH,
        .task_stack = 6144,
    };
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL) return ESP_FAIL;
    esp_err_t result = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
        websocket_event_handler, NULL);
    if (result != ESP_OK) return result;
    result = esp_websocket_client_start(s_client);
    if (result != ESP_OK) return result;
    s_enabled = true;
    if (xTaskCreate(stream_task, "wireless_pcm", WIRELESS_STREAM_STACK, NULL,
                    WIRELESS_STREAM_PRIORITY, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Wi-Fi microphone client started with bounded PCM transport");
    return ESP_OK;
#endif
}

bool wireless_microphone_is_enabled(void) { return s_enabled; }

bool wireless_microphone_is_ready(void)
{
    if (!lock_state(0)) return false;
    const bool ready = s_connected && s_authenticated;
    unlock_state();
    return ready;
}

bool wireless_microphone_has_active_session(void)
{
    if (!lock_state(0)) return false;
    const bool active = s_streaming || s_armed || s_pending_start || s_stopping;
    unlock_state();
    return active;
}

bool wireless_microphone_has_failed(void)
{
    if (s_events == NULL || !lock_state(0)) return false;
    const bool failed = s_failed_session
        && (xEventGroupGetBits(s_events) & WIRELESS_EVENT_FAILED) != 0;
    unlock_state();
    return failed;
}

esp_err_t wireless_microphone_start_session(const char *thread_id, const char *request_id)
{
    if (!s_enabled || thread_id == NULL || request_id == NULL || !wireless_microphone_is_ready()) return ESP_ERR_INVALID_STATE;
    if (!lock_state(pdMS_TO_TICKS(20))) return ESP_ERR_TIMEOUT;
    if (s_streaming || s_armed || s_pending_start || s_stopping || s_cancel_requested) {
        unlock_state(); return ESP_ERR_INVALID_STATE;
    }
    s_pending_start = true;
    s_stopping = false;
    s_send_in_flight = false;
    s_have_ack = false;
    s_cancel_requested = false;
    s_failed_session = false;
    s_canceled_request_id[0] = '\0';
    s_session_id[0] = '\0';
    s_generation = 0;
    strlcpy(s_request_id, request_id, sizeof(s_request_id));
    s_next_sequence = 0;
    xEventGroupClearBits(s_events, WIRELESS_EVENT_LISTENING | WIRELESS_EVENT_FAILED | WIRELESS_EVENT_STOPPED);
    unlock_state();
    if (send_start(thread_id, request_id) != ESP_OK) {
        mark_pending_start_canceled();
        fail_session("start send failed");
        return ESP_FAIL;
    }
    const EventBits_t bits = xEventGroupWaitBits(s_events, WIRELESS_EVENT_LISTENING | WIRELESS_EVENT_FAILED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIRELESS_START_TIMEOUT_MS));
    if ((bits & WIRELESS_EVENT_FAILED) != 0 || (bits & WIRELESS_EVENT_LISTENING) == 0) {
        mark_pending_start_canceled();
        fail_session("wireless start timed out");
        return ESP_ERR_TIMEOUT;
    }
    if (lock_state(pdMS_TO_TICKS(20))) { s_pending_start = false; unlock_state(); }
    return ESP_OK;
}

esp_err_t wireless_microphone_stop_session(void)
{
    if (!s_enabled || !wireless_microphone_has_active_session()) return ESP_ERR_INVALID_STATE;
    if (!lock_state(pdMS_TO_TICKS(20))) {
        voice_audio_set_listening(false);
        fail_session("stop state unavailable");
        return ESP_ERR_TIMEOUT;
    }
    if (s_stopping) { unlock_state(); return ESP_ERR_INVALID_STATE; }
    if (s_pending_start && !s_armed && !s_streaming) {
        strlcpy(s_canceled_request_id, s_request_id, sizeof(s_canceled_request_id));
        s_pending_start = false;
        s_cancel_requested = true;
        unlock_state();
        voice_audio_set_listening(false);
        // Cancel now if prepared already arrived; otherwise the prepared
        // handler will send cancel using the server-issued session identity.
        if (s_session_id[0] != '\0' && send_cancel() != ESP_OK) {
            fail_session("cancel send failed");
        }
        xEventGroupSetBits(s_events, WIRELESS_EVENT_FAILED);
        return ESP_OK;
    }
    s_stopping = true;
    unlock_state();
    voice_audio_set_listening(false);
    if (s_send_lock == NULL || xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(WIRELESS_SEND_TIMEOUT_MS)) != pdTRUE) {
        fail_session("stop send lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    uint32_t final_sequence;
    if (!lock_state(pdMS_TO_TICKS(20))) {
        xSemaphoreGive(s_send_lock);
        fail_session("stop state unavailable");
        return ESP_ERR_TIMEOUT;
    }
    s_streaming = false;
    s_armed = false;
    s_cancel_requested = false;
    final_sequence = s_next_sequence;
    unlock_state();
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) {
        xSemaphoreGive(s_send_lock);
        fail_session("stop allocation failed");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(message, "type", "stop");
    cJSON_AddNumberToObject(message, "version", WIRELESS_MICROPHONE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(message, "sessionID", s_session_id);
    cJSON_AddNumberToObject(message, "generation", (double)s_generation);
    cJSON_AddNumberToObject(message, "finalSequence", final_sequence);
    const esp_err_t stop_result = send_json_unlocked(message);
    xSemaphoreGive(s_send_lock);
    if (stop_result != ESP_OK) { fail_session("stop send failed"); return ESP_FAIL; }
    const EventBits_t stopped_bits = xEventGroupWaitBits(
        s_events, WIRELESS_EVENT_STOPPED | WIRELESS_EVENT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIRELESS_STOP_DRAIN_MS)
    );
    if ((stopped_bits & WIRELESS_EVENT_STOPPED) != 0
        && (stopped_bits & WIRELESS_EVENT_FAILED) == 0) {
        stop_local_stream();
        return ESP_OK;
    }
    fail_session("stop acknowledgement timeout or failure");
    return ESP_ERR_TIMEOUT;
}
