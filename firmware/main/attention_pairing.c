#include "attention_pairing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_hmac.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"
#include "sdkconfig.h"

static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static bool s_ready;
static attention_pairing_record_t s_record;
static char s_ack_nonce[65], s_ack_proof[65];
static bool s_ack_pending;

static bool equal_bytes(const uint8_t *a, const uint8_t *b, size_t size)
{
    unsigned different = 0;
    for (size_t i = 0; i < size; ++i) different |= a[i] ^ b[i];
    return different == 0;
}

static void hex(const uint8_t *bytes, size_t length, char *output)
{
    static const char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) { output[i * 2] = alphabet[bytes[i] >> 4]; output[i * 2 + 1] = alphabet[bytes[i] & 15]; }
    output[length * 2] = 0;
}

static esp_err_t seal(attention_pairing_record_t *record)
{
    return esp_hmac_calculate((hmac_key_id_t)CONFIG_CODEX_ATTENTION_PAIRING_HMAC_KEY_ID,
        record, offsetof(attention_pairing_record_t, seal), record->seal);
}

// Caller owns s_lock. There is one authenticated blob, never separate keys
// that can mix generations, and never a fallback to an older corrupt slot.
static esp_err_t persist(attention_pairing_record_t *next)
{
    if (!attention_pairing_record_valid(next)) return ESP_ERR_INVALID_ARG;
    // Flash/cache operations must not read a PSRAM source buffer. The caller
    // also runs on the internal-stack provisioning actor (never the poll task).
    attention_pairing_record_t *staged = heap_caps_malloc(sizeof(*staged), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (staged == NULL) return ESP_ERR_NO_MEM;
    *staged = *next;
    esp_err_t result = seal(staged);
    if (result == ESP_OK) result = nvs_set_blob(s_nvs, "record", staged, sizeof(*staged));
    if (result == ESP_OK) result = nvs_commit(s_nvs);
    if (result == ESP_OK) s_record = *staged;
    else s_ready = false; // A failed flash write has an uncertain durable outcome. Reboot and verify.
    attention_pairing_zero(staged, sizeof(*staged));
    free(staged);
    return result;
}

esp_err_t attention_pairing_init(void)
{
#if !CONFIG_NVS_ENCRYPTION
    return ESP_ERR_NOT_SUPPORTED;
#else
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    nvs_sec_scheme_t *scheme = NULL;
    static const nvs_sec_config_hmac_t hmac_config = { .hmac_key_id = (hmac_key_id_t)CONFIG_CODEX_ATTENTION_PAIRING_HMAC_KEY_ID };
    nvs_sec_cfg_t encryption = { 0 };
    esp_err_t result = nvs_sec_provider_register_hmac(&hmac_config, &scheme);
    // READ ONLY. nvs_flash_generate_keys_v2 would burn an eFuse on a fresh
    // board. Factory/owner security provisioning is deliberately separate.
    if (result == ESP_OK) result = nvs_flash_read_security_cfg_v2(scheme, &encryption);
    if (result == ESP_OK) result = nvs_flash_secure_init_partition(ATTENTION_PAIRING_PARTITION, &encryption);
    attention_pairing_zero(&encryption, sizeof(encryption));
    if (scheme != NULL) nvs_sec_provider_deregister(scheme);
    if (result != ESP_OK) return result;
    result = nvs_open_from_partition(ATTENTION_PAIRING_PARTITION, "pairing", NVS_READWRITE, &s_nvs);
    if (result != ESP_OK) return result;
    size_t size = sizeof(s_record);
    result = nvs_get_blob(s_nvs, "record", &s_record, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        s_record.magic = ATTENTION_PAIRING_MAGIC;
        s_record.version = ATTENTION_PAIRING_VERSION;
        result = ESP_OK;
    } else if (result == ESP_OK) {
        uint8_t saved[32];
        memcpy(saved, s_record.seal, sizeof(saved));
        if (size != sizeof(s_record) || !attention_pairing_record_valid(&s_record)
            || seal(&s_record) != ESP_OK || !equal_bytes(saved, s_record.seal, sizeof(saved))) result = ESP_ERR_INVALID_CRC;
        attention_pairing_zero(saved, sizeof(saved));
    }
    if (result != ESP_OK) { attention_pairing_zero(&s_record, sizeof(s_record)); nvs_close(s_nvs); return result; }
    s_ready = true;
    // Seed cold-boot certificate validation from the authenticated local
    // provisioning time. SNTP and authenticated bridge responses advance it.
    if (s_record.state != ATTENTION_PAIRING_EMPTY && time(NULL) < (time_t)s_record.provisioned_at) {
        struct timeval now = { .tv_sec = s_record.provisioned_at };
        settimeofday(&now, NULL);
    }
    return ESP_OK;
#endif
}

bool attention_pairing_storage_ready(void) { return s_ready; }

esp_err_t attention_pairing_copy(attention_pairing_record_t *record)
{
    if (record == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ready) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    *record = s_record;
    xSemaphoreGive(s_lock);
    return record->state == ATTENTION_PAIRING_EMPTY ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t attention_pairing_install(const attention_pairing_record_t *record)
{
    if (!s_ready || record == NULL || record->state != ATTENTION_PAIRING_ACTIVE
        || !attention_pairing_record_valid(record)) return ESP_ERR_INVALID_STATE;
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    const int parsed = mbedtls_x509_crt_parse(&certificate, (const unsigned char *)record->certificate, strlen(record->certificate) + 1);
    mbedtls_x509_crt_free(&certificate);
    if (parsed != 0) return ESP_ERR_INVALID_ARG;
    attention_pairing_record_t *next = malloc(sizeof(*next));
    if (next == NULL) return ESP_ERR_NO_MEM;
    *next = *record;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t result = s_record.state != ATTENTION_PAIRING_EMPTY ? ESP_ERR_INVALID_STATE : persist(next);
    xSemaphoreGive(s_lock);
    attention_pairing_zero(next, sizeof(*next));
    free(next);
    return result;
}

esp_err_t attention_pairing_begin_reset(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    attention_pairing_record_t *next = malloc(sizeof(*next));
    if (next == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *next = s_record;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (next->state == ATTENTION_PAIRING_ACTIVE) {
        uint8_t nonce[32];
        esp_fill_random(nonce, sizeof(nonce));
        hex(nonce, sizeof(nonce), next->reset_nonce);
        attention_pairing_zero(nonce, sizeof(nonce));
        next->state = ATTENTION_PAIRING_RESETTING;
        result = persist(next);
    } else if (next->state == ATTENTION_PAIRING_RESETTING) result = ESP_OK;
    xSemaphoreGive(s_lock);
    attention_pairing_zero(next, sizeof(*next));
    free(next);
    return result;
}

esp_err_t attention_pairing_proof(const attention_pairing_record_t *r, const char *domain,
    const char *first, const char *second, char output[65])
{
    if (!attention_pairing_record_valid(r) || r->state == ATTENTION_PAIRING_EMPTY
        || domain == NULL || !attention_pairing_hex(first, 64)
        || (second != NULL && !attention_pairing_hex(second, 64))) return ESP_ERR_INVALID_ARG;
    char input[320];
    const int length = snprintf(input, sizeof(input), "codex-attention-%s-v1\n%s\n%s\n%lu\n%s%s%s",
        domain, r->bridge_id, r->device_id, (unsigned long)r->generation, first,
        second == NULL ? "" : "\n", second == NULL ? "" : second);
    if (length < 0 || (size_t)length >= sizeof(input)) return ESP_ERR_INVALID_SIZE;
    uint8_t key[32], mac[32];
    for (size_t i = 0; i < sizeof(key); ++i) {
        unsigned value;
        if (sscanf(r->secret + i * 2, "%2x", &value) != 1) return ESP_ERR_INVALID_ARG;
        key[i] = (uint8_t)value;
    }
    const int result = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, sizeof(key),
        (const unsigned char *)input, (size_t)length, mac);
    if (result == 0) hex(mac, sizeof(mac), output);
    attention_pairing_zero(key, sizeof(key));
    attention_pairing_zero(mac, sizeof(mac));
    attention_pairing_zero(input, sizeof(input));
    return result == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t attention_pairing_complete_reset(const char *nonce, const char *proof)
{
    if (!s_ready || !attention_pairing_hex(nonce, 64) || !attention_pairing_hex(proof, 64)) return ESP_ERR_INVALID_ARG;
    attention_pairing_record_t *next = calloc(1, sizeof(*next));
    if (next == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char expected[65];
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_record.state == ATTENTION_PAIRING_RESETTING && strcmp(s_record.reset_nonce, nonce) == 0
        && attention_pairing_proof(&s_record, "revoked", nonce, NULL, expected) == ESP_OK
        && equal_bytes((const uint8_t *)proof, (const uint8_t *)expected, 64)) {
        next->magic = ATTENTION_PAIRING_MAGIC;
        next->version = ATTENTION_PAIRING_VERSION;
        result = persist(next);
    }
    xSemaphoreGive(s_lock);
    attention_pairing_zero(expected, sizeof(expected));
    attention_pairing_zero(next, sizeof(*next));
    free(next);
    return result;
}

// A single bounded mailbox is sufficient: reset has one durable nonce. A lost
// mailbox on reboot is recovered by retrying the idempotent bridge revocation.
esp_err_t attention_pairing_queue_reset_ack(const char *nonce, const char *proof)
{
    if (!s_ready || !attention_pairing_hex(nonce, 64) || !attention_pairing_hex(proof, 64)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_record.state == ATTENTION_PAIRING_RESETTING && strcmp(s_record.reset_nonce, nonce) == 0) {
        memcpy(s_ack_nonce, nonce, sizeof(s_ack_nonce));
        memcpy(s_ack_proof, proof, sizeof(s_ack_proof));
        s_ack_pending = true;
        result = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t attention_pairing_process_reset_ack(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char nonce[65], proof[65];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool pending = s_ack_pending;
    memcpy(nonce, s_ack_nonce, sizeof(nonce));
    memcpy(proof, s_ack_proof, sizeof(proof));
    attention_pairing_zero(s_ack_nonce, sizeof(s_ack_nonce));
    attention_pairing_zero(s_ack_proof, sizeof(s_ack_proof));
    s_ack_pending = false;
    xSemaphoreGive(s_lock);
    const esp_err_t result = pending ? attention_pairing_complete_reset(nonce, proof) : ESP_ERR_NOT_FOUND;
    attention_pairing_zero(nonce, sizeof(nonce));
    attention_pairing_zero(proof, sizeof(proof));
    return result;
}
