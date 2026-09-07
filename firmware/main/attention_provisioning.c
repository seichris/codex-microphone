#include "attention_provisioning.h"
#include "attention_confirmation.h"
#include "attention_connection.h"
#include "attention_pairing.h"
#include "attention_ui.h"
#include "voice_audio.h"
#include "bsp/esp-bsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define SERIAL_LIMIT 8192
#define SERIAL_PREFIX "CODEX-ATTENTION "

typedef enum { ACTION_NONE, ACTION_PAIR, ACTION_RESET } action_t;
static SemaphoreHandle_t s_lock;
static attention_confirmation_t s_confirmation;
static attention_pairing_record_t *s_pending;
static action_t s_action;
static bool s_busy;
static bool s_serial_ready;
static char s_candidate[33];

static void reply(const char *result)
{
    // The serial input is never echoed. Only these bounded, non-secret replies
    // reach the console; credentials must never be formatted through ESP_LOG.
    printf(SERIAL_PREFIX "{\"version\":1,\"result\":\"%s\"}\n", result);
    fflush(stdout);
}

static void status(const char *message, bool failed)
{
    bsp_display_lock(0);
    attention_ui_show_pairing_prompt(message, failed);
    bsp_display_unlock();
}

static void new_candidate(void)
{
    uint8_t bytes[16];
    static const char digits[] = "0123456789abcdef";
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i) { s_candidate[2*i] = digits[bytes[i] >> 4]; s_candidate[2*i+1] = digits[bytes[i] & 15]; }
    s_candidate[32] = 0;
    attention_pairing_zero(bytes, sizeof(bytes));
}

static bool exact_keys(const cJSON *object, const char *const *keys, size_t count)
{
    if (!cJSON_IsObject(object) || (size_t)cJSON_GetArraySize(object) != count) return false;
    for (size_t i = 0; i < count; ++i) {
        unsigned found = 0;
        for (const cJSON *item = object->child; item != NULL; item = item->next)
            if (item->string != NULL && !strcmp(item->string, keys[i])) ++found;
        if (found != 1) return false;
    }
    return true;
}

static const char *text(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool string_field(const cJSON *object, const char *key, char *output, size_t capacity)
{
    const char *value = text(object, key);
    if (value == NULL || strlen(value) >= capacity) return false;
    memcpy(output, value, strlen(value) + 1);
    return true;
}

static bool number_field(const cJSON *object, const char *key, uint32_t *output)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > UINT32_MAX
        || value->valuedouble != (uint32_t)value->valuedouble) return false;
    *output = (uint32_t)value->valuedouble;
    return true;
}

static attention_pairing_record_t *parse_record(const cJSON *root)
{
    static const char *const keys[] = { "command", "version", "deviceId", "bridgeId", "generation", "secret",
        "ssid", "password", "certificate", "fallbackHost", "port", "provisionedAt" };
    if (!exact_keys(root, keys, sizeof(keys)/sizeof(keys[0]))) return NULL;
    attention_pairing_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) return NULL;
    record->magic = ATTENTION_PAIRING_MAGIC;
    record->state = ATTENTION_PAIRING_ACTIVE;
    bool valid = number_field(root, "version", &record->version)
        && number_field(root, "generation", &record->generation) && number_field(root, "port", &record->port)
        && number_field(root, "provisionedAt", &record->provisioned_at)
        && string_field(root, "deviceId", record->device_id, sizeof(record->device_id))
        && !strcmp(record->device_id, s_candidate)
        && string_field(root, "bridgeId", record->bridge_id, sizeof(record->bridge_id))
        && string_field(root, "secret", record->secret, sizeof(record->secret))
        && string_field(root, "ssid", record->ssid, sizeof(record->ssid))
        && string_field(root, "password", record->password, sizeof(record->password))
        && string_field(root, "certificate", record->certificate, sizeof(record->certificate))
        && string_field(root, "fallbackHost", record->fallback_host, sizeof(record->fallback_host))
        && attention_pairing_record_valid(record);
    if (valid) return record;
    attention_pairing_zero(record, sizeof(*record));
    free(record);
    return NULL;
}

