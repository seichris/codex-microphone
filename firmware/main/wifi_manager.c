#include "wifi_manager.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_sntp_started;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_retry_ms = 1000;

static void reconnect(void *arg)
{
    (void)arg;
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Connect request failed: %s", esp_err_to_name(result));
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_retry_ms * 1000);
    }
}

static void on_time_sync(struct timeval *time_value)
{
    (void)time_value;
    ESP_LOGI(TAG, "Clock synchronized for TLS certificate validation");
}

static void start_sntp_once(void)
{
    if (s_sntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();
    s_sntp_started = true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        reconnect(NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Disconnected (reason %u); retry in %lu ms",
            event == NULL ? 0U : (unsigned)event->reason, (unsigned long)s_retry_ms);
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_retry_ms * 1000);
        if (s_retry_ms < 30000) {
            s_retry_ms *= 2;
            if (s_retry_ms > 30000) s_retry_ms = 30000;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi address lost; waiting for DHCP");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        start_sntp_once();
        esp_timer_stop(s_reconnect_timer);
        s_retry_ms = 1000;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        const ip_event_got_ip_t *event = data;
        if (event != NULL) ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_manager_start(void)
{
    const size_t ssid_length = strlen(CONFIG_CODEX_ATTENTION_WIFI_SSID);
    const size_t password_length = strlen(CONFIG_CODEX_ATTENTION_WIFI_PASSWORD);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64) {
        ESP_LOGW(TAG, "Invalid Wi-Fi credential lengths; check provisioning");
        return ESP_ERR_INVALID_ARG;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t retry_timer = { .callback = reconnect, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_timer, &s_reconnect_timer), TAG, "retry timer failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    esp_err_t loop_result = esp_event_loop_create_default();
    if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) return loop_result;
    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_FAIL;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_wifi_handler),
        TAG,
        "Wi-Fi handler registration failed"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &s_ip_handler),
        TAG,
        "IP handler registration failed"
    );

    wifi_config_t config = { 0 };
    // ESP-IDF accepts full-width 32-byte SSIDs and 64-byte hexadecimal PSKs.
    // strlcpy silently removed their final byte.
    memcpy(config.sta.ssid, CONFIG_CODEX_ATTENTION_WIFI_SSID, ssid_length);
    memcpy(config.sta.password, CONFIG_CODEX_ATTENTION_WIFI_PASSWORD, password_length);
    config.sta.threshold.authmode = strlen(CONFIG_CODEX_ATTENTION_WIFI_PASSWORD) == 0
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "set config failed");
    return esp_wifi_start();
}

bool wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL) return false;
    const TickType_t ticks = timeout_ms == UINT32_MAX
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_events,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        ticks
    );
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    return s_events != NULL && (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}
