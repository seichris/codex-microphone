#include "attention_pairing.h"
#include <string.h>

void attention_pairing_zero(void *data, size_t length)
{
    volatile unsigned char *bytes = data;
    while (length--) *bytes++ = 0;
}

bool attention_pairing_hex(const char *text, size_t length)
{
    if (text == NULL || strnlen(text, length + 1) != length) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    }
    return true;
}

bool attention_pairing_hostname(const char *text)
{
    if (text == NULL) return false;
    const size_t length = strnlen(text, 254);
    if (length > 253) return false;
    size_t label = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '.') {
            if (label == 0 || text[i - 1] == '-') return false;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || (c == '-' && label != 0))) return false;
            if (++label > 63) return false;
        }
    }
    return length == 0 || (label != 0 && text[length - 1] != '-');
}

bool attention_pairing_record_valid(const attention_pairing_record_t *r)
{
    if (r == NULL || r->magic != ATTENTION_PAIRING_MAGIC || r->version != ATTENTION_PAIRING_VERSION) return false;
    if (r->state == ATTENTION_PAIRING_EMPTY) {
        const unsigned char *bytes = (const unsigned char *)r;
        for (size_t i = offsetof(attention_pairing_record_t, generation); i < offsetof(attention_pairing_record_t, seal); ++i)
            if (bytes[i] != 0) return false;
        return true;
    }
    if (r->state != ATTENTION_PAIRING_ACTIVE && r->state != ATTENTION_PAIRING_RESETTING) return false;
    if (r->generation == 0 || r->port == 0 || r->port > 65535 || r->provisioned_at < 1735689600U
        || !attention_pairing_hex(r->device_id, 32) || !attention_pairing_hex(r->bridge_id, 32)
        || !attention_pairing_hex(r->secret, 64) || !attention_pairing_hostname(r->fallback_host)) return false;
    const size_t ssid_length = strnlen(r->ssid, sizeof(r->ssid));
    const size_t password_length = strnlen(r->password, sizeof(r->password));
    const size_t certificate_length = strnlen(r->certificate, sizeof(r->certificate));
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64
        || (password_length > 0 && password_length < 8)
        || certificate_length == 0 || certificate_length >= sizeof(r->certificate)
        || strncmp(r->certificate, "-----BEGIN CERTIFICATE-----\n", 28) != 0
        || strstr(r->certificate, "-----END CERTIFICATE-----") == NULL) return false;
    if (password_length == 64) {
        for (size_t i = 0; i < 64; ++i) {
            const char c = r->password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        }
    }
    return r->state == ATTENTION_PAIRING_RESETTING ? attention_pairing_hex(r->reset_nonce, 64) : r->reset_nonce[0] == 0;
}

bool attention_discovery_txt_matches(const char *bridge_id, const char *const *keys,
    const char *const *values, size_t count)
{
    if (!attention_pairing_hex(bridge_id, 32) || count > 16 || keys == NULL || values == NULL) return false;
    unsigned found = 0;
    for (size_t i = 0; i < count; ++i) {
        if (keys[i] == NULL || values[i] == NULL) return false;
        unsigned flag = 0;
        const char *expected = NULL;
        if (strcmp(keys[i], "id") == 0) { flag = 1; expected = bridge_id; }
        else if (strcmp(keys[i], "v") == 0) { flag = 2; expected = "1"; }
        else if (strcmp(keys[i], "tls") == 0) { flag = 4; expected = "1"; }
        if (flag && ((found & flag) || strcmp(values[i], expected) != 0)) return false;
        found |= flag;
    }
    return found == 7;
}
