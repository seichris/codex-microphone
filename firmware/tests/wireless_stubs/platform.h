#pragma once
#include "../audio_stubs/platform.h"
static inline void test_log(const char *format, ...) { (void)format; }
#undef ESP_LOGW
#define ESP_LOGW(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
#define BIT0 1U
#define BIT1 2U
#define BIT2 4U
#define BIT3 8U
#define BIT4 16U
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
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t);
int esp_websocket_client_send_text(esp_websocket_client_handle_t, const char *, int, TickType_t);
int esp_websocket_client_send_bin(esp_websocket_client_handle_t, const char *, int, TickType_t);
#undef strlcpy
size_t strlcpy(char *, const char *, size_t);

// JSON fixtures model parsed fields, not parser behavior. The transport tests
// inject these objects into the real control handler; they do not test cJSON.
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

static inline void vTaskDelete(TaskHandle_t t) { (void)t; }