static void hello(void)
{
    attention_pairing_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) { reply("no_memory"); return; }
    const esp_err_t result = attention_pairing_copy(record);
    printf(SERIAL_PREFIX "{\"version\":1,\"result\":\"hello\",\"storageReady\":%s,\"state\":%lu,"
        "\"deviceId\":\"%s\",\"bridgeId\":\"%s\",\"candidateId\":\"%s\",\"resetNonce\":\"%s\"}\n",
        attention_pairing_storage_ready() ? "true" : "false",
        (unsigned long)(result == ESP_OK ? record->state : ATTENTION_PAIRING_EMPTY),
        result == ESP_OK ? record->device_id : "", result == ESP_OK ? record->bridge_id : "", s_candidate,
        result == ESP_OK && record->state == ATTENTION_PAIRING_RESETTING ? record->reset_nonce : "");
    fflush(stdout);
    attention_pairing_zero(record, sizeof(*record));
    free(record);
}

static void stage(action_t action, attention_pairing_record_t *record)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending = record;
    s_action = action;
    s_busy = true;
    attention_confirmation_begin(&s_confirmation, (uint64_t)esp_timer_get_time() / 1000);
    xSemaphoreGive(s_lock);
    // Pairing/reset is not permission to keep recording during a modal.
    (void)voice_audio_revoke_capture();
    char message[128];
    if (action == ACTION_PAIR) snprintf(message, sizeof(message), "Pair Mac %.8s? Release, then hold BOOT 1.5s. PWR cancels", record->bridge_id);
    else strlcpy(message, "Reset pairing? Release, then hold BOOT 1.5s. PWR cancels", sizeof(message));
    status(message, false);
    reply("confirm_on_device");
}

static void command(char *line, size_t length)
{
    if (memchr(line, 0, length) != NULL) { reply("invalid_request"); return; }
    cJSON *root = cJSON_ParseWithLengthOpts(line, length + 1, NULL, true);
    const char *operation = text(root, "command");
    if (operation == NULL) { cJSON_Delete(root); reply("invalid_request"); return; }
    static const char *const simple[] = { "command" };
    static const char *const ack[] = { "command", "nonce", "proof" };
    if (!strcmp(operation, "hello") && exact_keys(root, simple, 1)) hello();
    else if (attention_provisioning_active()) reply("busy");
    else if (!attention_pairing_storage_ready()) reply("secure_storage_unavailable");
    else if (!strcmp(operation, "pair")) {
        attention_pairing_record_t *current = calloc(1, sizeof(*current));
        const bool empty = current != NULL && attention_pairing_copy(current) == ESP_ERR_NOT_FOUND;
        if (current != NULL) attention_pairing_zero(current, sizeof(*current));
        free(current);
        attention_pairing_record_t *record = empty ? parse_record(root) : NULL;
        if (record == NULL) reply(empty ? "invalid_record" : "reset_required");
        else stage(ACTION_PAIR, record);
    } else if (!strcmp(operation, "reset") && exact_keys(root, simple, 1)) stage(ACTION_RESET, NULL);
    else if (!strcmp(operation, "reset-ack") && exact_keys(root, ack, 3)) {
        const esp_err_t result = attention_pairing_complete_reset(text(root, "nonce"), text(root, "proof"));
        if (result == ESP_OK) { new_candidate(); reply("reset_complete"); }
        else reply("invalid_reset_ack");
    } else reply("invalid_request");
    // Erase sensitive JSON values before cJSON releases its allocations.
    for (cJSON *item = root->child; item != NULL; item = item->next)
        if (cJSON_IsString(item) && item->valuestring != NULL) attention_pairing_zero(item->valuestring, strlen(item->valuestring));
    cJSON_Delete(root);
}

