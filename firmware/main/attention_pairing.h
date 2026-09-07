#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ATTENTION_PAIRING_VERSION 1U
#define ATTENTION_PAIRING_MAGIC 0x31415043U
#define ATTENTION_PAIRING_CERT_MAX 2048
#define ATTENTION_PAIRING_PARTITION "attn_pair"

typedef enum { ATTENTION_PAIRING_EMPTY = 0, ATTENTION_PAIRING_ACTIVE = 1,
    ATTENTION_PAIRING_RESETTING = 2 } attention_pairing_state_t;

// Fixed-width, zero-initialized wire-independent storage layout. The eFuse
// HMAC authenticates every byte preceding seal; encrypted NVS hides the blob.
typedef struct {
    uint32_t magic, version, generation, state, port, provisioned_at;
    char device_id[33], bridge_id[33], secret[65];
    char ssid[33], password[65];
    char certificate[ATTENTION_PAIRING_CERT_MAX];
    char fallback_host[254];
    char reset_nonce[65];
    uint8_t seal[32];
} attention_pairing_record_t;
_Static_assert(sizeof(attention_pairing_record_t) == 2652, "Pairing layout changed; bump schema version");
_Static_assert(offsetof(attention_pairing_record_t, seal) == 2620, "Pairing seal layout changed");

bool attention_pairing_record_valid(const attention_pairing_record_t *record);
bool attention_pairing_hex(const char *text, size_t length);
bool attention_pairing_hostname(const char *text);
bool attention_discovery_txt_matches(const char *bridge_id, const char *const *keys,
    const char *const *values, size_t count);
void attention_pairing_zero(void *data, size_t length);

esp_err_t attention_pairing_init(void);
bool attention_pairing_storage_ready(void);
esp_err_t attention_pairing_copy(attention_pairing_record_t *record);
esp_err_t attention_pairing_install(const attention_pairing_record_t *record);
esp_err_t attention_pairing_begin_reset(void);
// Persistent mutations must run on an internal-RAM stack. Network tasks only
// enqueue authenticated acknowledgements; the internal provisioning actor drains them.
esp_err_t attention_pairing_queue_reset_ack(const char *nonce, const char *proof);
esp_err_t attention_pairing_process_reset_ack(void);
esp_err_t attention_pairing_complete_reset(const char *nonce, const char *proof);
esp_err_t attention_pairing_proof(const attention_pairing_record_t *record,
    const char *domain, const char *first, const char *second, char output[65]);
