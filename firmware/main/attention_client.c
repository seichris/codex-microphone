#include "attention_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "sdkconfig.h"

#define DETAIL_URL_MAX 768
#define COMMAND_BODY_MAX 512

typedef struct { char *data; } response_buffer_t;

static esp_err_t perform_get(const char *path, response_buffer_t *response)
{
    return attention_connection_request(path, NULL, &response->data);
}

static esp_err_t perform_post(const char *path, const char *body, response_buffer_t *response)
{
    return attention_connection_request(path, body, &response->data);
}

static void copy_json_string(char *destination, size_t size, const cJSON *object, const char *key)
{
    if (size == 0) return;
    destination[0] = '\0';
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        strlcpy(destination, value->valuestring, size);
    }
}

static void strip_citation_marker_block(char *text, const char *opening, const char *closing)
{
    if (text == NULL || opening == NULL || closing == NULL) return;

    while (true) {
        char *start = strstr(text, opening);
        if (start == NULL) return;

        char *end = strstr(start, closing);
        if (end != NULL) end += strlen(closing);
        else end = text + strlen(text);

        char *prefix_end = start;
        while (prefix_end > text && isspace((unsigned char)prefix_end[-1])) prefix_end--;

        char *suffix_start = end;
        while (*suffix_start != '\0' && isspace((unsigned char)*suffix_start)) suffix_start++;
        memmove(prefix_end, suffix_start, strlen(suffix_start) + 1);
    }
}

static void strip_output_citations(char *text)
{
    strip_citation_marker_block(text, "<oai-mem-citation", "</oai-mem-citation>");
    strip_citation_marker_block(text, "&lt;oai-mem-citation", "&lt;/oai-mem-citation&gt;");
}

static bool json_bool(const cJSON *object, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, key));
}

static uint32_t json_u32(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble <= 0) return 0;
    if (value->valuedouble >= UINT32_MAX) return UINT32_MAX;
    return (uint32_t)value->valuedouble;
}

static attention_status_t parse_status(const cJSON *object)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "status");
    if (!cJSON_IsString(value) || value->valuestring == NULL) return ATTENTION_STATUS_IDLE;
    if (strcmp(value->valuestring, "running") == 0) return ATTENTION_STATUS_RUNNING;
    if (strcmp(value->valuestring, "waiting_input") == 0) return ATTENTION_STATUS_WAITING_INPUT;
    if (strcmp(value->valuestring, "waiting_approval") == 0) return ATTENTION_STATUS_WAITING_APPROVAL;
    if (strcmp(value->valuestring, "error") == 0) return ATTENTION_STATUS_ERROR;
    return ATTENTION_STATUS_IDLE;
}

static attention_focus_confidence_t parse_focus_confidence(const cJSON *object)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "focusConfidence");
    if (!cJSON_IsString(value) || value->valuestring == NULL) return ATTENTION_FOCUS_UNAVAILABLE;
    if (strcmp(value->valuestring, "confirmed") == 0) return ATTENTION_FOCUS_CONFIRMED;
    if (strcmp(value->valuestring, "inferred") == 0) return ATTENTION_FOCUS_INFERRED;
    return ATTENTION_FOCUS_UNAVAILABLE;
}

static attention_voice_state_t parse_voice_state(const cJSON *object)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "voiceState");
    if (!cJSON_IsString(value) || value->valuestring == NULL) return ATTENTION_VOICE_UNKNOWN;
    if (strcmp(value->valuestring, "ready") == 0) return ATTENTION_VOICE_READY;
    if (strcmp(value->valuestring, "focusing") == 0) return ATTENTION_VOICE_FOCUSING;
    if (strcmp(value->valuestring, "starting") == 0) return ATTENTION_VOICE_STARTING;
    if (strcmp(value->valuestring, "listening") == 0) return ATTENTION_VOICE_LISTENING;
    if (strcmp(value->valuestring, "muted") == 0) return ATTENTION_VOICE_MUTED;
    if (strcmp(value->valuestring, "error") == 0) return ATTENTION_VOICE_ERROR;
    return ATTENTION_VOICE_UNKNOWN;
}

