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
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_sntp_started;

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
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Disconnected; reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        start_sntp_once();
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Connected");
    }
}

esp_err_t wifi_manager_start(void)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) return ESP_ERR_NO_MEM;

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
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, &s_ip_handler),
        TAG,
        "IP handler registration failed"
    );

    wifi_config_t config = { 0 };
    strlcpy((char *)config.sta.ssid, CONFIG_CODEX_ATTENTION_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_CODEX_ATTENTION_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = strlen(CONFIG_CODEX_ATTENTION_WIFI_PASSWORD) == 0
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
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
