#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
#define _DEFAULT_SOURCE 1
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_INVALID_STATE -4
#define ESP_ERR_NOT_FOUND -5
#define ESP_ERR_NOT_SUPPORTED -6
#define ESP_ERR_INVALID_SIZE -7
#define ESP_ERR_INVALID_CRC -8
#define ESP_ERR_INVALID_RESPONSE -9
#define ESP_ERR_NVS_NOT_FOUND -10
#define NVS_READWRITE 1
#define CONFIG_NVS_ENCRYPTION 1
#define CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC 1
#define NVS_SEC_SCHEME_HMAC 1
#define CONFIG_CODEX_ATTENTION_PAIRING_HMAC_KEY_ID 5
#define CONFIG_CODEX_ATTENTION_MAX_ITEMS 20
#define portMAX_DELAY UINT32_MAX
#define MBEDTLS_MD_SHA256 1
#define pdMS_TO_TICKS(ms) (ms)

typedef int esp_err_t;
typedef int hmac_key_id_t;
typedef void *SemaphoreHandle_t;
typedef unsigned nvs_handle_t;
typedef struct { unsigned char eky[32], tky[32]; } nvs_sec_cfg_t;
typedef struct {
    int scheme_id;
    void *scheme_data;
    esp_err_t (*nvs_flash_key_gen)(const void *, nvs_sec_cfg_t *);
} nvs_sec_scheme_t;
typedef struct { hmac_key_id_t hmac_key_id; } nvs_sec_config_hmac_t;
typedef int mbedtls_md_info_t;
typedef struct { X509 *native; } mbedtls_x509_crt;

extern bool host_key_available, host_fail_write, host_fail_commit;
extern unsigned host_nvs_writes, host_nvs_commits;
extern unsigned char host_flash[4096];
extern size_t host_flash_size;
extern int64_t host_now_us;
extern time_t host_wall_time;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout);
int xSemaphoreGive(SemaphoreHandle_t lock);
void esp_fill_random(void *output, size_t size);
esp_err_t esp_hmac_calculate(hmac_key_id_t key, const void *data, size_t size, uint8_t *output);
nvs_sec_scheme_t *nvs_flash_get_default_security_scheme(void);
esp_err_t nvs_flash_read_security_cfg_v2(nvs_sec_scheme_t *scheme, nvs_sec_cfg_t *config);
esp_err_t nvs_flash_secure_init_partition(const char *name, nvs_sec_cfg_t *config);
esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
void mbedtls_x509_crt_init(mbedtls_x509_crt *cert);
int mbedtls_x509_crt_parse(mbedtls_x509_crt *cert, const unsigned char *data, size_t size);
void mbedtls_x509_crt_free(mbedtls_x509_crt *cert);
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type);
int mbedtls_md_hmac(const mbedtls_md_info_t *info, const unsigned char *key, size_t key_size,
    const unsigned char *data, size_t size, unsigned char *output);
int host_settimeofday(const struct timeval *value, const void *zone);
time_t host_time(time_t *value);
#define settimeofday host_settimeofday
#define time host_time
int64_t esp_timer_get_time(void);
size_t strlcpy(char *destination, const char *source, size_t size);

// Minimal declarations of the public cJSON ABI, linked to the system cJSON
// library. No JSON parser behavior is mocked.
typedef struct cJSON {
    struct cJSON *next, *prev, *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;
cJSON *cJSON_Parse(const char *value);
cJSON *cJSON_ParseWithLengthOpts(const char *value, size_t length, const char **end, int require_null);
void cJSON_Delete(cJSON *item);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *key);
int cJSON_IsString(const cJSON *item);
int cJSON_IsNumber(const cJSON *item);
int cJSON_IsObject(const cJSON *item);
int cJSON_IsArray(const cJSON *item);
int cJSON_IsTrue(const cJSON *item);
int cJSON_IsBool(const cJSON *item);
int cJSON_GetArraySize(const cJSON *object);
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_AddStringToObject(cJSON *object, const char *key, const char *value);
char *cJSON_PrintUnformatted(const cJSON *item);
#define cJSON_ArrayForEach(element, array) for ((element) = (array) == NULL ? NULL : (array)->child; (element) != NULL; (element) = (element)->next)

#define HTTP_EVENT_DISCONNECTED 1
#define HTTP_EVENT_ON_DATA 2
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define HTTP_TRANSPORT_OVER_SSL 1
#define ESP_IPADDR_TYPE_V4 0
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (unsigned)((ip)->addr & 255), (unsigned)(((ip)->addr >> 8) & 255), (unsigned)(((ip)->addr >> 16) & 255), (unsigned)(((ip)->addr >> 24) & 255)

typedef struct {
    int event_id;
    void *user_data, *data;
    int data_len;
} esp_http_client_event_t;
typedef struct {
    const char *host, *path, *cert_pem, *common_name, *user_agent;
    int port, transport_type, max_authorization_retries, timeout_ms, buffer_size, buffer_size_tx;
    bool disable_auto_redirect;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
} esp_http_client_config_t;
typedef struct host_http_client *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, int method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *body, int length);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_tls_get_and_clear_last_error(void *handle, int *error, int *flags);

typedef struct { const char *key, *value; } mdns_txt_item_t;
typedef struct mdns_ip_addr_s {
    struct { int type; union { struct { uint32_t addr; } ip4; } u_addr; } addr;
    struct mdns_ip_addr_s *next;
} mdns_ip_addr_t;
typedef struct mdns_result_s {
    struct mdns_result_s *next;
    uint16_t port;
    mdns_txt_item_t *txt;
    uint8_t *txt_value_len;
    size_t txt_count;
    mdns_ip_addr_t *addr;
} mdns_result_t;
extern mdns_result_t host_mdns_result;
extern bool host_wifi_connected, host_mdns_available;
extern unsigned host_discoveries, host_http_calls;
esp_err_t mdns_init(void);
esp_err_t mdns_query_ptr(const char *service, const char *proto, uint32_t timeout, size_t max, mdns_result_t **results);
void mdns_query_results_free(mdns_result_t *results);
bool wifi_manager_is_connected(void);

// Old sdkconfig provisioning must not affect the production attention client.
#define CONFIG_CODEX_ATTENTION_BRIDGE_URL "http://obsolete.invalid/api/v1/attention"
#define CONFIG_CODEX_ATTENTION_BRIDGE_TOKEN "legacy-admin-not-a-device-credential"