static void parse_capabilities(const cJSON *object, attention_capabilities_t *capabilities)
{
    memset(capabilities, 0, sizeof(*capabilities));
    if (!cJSON_IsObject(object)) return;
    capabilities->desktop_focus = json_bool(object, "desktopFocus");
    capabilities->desktop_voice_hotkey = json_bool(object, "desktopVoiceHotkey");
    capabilities->power_button_long_press = json_bool(object, "powerButtonLongPress");
    capabilities->wireless_microphone = json_bool(object, "wirelessMicrophone");
}

static esp_err_t parse_snapshot(const char *json, attention_snapshot_t *snapshot)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsNumber(version) || version->valueint != 1 || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->total_count = json_u32(root, "totalCount");
    snapshot->truncated = json_bool(root, "truncated");

    const cJSON *diagnostics = cJSON_GetObjectItemCaseSensitive(root, "diagnostics");
    if (cJSON_IsObject(diagnostics)) {
        snapshot->desktop_state_available = json_bool(diagnostics, "desktopStateAvailable");
        copy_json_string(snapshot->source_error, sizeof(snapshot->source_error), diagnostics, "sourceError");
    }

    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    parse_capabilities(capabilities, &snapshot->capabilities);

    const cJSON *control_available = cJSON_GetObjectItemCaseSensitive(root, "desktopControlAvailable");
    if (cJSON_IsBool(control_available)) {
        snapshot->desktop_control_availability = cJSON_IsTrue(control_available)
            ? ATTENTION_DESKTOP_CONTROL_AVAILABLE
            : ATTENTION_DESKTOP_CONTROL_UNAVAILABLE;
    }

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "currentThread");
    if (cJSON_IsObject(current)) {
        attention_current_thread_t *output = &snapshot->current_thread;
        copy_json_string(output->id, sizeof(output->id), current, "id");
        copy_json_string(output->title, sizeof(output->title), current, "title");
        copy_json_string(output->project, sizeof(output->project), current, "project");
        output->available = output->id[0] != '\0';
        output->status = parse_status(current);
        output->focus_confidence = parse_focus_confidence(current);
        output->voice_state = parse_voice_state(current);
        if (output->available && output->title[0] == '\0') {
            strlcpy(output->title, "Current Codex thread", sizeof(output->title));
        }
        if (output->available && output->project[0] == '\0') {
            strlcpy(output->project, "Codex", sizeof(output->project));
        }
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (snapshot->count >= CONFIG_CODEX_ATTENTION_MAX_ITEMS) {
            snapshot->truncated = true;
            break;
        }
        if (!cJSON_IsObject(item)) continue;

        attention_item_t *output = &snapshot->items[snapshot->count];
        copy_json_string(output->id, sizeof(output->id), item, "id");
        copy_json_string(output->title, sizeof(output->title), item, "title");
        copy_json_string(output->project, sizeof(output->project), item, "project");
        if (output->id[0] == '\0') continue;
        if (output->title[0] == '\0') strlcpy(output->title, "Untitled Codex thread", sizeof(output->title));
        if (output->project[0] == '\0') strlcpy(output->project, "Codex", sizeof(output->project));
        output->status = parse_status(item);
        output->age_seconds = json_u32(item, "ageSeconds");
        output->unread = json_bool(item, "unread");
        output->pinned = json_bool(item, "pinned");
        output->new_result = json_bool(item, "newResult");
        snapshot->count++;
    }

    if (snapshot->total_count == 0) snapshot->total_count = snapshot->count;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_detail(const char *json, attention_detail_t *detail)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version) || version->valueint != 1) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(detail, 0, sizeof(*detail));
    copy_json_string(detail->id, sizeof(detail->id), root, "id");
    copy_json_string(detail->title, sizeof(detail->title), root, "title");
    copy_json_string(detail->project, sizeof(detail->project), root, "project");
    copy_json_string(detail->kind, sizeof(detail->kind), root, "kind");
    copy_json_string(detail->text, sizeof(detail->text), root, "text");
    strip_output_citations(detail->text);
    detail->truncated = json_bool(root, "truncated");

    if (detail->id[0] == '\0' || detail->text[0] == '\0') {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (detail->title[0] == '\0') strlcpy(detail->title, "Untitled Codex thread", sizeof(detail->title));
    if (detail->project[0] == '\0') strlcpy(detail->project, "Codex", sizeof(detail->project));
    if (detail->kind[0] == '\0') strlcpy(detail->kind, "latest", sizeof(detail->kind));

    cJSON_Delete(root);
    return ESP_OK;
}

