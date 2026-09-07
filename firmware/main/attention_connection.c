#include "attention_connection.h"
#include "attention_pairing.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"

#define RESPONSE_LIMIT (64U * 1024U)
#define AUTH_RESPONSE_LIMIT 2048U
#define DISCOVERY_LIMIT 8U

typedef struct {
    char *data;
    size_t length, capacity, limit;
    bool failed, tls_failure;
} response_t;

static SemaphoreHandle_t s_lock;
static bool s_mdns_started;
static char s_host[254], s_device[33], s_token[65], s_session[65];
static uint16_t s_port;
static uint32_t s_sequence, s_generation, s_discovery_cursor;
static int64_t s_discovery_until, s_token_until;

static void clear_token(void)
{
    attention_pairing_zero(s_token, sizeof(s_token));
    attention_pairing_zero(s_session, sizeof(s_session));
    s_token_until = 0;
    s_sequence = 0;
}

static void dispose(response_t *r)
{
    if (r->data != NULL) attention_pairing_zero(r->data, r->capacity);
    free(r->data);
    r->data = NULL;
    r->length = r->capacity = 0;
}

static esp_err_t receive(esp_http_client_event_t *event)
{
    response_t *r = event->user_data;
    if (event->event_id == HTTP_EVENT_DISCONNECTED && event->data != NULL) {
        int tls_error = 0, flags = 0;
        (void)esp_tls_get_and_clear_last_error(event->data, &tls_error, &flags);
        if (tls_error != 0 || flags != 0) r->tls_failure = true;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    const size_t count = (size_t)event->data_len;
    if (count > r->limit || r->length > r->limit - count) { r->failed = true; return ESP_ERR_INVALID_SIZE; }
    const size_t required = r->length + count + 1;
    if (required > r->capacity) {
        size_t size = r->capacity == 0 ? 2048 : r->capacity * 2;
        if (size < required) size = required;
        if (size > r->limit + 1) size = r->limit + 1;
        char *next = realloc(r->data, size);
        if (next == NULL) { r->failed = true; return ESP_ERR_NO_MEM; }
        r->data = next;
        r->capacity = size;
    }
    memcpy(r->data + r->length, event->data, count);
    r->length += count;
    r->data[r->length] = 0;
    return ESP_OK;
}

static bool matching_service(const attention_pairing_record_t *record, const mdns_result_t *entry)
{
    if (entry->port == 0 || entry->txt_count > 16 || entry->txt == NULL || entry->txt_value_len == NULL) return false;
    const char *keys[16], *values[16];
    for (size_t i = 0; i < entry->txt_count; ++i) {
        keys[i] = entry->txt[i].key;
        values[i] = entry->txt[i].value;
        // Reject embedded NULs instead of treating the prefix as the identity.
        if (values[i] == NULL || strnlen(values[i], (size_t)entry->txt_value_len[i] + 1) != entry->txt_value_len[i]) return false;
    }
    return attention_discovery_txt_matches(record->bridge_id, keys, values, entry->txt_count);
}

static esp_err_t discover(const attention_pairing_record_t *record)
{
    const int64_t now = esp_timer_get_time();
    if (s_host[0] && now < s_discovery_until) return ESP_OK;
    s_host[0] = 0;
    if (!s_mdns_started) s_mdns_started = mdns_init() == ESP_OK;
    mdns_result_t *results = NULL;
    if (s_mdns_started) (void)mdns_query_ptr("_codex-attention", "_tcp", 1500, DISCOVERY_LIMIT, &results);
    size_t candidates = 0;
    for (const mdns_result_t *entry = results; entry != NULL; entry = entry->next) {
        if (!matching_service(record, entry)) continue;
        for (const mdns_ip_addr_t *address = entry->addr; address != NULL; address = address->next) {
            if (address->addr.type == ESP_IPADDR_TYPE_V4) { ++candidates; break; }
        }
    }
    const size_t selected = candidates ? s_discovery_cursor++ % candidates : 0;
    size_t index = 0;
    for (const mdns_result_t *entry = results; entry != NULL && !s_host[0]; entry = entry->next) {
        if (!matching_service(record, entry)) continue;
        for (const mdns_ip_addr_t *address = entry->addr; address != NULL; address = address->next) {
            if (address->addr.type != ESP_IPADDR_TYPE_V4) continue;
            if (index++ == selected) {
                snprintf(s_host, sizeof(s_host), IPSTR, IP2STR(&address->addr.u_addr.ip4));
                s_port = entry->port;
            }
            break;
        }
    }
    mdns_query_results_free(results);
    if (!s_host[0] && record->fallback_host[0]) {
        strlcpy(s_host, record->fallback_host, sizeof(s_host));
        s_port = (uint16_t)record->port;
    }
    if (!s_host[0]) return ATTENTION_ERR_DISCOVERY;
    s_discovery_until = now + 30LL * 1000000;
    return ESP_OK;
}

// Only this function creates HTTP clients. No caller can opt out of paired
// trust, change transport, follow redirects, or provide its own destination.
static esp_err_t exchange(const attention_pairing_record_t *record, const char *path,
    const char *body, bool authenticated, size_t limit, response_t *response, int *status)
{
    *status = 0;
    response->limit = limit;
    char server_name[64];
    snprintf(server_name, sizeof(server_name), "attention-%s.local", record->bridge_id);
    const esp_http_client_config_t config = {
        .host = s_host, .port = s_port, .path = path,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = record->certificate, .common_name = server_name,
        .disable_auto_redirect = true, .max_authorization_retries = -1,
        .event_handler = receive, .user_data = response,
        .timeout_ms = 5000, .buffer_size = 2048, .buffer_size_tx = 1024,
        .user_agent = "codex-esp32-display/0.3.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    esp_err_t result = esp_http_client_set_method(client, body ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Accept", "application/json");
    if (body != NULL && result == ESP_OK) result = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (body != NULL && result == ESP_OK) result = esp_http_client_set_post_field(client, body, (int)strlen(body));
    char authorization[80] = { 0 }, generation[11], sequence[11];
    if (authenticated && result == ESP_OK) {
        snprintf(authorization, sizeof(authorization), "Bearer %s", s_token);
        snprintf(generation, sizeof(generation), "%lu", (unsigned long)record->generation);
        snprintf(sequence, sizeof(sequence), "%lu", (unsigned long)++s_sequence);
        result = esp_http_client_set_header(client, "Authorization", authorization);
        if (result == ESP_OK) result = esp_http_client_set_header(client, "X-Codex-Device", record->device_id);
        if (result == ESP_OK) result = esp_http_client_set_header(client, "X-Codex-Generation", generation);
        if (result == ESP_OK) result = esp_http_client_set_header(client, "X-Codex-Session", s_session);
        if (result == ESP_OK) result = esp_http_client_set_header(client, "X-Codex-Sequence", sequence);
    }
    if (result == ESP_OK) result = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    attention_pairing_zero(authorization, sizeof(authorization));
    if (result != ESP_OK) {
        s_discovery_until = 0;
        return response->tls_failure ? ATTENTION_ERR_TLS : ATTENTION_ERR_UNAVAILABLE;
    }
    if (response->failed || response->length == 0 || response->data == NULL
        || memchr(response->data, 0, response->length) != NULL) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

static const char *json_string(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool json_number(const cJSON *object, const char *key, uint32_t expected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) && value->valuedouble == expected;
}

static esp_err_t authenticate(const attention_pairing_record_t *record)
{
    clear_token();
    char body[512], mac[65] = { 0 };
    snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"generation\":%lu}", record->device_id, (unsigned long)record->generation);
    response_t response = { 0 };
    int status;
    esp_err_t result = exchange(record, "/api/v1/device/challenge", body, false, AUTH_RESPONSE_LIMIT, &response, &status);
    if (result == ESP_OK && status != 200) result = (status == 401 || status == 403) ? ATTENTION_ERR_UNAUTHORIZED : ATTENTION_ERR_UNAVAILABLE;
    cJSON *challenge = result == ESP_OK ? cJSON_Parse(response.data) : NULL;
    if (result == ESP_OK) {
        const char *bridge = json_string(challenge, "bridgeId"), *session = json_string(challenge, "sessionId"),
            *nonce = json_string(challenge, "challenge");
        if (!json_number(challenge, "version", 1) || bridge == NULL || strcmp(bridge, record->bridge_id)
            || !attention_pairing_hex(session, 64) || !attention_pairing_hex(nonce, 64)) result = ESP_ERR_INVALID_RESPONSE;
        else {
            result = attention_pairing_proof(record, "auth", session, nonce, mac);
            if (result == ESP_OK) {
                strlcpy(s_session, session, sizeof(s_session));
                snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"generation\":%lu,\"sessionId\":\"%s\",\"challenge\":\"%s\",\"proof\":\"%s\"}",
                    record->device_id, (unsigned long)record->generation, session, nonce, mac);
            }
        }
    }
    cJSON_Delete(challenge);
    dispose(&response);
    response = (response_t){ 0 };
    const int64_t issued_at = esp_timer_get_time();
    if (result == ESP_OK) result = exchange(record, "/api/v1/device/token", body, false, AUTH_RESPONSE_LIMIT, &response, &status);
    if (result == ESP_OK && status != 200) result = (status == 401 || status == 403) ? ATTENTION_ERR_UNAUTHORIZED : ATTENTION_ERR_UNAVAILABLE;
    cJSON *token = result == ESP_OK ? cJSON_Parse(response.data) : NULL;
    if (result == ESP_OK) {
        const char *bridge = json_string(token, "bridgeId"), *session = json_string(token, "sessionId"), *access = json_string(token, "accessToken");
        const cJSON *expires = cJSON_GetObjectItemCaseSensitive(token, "expiresIn");
        if (!json_number(token, "version", 1) || !json_number(token, "generation", record->generation)
            || bridge == NULL || strcmp(bridge, record->bridge_id) || session == NULL || strcmp(session, s_session)
            || !attention_pairing_hex(access, 64) || !cJSON_IsNumber(expires)
            || expires->valuedouble < 30 || expires->valuedouble > 300) result = ESP_ERR_INVALID_RESPONSE;
        else {
            strlcpy(s_token, access, sizeof(s_token));
            s_token_until = issued_at + ((int64_t)expires->valuedouble - 15) * 1000000;
        }
    }
    cJSON_Delete(token);
    dispose(&response);
    attention_pairing_zero(mac, sizeof(mac));
    attention_pairing_zero(body, sizeof(body));
    if (result != ESP_OK) clear_token();
    return result;
}

