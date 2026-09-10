#include "attention_host_platform.h"
#include <stdio.h>
#include <pthread.h>
#include <openssl/rand.h>

bool host_key_available = true, host_fail_write, host_fail_commit;
unsigned host_nvs_writes, host_nvs_commits;
unsigned char host_flash[4096];
size_t host_flash_size;
int64_t host_now_us = 1000000;
time_t host_wall_time;
static unsigned char pending[4096];
static size_t pending_size;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *lock = malloc(sizeof(*lock));
    assert(lock != NULL && pthread_mutex_init(lock, NULL) == 0);
    return lock;
}
int xSemaphoreTake(SemaphoreHandle_t lock, unsigned timeout) { (void)timeout; return pthread_mutex_lock(lock) == 0; }
int xSemaphoreGive(SemaphoreHandle_t lock) { return pthread_mutex_unlock(lock) == 0; }
void esp_fill_random(void *output, size_t size) { assert(RAND_bytes(output, (int)size) == 1); }
esp_err_t esp_hmac_calculate(hmac_key_id_t key, const void *data, size_t size, uint8_t *output)
{
    assert(key == 5);
    const uint8_t owner_key[32] = { 0x72 }; // synthetic host-only eFuse fixture
    return HMAC(EVP_sha256(), owner_key, sizeof(owner_key), data, size, output, NULL) ? ESP_OK : ESP_FAIL;
}
static esp_err_t forbidden_key_generation(const void *data, nvs_sec_cfg_t *config)
{
    (void)data; (void)config;
    assert(!"firmware must never invoke eFuse key generation");
    return ESP_FAIL;
}
nvs_sec_scheme_t *nvs_flash_get_default_security_scheme(void)
{
    static nvs_sec_config_hmac_t config = { .hmac_key_id = 0 };
    static nvs_sec_scheme_t scheme = { .scheme_id = NVS_SEC_SCHEME_HMAC,
        .scheme_data = &config, .nvs_flash_key_gen = forbidden_key_generation };
    return &scheme;
}
esp_err_t nvs_flash_read_security_cfg_v2(nvs_sec_scheme_t *scheme, nvs_sec_cfg_t *config)
{
    assert(scheme && config && scheme->scheme_id == NVS_SEC_SCHEME_HMAC);
    assert(scheme->nvs_flash_key_gen == NULL); // disabled even on an unprepared board
    assert(((nvs_sec_config_hmac_t *)scheme->scheme_data)->hmac_key_id == 5);
    memset(config, 0x55, sizeof(*config));
    return host_key_available ? ESP_OK : ESP_ERR_NOT_FOUND;
}
esp_err_t nvs_flash_secure_init_partition(const char *name, nvs_sec_cfg_t *config)
{
    assert(!strcmp(name, "attn_pair") && config->eky[0] == 0x55); return ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *partition, const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(partition, "attn_pair") && !strcmp(name, "pairing") && mode == NVS_READWRITE); *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle == 1 && !strcmp(key, "record"));
    if (!host_flash_size) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < host_flash_size) { *size = host_flash_size; return ESP_ERR_INVALID_SIZE; }
    memcpy(value, host_flash, host_flash_size); *size = host_flash_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    assert(handle == 1 && !strcmp(key, "record") && size <= sizeof(pending)); ++host_nvs_writes;
    if (host_fail_write) return ESP_FAIL;
    memcpy(pending, value, size); pending_size = size; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1); ++host_nvs_commits;
    if (host_fail_commit) return ESP_FAIL;
    memcpy(host_flash, pending, pending_size); host_flash_size = pending_size; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
void mbedtls_x509_crt_init(mbedtls_x509_crt *cert) { cert->native = NULL; }
int mbedtls_x509_crt_parse(mbedtls_x509_crt *cert, const unsigned char *data, size_t size)
{
    BIO *input = BIO_new_mem_buf(data, (int)size);
    cert->native = PEM_read_bio_X509(input, NULL, NULL, NULL);
    BIO_free(input);
    return cert->native ? 0 : -1;
}
void mbedtls_x509_crt_free(mbedtls_x509_crt *cert) { X509_free(cert->native); cert->native = NULL; }
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) { static const int info = 1; assert(type == MBEDTLS_MD_SHA256); return &info; }
int mbedtls_md_hmac(const mbedtls_md_info_t *info, const unsigned char *key, size_t key_size,
    const unsigned char *data, size_t size, unsigned char *output)
{
    assert(info != NULL); return HMAC(EVP_sha256(), key, (int)key_size, data, size, output, NULL) ? 0 : -1;
}
int host_settimeofday(const struct timeval *value, const void *zone) { (void)zone; host_wall_time = value->tv_sec; return 0; }
time_t host_time(time_t *value) { if (value) *value = host_wall_time; return host_wall_time; }
int64_t esp_timer_get_time(void) { return host_now_us; }
size_t strlcpy(char *destination, const char *source, size_t size)
{
    const size_t length = strlen(source);
    if (size) { const size_t copied = length < size - 1 ? length : size - 1; memcpy(destination, source, copied); destination[copied] = 0; }
    return length;
}