static bool valid_thread_id(const char *thread_id)
{
    if (thread_id == NULL || thread_id[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)thread_id; *cursor != '\0'; ++cursor) {
        if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_') return false;
    }
    return true;
}

static esp_err_t bridge_api_url(const char *path, char *output, size_t output_size)
{
    if (path == NULL || output == NULL || strlen(path) >= output_size) return ESP_ERR_INVALID_ARG;
    strlcpy(output, path, output_size);
    return ESP_OK;
}

static esp_err_t detail_url(const char *thread_id, char *output, size_t output_size)
{
    if (!valid_thread_id(thread_id) || output == NULL || output_size == 0) return ESP_ERR_INVALID_ARG;
    const int written = snprintf(output, output_size, "/api/v1/threads/%s/latest", thread_id);
    return written > 0 && (size_t)written < output_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t attention_client_fetch(attention_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;

    response_buffer_t response = { 0 };
    esp_err_t result = perform_get("/api/v1/attention", &response);
    if (result == ESP_OK) result = parse_snapshot(response.data, snapshot);
    free(response.data);
    return result;
}

esp_err_t attention_client_fetch_detail(const char *thread_id, attention_detail_t *detail)
{
    if (detail == NULL) return ESP_ERR_INVALID_ARG;

    char url[DETAIL_URL_MAX];
    esp_err_t result = detail_url(thread_id, url, sizeof(url));
    if (result != ESP_OK) return result;

    response_buffer_t response = { 0 };
    result = perform_get(url, &response);
    if (result == ESP_OK) result = parse_detail(response.data, detail);
    free(response.data);
    return result;
}

static esp_err_t parse_desktop_state(const char *json, attention_desktop_state_t *state)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version) || version->valueint != 1) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(state, 0, sizeof(*state));
    copy_json_string(state->request_id, sizeof(state->request_id), root, "requestId");
    copy_json_string(state->thread_id, sizeof(state->thread_id), root, "threadId");
    state->focus_confidence = parse_focus_confidence(root);
    state->voice_state = parse_voice_state(root);
    parse_capabilities(cJSON_GetObjectItemCaseSensitive(root, "capabilities"), &state->capabilities);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t attention_client_fetch_desktop_state(attention_desktop_state_t *state)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;
    char url[DETAIL_URL_MAX];
    esp_err_t result = bridge_api_url("/api/v1/desktop/state", url, sizeof(url));
    if (result != ESP_OK) return result;
    response_buffer_t response = { 0 };
    result = perform_get(url, &response);
    if (result == ESP_OK) result = parse_desktop_state(response.data, state);
    free(response.data);
    return result;
}

static esp_err_t desktop_command(
    const char *path,
    const char *thread_id,
    const char *command,
    const char *request_id,
    attention_desktop_state_t *state
)
{
    if (!valid_thread_id(thread_id) || request_id == NULL || request_id[0] == '\0' || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *body = cJSON_CreateObject();
    if (body == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(body, "threadId", thread_id);
    cJSON_AddStringToObject(body, "requestId", request_id);
    if (command != NULL) cJSON_AddStringToObject(body, "command", command);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json == NULL) return ESP_ERR_NO_MEM;
    if (strlen(json) >= COMMAND_BODY_MAX) {
        free(json);
        return ESP_ERR_INVALID_SIZE;
    }

    char url[DETAIL_URL_MAX];
    esp_err_t result = bridge_api_url(path, url, sizeof(url));
    response_buffer_t response = { 0 };
    if (result == ESP_OK) result = perform_post(url, json, &response);
    if (result == ESP_OK) result = parse_desktop_state(response.data, state);
    free(json);
    free(response.data);
    return result;
}

esp_err_t attention_client_focus(
    const char *thread_id,
    const char *request_id,
    attention_desktop_state_t *state
)
{
    return desktop_command("/api/v1/desktop/focus", thread_id, NULL, request_id, state);
}

esp_err_t attention_client_voice(
    const char *thread_id,
    const char *command,
    const char *request_id,
    attention_desktop_state_t *state
)
{
    if (command == NULL
        || (strcmp(command, "start-or-resume") != 0 && strcmp(command, "mute") != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    return desktop_command("/api/v1/desktop/voice", thread_id, command, request_id, state);
}
