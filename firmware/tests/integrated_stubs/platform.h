#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_TIMEOUT -4
#define ESP_ERR_NO_MEM -5
#define ESP_ERR_NOT_FOUND -6
#define ESP_CODEC_DEV_OK 0
static inline void test_log(const char *format, ...) { (void)format; }
#define ESP_LOGI(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY UINT32_MAX
#define BIT0 1U
#define BIT1 2U
#define BIT2 4U
#define BIT3 8U
#define BIT4 16U
#define BIT5 32U
#define BIT6 64U
struct fake_semaphore { int available; bool mutex; };
typedef struct fake_semaphore *SemaphoreHandle_t;
struct fake_queue;
typedef struct fake_queue *QueueHandle_t;
typedef struct { int unused; } StaticQueue_t;
typedef void *TaskHandle_t;
typedef void *esp_codec_dev_handle_t;
typedef struct {
    int clk_cfg, slot_cfg;
    struct { int mclk, bclk, ws, dout, din;
        struct { bool mclk_inv, bclk_inv, ws_inv; } invert_flags;
    } gpio_cfg;
} i2s_std_config_t;
typedef struct { int bits_per_sample, channel, channel_mask, sample_rate, mclk_multiple; } esp_codec_dev_sample_info_t;
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(bits, mode) (bits)
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
#define BSP_I2S_MCLK 1
#define BSP_I2S_SCLK 2
#define BSP_I2S_LCLK 3
#define BSP_I2S_DOUT 4
#define BSP_I2S_DSIN 5
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_malloc(size_t, unsigned);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
QueueHandle_t xQueueCreateStatic(unsigned, size_t, uint8_t *, StaticQueue_t *);
int xQueueReset(QueueHandle_t);
int xQueueSend(QueueHandle_t, const void *, TickType_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
esp_err_t bsp_audio_init(const i2s_std_config_t *);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
int esp_codec_dev_open(esp_codec_dev_handle_t, const esp_codec_dev_sample_info_t *);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t, float);
int esp_codec_dev_read(esp_codec_dev_handle_t, void *, int);
#define CONFIG_CODEX_ATTENTION_WIRELESS_ENABLED 1
#define CONFIG_CODEX_ATTENTION_WIRELESS_URL "wss://test.invalid"
#define CONFIG_CODEX_ATTENTION_WIRELESS_SERVER_NAME "test.invalid"
#define CONFIG_CODEX_ATTENTION_WIRELESS_CA_PEM "-----BEGIN CERTIFICATE-----\\nfixture\\n-----END CERTIFICATE-----\\n"
#define CONFIG_CODEX_ATTENTION_WIRELESS_DEVICE_ID "test-board"
#define CONFIG_CODEX_ATTENTION_WIRELESS_CREDENTIAL "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
typedef uint32_t EventBits_t;
typedef EventBits_t *EventGroupHandle_t;
typedef const char *esp_event_base_t;
typedef void *esp_websocket_client_handle_t;
typedef struct {
    const char *uri, *cert_common_name, *cert_pem, *subprotocol;
    int buffer_size, task_stack;
    bool enable_close_reconnect;
} esp_websocket_client_config_t;
typedef struct {
    int esp_tls_last_esp_err, esp_tls_stack_err, esp_tls_cert_verify_flags;
    int error_type, esp_ws_handshake_status_code, esp_transport_sock_errno;
} esp_websocket_error_codes_t;
typedef struct {
    const char *data_ptr;
    int data_len, payload_len, payload_offset, op_code;
    bool fin;
    esp_websocket_error_codes_t error_handle;
} esp_websocket_event_data_t;
enum { WEBSOCKET_EVENT_ANY, WEBSOCKET_EVENT_CONNECTED, WEBSOCKET_EVENT_DISCONNECTED,
       WEBSOCKET_EVENT_CLOSED, WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_DATA };
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t);
EventBits_t xEventGroupGetBits(EventGroupHandle_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t, int, int, TickType_t);
int64_t esp_timer_get_time(void);
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t, int,
    void (*)(void *, esp_event_base_t, int32_t, void *), void *);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t);
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t);
int esp_websocket_client_send_text(esp_websocket_client_handle_t, const char *, int, TickType_t);
int esp_websocket_client_send_bin(esp_websocket_client_handle_t, const char *, int, TickType_t);
#undef strlcpy
size_t strlcpy(char *, const char *, size_t);
// Fixtures replace cJSON parsing only, not lifecycle/capture/queue/framing.
typedef struct cJSON {
    struct cJSON *next, *child;
    const char *key;
    int type;
    char *valuestring;
    double valuedouble;
} cJSON;
cJSON *cJSON_CreateObject(void);
void cJSON_Delete(cJSON *);
void cJSON_free(void *);
void cJSON_AddStringToObject(cJSON *, const char *, const char *);
void cJSON_AddNumberToObject(cJSON *, const char *, double);
const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *, const char *);
bool cJSON_IsString(const cJSON *);
bool cJSON_IsNumber(const cJSON *);
bool cJSON_IsObject(const cJSON *);
cJSON *cJSON_ParseWithLength(const char *, size_t);
char *cJSON_PrintUnformatted(const cJSON *);
typedef struct { int unused; } mbedtls_x509_crt;
static inline void mbedtls_x509_crt_init(mbedtls_x509_crt *c) { (void)c; }
static inline void mbedtls_x509_crt_free(mbedtls_x509_crt *c) { (void)c; }
static inline int mbedtls_x509_crt_parse(mbedtls_x509_crt *c, const unsigned char *p, size_t n) { (void)c; (void)p; (void)n; return 0; }
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