bool attention_provisioning_active(void)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = s_busy;
    xSemaphoreGive(s_lock);
    return active;
}

bool attention_provisioning_tick(button_input_event_t event)
{
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = s_busy;
    if (active) attention_confirmation_update(&s_confirmation, (uint64_t)esp_timer_get_time() / 1000,
        gpio_get_level(GPIO_NUM_0) == 0, event == BUTTON_INPUT_PWR_SHORT || event == BUTTON_INPUT_PWR_LONG);
    xSemaphoreGive(s_lock);
    return active;
}

static void complete_action(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_busy || attention_confirmation_pending(&s_confirmation)) { xSemaphoreGive(s_lock); return; }
    const bool accepted = s_confirmation.phase == ATTENTION_CONFIRM_ACCEPTED;
    const action_t action = s_action;
    attention_pairing_record_t *record = s_pending;
    s_pending = NULL;
    s_action = ACTION_NONE;
    xSemaphoreGive(s_lock);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (accepted) result = action == ACTION_PAIR ? attention_pairing_install(record) : attention_connection_begin_reset();
    if (record != NULL) { attention_pairing_zero(record, sizeof(*record)); free(record); }
    if (!accepted) { reply("cancelled_or_expired"); status("Pairing cancelled", false); }
    else if (result != ESP_OK) { reply("storage_or_state_error"); status("Pairing could not be saved", true); }
    else if (action == ACTION_PAIR) {
        reply("paired");
        status("Pairing saved. Restarting", false);
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else { reply("reset_pending"); hello(); status("Reset pending bridge revocation", false); }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_busy = false;
    xSemaphoreGive(s_lock);
    bsp_display_lock(0);
    attention_ui_hide_pairing_prompt();
    bsp_display_unlock();
}

static void serial_task(void *argument)
{
    (void)argument;
    char *line = calloc(1, SERIAL_LIMIT + 1);
    if (line == NULL) { reply("no_memory"); vTaskDelete(NULL); return; }
    size_t length = 0;
    bool discard = false;
    int64_t last_byte = 0;
    while (true) {
        // This actor uses an INTERNAL stack: NVS writes must not run on the
        // PSRAM-backed network poll task, even when reset arrived over HTTPS.
        if (attention_pairing_process_reset_ack() == ESP_OK) { new_candidate(); reply("reset_complete"); }
        complete_action();
        if (!s_serial_ready) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        uint8_t byte;
        const int count = uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(20));
        const int64_t now = esp_timer_get_time();
        if (length && now - last_byte > 5LL * 1000000) {
            attention_pairing_zero(line, SERIAL_LIMIT + 1);
            length = 0;
            discard = true;
        }
        if (count != 1) continue;
        last_byte = now;
        if (byte == '\n') {
            if (!discard && length) {
                if (line[length - 1] == '\r') --length;
                line[length] = 0;
                command(line, length);
            } else if (discard) reply("invalid_request");
            attention_pairing_zero(line, SERIAL_LIMIT + 1);
            length = 0;
            discard = false;
        } else if (!discard) {
            if (length == SERIAL_LIMIT) { attention_pairing_zero(line, SERIAL_LIMIT + 1); length = 0; discard = true; }
            else line[length++] = (char)byte;
        }
    }
}

esp_err_t attention_provisioning_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    new_candidate();
    // UART0 maintenance channel, not the native TinyUSB UAC port. This keeps
    // the existing USB microphone descriptors and audio path unchanged.
    const uart_config_t config = { .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT };
    esp_err_t result = uart_param_config(UART_NUM_0, &config);
    if (result == ESP_OK && !uart_is_driver_installed(UART_NUM_0))
        result = uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0);
    s_serial_ready = result == ESP_OK;
    // Keep the reset-persistence actor alive even if the maintenance UART is
    // unavailable. A confirmed pending reset can still finish over the LAN.
    if (xTaskCreate(serial_task, "attention_pair", 8192, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return result;
}