static esp_err_t finish_reset(const attention_pairing_record_t *record)
{
    clear_token();
    char mac[65] = { 0 }, body[320];
    esp_err_t result = attention_pairing_proof(record, "revoke", record->reset_nonce, NULL, mac);
    if (result != ESP_OK) return result;
    snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"generation\":%lu,\"nonce\":\"%s\",\"proof\":\"%s\"}",
        record->device_id, (unsigned long)record->generation, record->reset_nonce, mac);
    response_t response = { 0 };
    int status;
    result = exchange(record, "/api/v1/device/revoke", body, false, AUTH_RESPONSE_LIMIT, &response, &status);
    cJSON *ack = result == ESP_OK && status == 200 ? cJSON_Parse(response.data) : NULL;
    const char *device = json_string(ack, "deviceId");
    if (json_number(ack, "version", 1) && device != NULL && !strcmp(device, record->device_id))
        (void)attention_pairing_queue_reset_ack(json_string(ack, "nonce"), json_string(ack, "proof"));
    cJSON_Delete(ack);
    dispose(&response);
    attention_pairing_zero(mac, sizeof(mac));
    attention_pairing_zero(body, sizeof(body));
    return ATTENTION_ERR_RESET_PENDING;
}

esp_err_t attention_connection_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t attention_connection_request(const char *path, const char *body, char **json)
{
    if (json == NULL || path == NULL || strncmp(path, "/api/v1/", 8)
        || strchr(path, '?') || strstr(path, "://") || (body != NULL && strlen(body) > 4096)) return ESP_ERR_INVALID_ARG;
    *json = NULL;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    attention_pairing_record_t *record = malloc(sizeof(*record));
    if (record == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t result = attention_pairing_copy(record);
    if (result != ESP_OK) result = result == ESP_ERR_NOT_FOUND ? ATTENTION_ERR_UNPAIRED : ATTENTION_ERR_STORAGE;
    if (result == ESP_OK && (!wifi_manager_is_connected())) { s_discovery_until = 0; clear_token(); result = ATTENTION_ERR_WIFI; }
    if (result == ESP_OK) {
        if (strcmp(s_device, record->device_id) || s_generation != record->generation) {
            clear_token();
            s_discovery_until = 0;
            strlcpy(s_device, record->device_id, sizeof(s_device));
            s_generation = record->generation;
        }
        result = discover(record);
    }
    if (result == ESP_OK && record->state == ATTENTION_PAIRING_RESETTING) result = finish_reset(record);
    if (result == ESP_OK && (!s_token[0] || esp_timer_get_time() >= s_token_until || s_sequence == UINT32_MAX)) result = authenticate(record);
    response_t response = { 0 };
    int status = 0;
    if (result == ESP_OK) result = exchange(record, path, body, true, RESPONSE_LIMIT, &response, &status);
    // A 401 is a PRE-execution rejection. Never retry an ambiguous network
    // failure or a POST that might already have performed its side effect.
    if (result == ESP_OK && status == 401) {
        dispose(&response);
        response = (response_t){ 0 };
        result = authenticate(record);
        if (result == ESP_OK) result = exchange(record, path, body, true, RESPONSE_LIMIT, &response, &status);
    }
    if (result == ESP_OK && status != 200) {
        if (status == 401 || status == 403) { clear_token(); result = ATTENTION_ERR_UNAUTHORIZED; }
        else if (status == 404) result = ESP_ERR_NOT_FOUND;
        else if (status == 409) result = ESP_ERR_INVALID_STATE;
        else result = ATTENTION_ERR_UNAVAILABLE;
    }
    if (result == ESP_OK) { *json = response.data; response.data = NULL; }
    dispose(&response);
    attention_pairing_zero(record, sizeof(*record));
    free(record);
    xSemaphoreGive(s_lock);
    return result;
}

// Serialize the durable reset boundary with all existing attention requests.
// Once this returns success, no successor can use the previous access token.
esp_err_t attention_connection_begin_reset(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    clear_token();
    const esp_err_t result = attention_pairing_begin_reset();
    xSemaphoreGive(s_lock);
    return result;
}
