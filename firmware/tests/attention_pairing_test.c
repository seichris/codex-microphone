#include "attention_host_platform.h"
#include "../main/attention_pairing.c"
#include "../main/attention_confirmation.h"
#include "../main/attention_connection.h"
#include <stdio.h>

static void reboot(void)
{
    if (s_lock != NULL) free(s_lock);
    s_lock = NULL;
    s_ready = false;
    memset(&s_record, 0, sizeof(s_record));
}

static attention_pairing_record_t sample(const char *certificate_path)
{
    attention_pairing_record_t r = { .magic = ATTENTION_PAIRING_MAGIC, .version = 1, .state = ATTENTION_PAIRING_ACTIVE,
        .generation = 1, .port = 5182, .provisioned_at = 1800000000 };
    strcpy(r.device_id, "11111111111111111111111111111111");
    strcpy(r.bridge_id, "22222222222222222222222222222222");
    memset(r.secret, 'a', 64);
    strcpy(r.ssid, "Runtime Wi-Fi"); strcpy(r.password, "test-only-password");
    strcpy(r.fallback_host, "paired-mac.local");
    FILE *input = fopen(certificate_path, "rb"); assert(input);
    const size_t size = fread(r.certificate, 1, sizeof(r.certificate) - 1, input); fclose(input); assert(size > 0);
    assert(attention_pairing_record_valid(&r));
    return r;
}

static void confirmation(void)
{
    attention_confirmation_t s;
    attention_confirmation_begin(&s, 100);
    attention_confirmation_update(&s, 5000, true, false);
    assert(s.phase == ATTENTION_CONFIRM_RELEASE); // pre-existing hold is not consent
    attention_confirmation_update(&s, 5100, false, false);
    attention_confirmation_update(&s, 5149, false, false); assert(s.phase == ATTENTION_CONFIRM_RELEASE);
    attention_confirmation_update(&s, 5150, false, false); assert(s.phase == ATTENTION_CONFIRM_PRESS);
    attention_confirmation_update(&s, 5200, true, false);
    attention_confirmation_update(&s, 6699, true, false); assert(s.phase == ATTENTION_CONFIRM_HOLD);
    attention_confirmation_update(&s, 6700, true, false); assert(s.phase == ATTENTION_CONFIRM_ACCEPTED);
    attention_confirmation_begin(&s, 7000); attention_confirmation_update(&s, 7001, false, true);
    assert(s.phase == ATTENTION_CONFIRM_CANCELLED);
    attention_confirmation_begin(&s, 8000); attention_confirmation_update(&s, 68000, true, false);
    assert(s.phase == ATTENTION_CONFIRM_EXPIRED);
    attention_confirmation_update(&s, 70000, true, false); assert(s.phase == ATTENTION_CONFIRM_EXPIRED);
}

static void discovery(void)
{
    const char *id = "22222222222222222222222222222222";
    const char *keys[] = { "tls", "v", "id" }, *values[] = { "1", "1", id };
    assert(attention_discovery_txt_matches(id, keys, values, 3));
    assert(!attention_discovery_txt_matches(id, keys, values, 2));
    values[0] = "0"; assert(!attention_discovery_txt_matches(id, keys, values, 3)); values[0] = "1";
    values[1] = "2"; assert(!attention_discovery_txt_matches(id, keys, values, 3)); values[1] = "1";
    values[2] = "11111111111111111111111111111111"; assert(!attention_discovery_txt_matches(id, keys, values, 3));
    const char *duplicates[] = { "id", "id", "tls" }; assert(!attention_discovery_txt_matches(id, duplicates, values, 3));
    assert(attention_pairing_hostname("")); assert(attention_pairing_hostname("new-address.local"));
    assert(!attention_pairing_hostname("http://paired.local")); assert(!attention_pairing_hostname("host..local"));
    assert(!attention_pairing_hostname("-host.local")); assert(!attention_pairing_hostname("host-.local"));
    assert(!attention_pairing_hostname("user@host")); assert(!attention_pairing_hostname("host/path"));
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    confirmation(); discovery();
    attention_pairing_record_t record = sample(argv[1]), copy;
    host_key_available = false;
    assert(attention_pairing_init() != ESP_OK && !attention_pairing_storage_ready());
    assert(host_nvs_writes == 0); // missing eFuse does not generate or erase anything
    reboot(); host_key_available = true;
    assert(attention_pairing_init() == ESP_OK);
    assert(attention_pairing_copy(&copy) == ESP_ERR_NOT_FOUND);
    host_fail_commit = true;
    assert(attention_pairing_install(&record) != ESP_OK);
    assert(!attention_pairing_storage_ready());
    assert(attention_pairing_copy(&copy) == ESP_ERR_INVALID_STATE);
    reboot(); host_fail_commit = false;
    assert(attention_pairing_init() == ESP_OK && attention_pairing_copy(&copy) == ESP_ERR_NOT_FOUND);
    assert(attention_pairing_install(&record) == ESP_OK);
    assert(attention_pairing_install(&record) == ESP_ERR_INVALID_STATE); // explicit reset required
    reboot(); assert(attention_pairing_init() == ESP_OK && attention_pairing_copy(&copy) == ESP_OK);
    assert(!strcmp(copy.ssid, record.ssid) && !strcmp(copy.secret, record.secret));
    assert(host_wall_time == record.provisioned_at);
    char first[65], second[65], mac[65]; memset(first, 'b', 64); first[64] = 0; memset(second, 'c', 64); second[64] = 0;
    assert(attention_pairing_proof(&record, "auth", first, second, mac) == ESP_OK);
    assert(!strcmp(mac, argv[2])); // cross-language canonical HMAC fixture
    host_fail_write = true;
    assert(attention_pairing_begin_reset() != ESP_OK);
    assert(!attention_pairing_storage_ready());
    host_fail_write = false;
    reboot(); assert(attention_pairing_init() == ESP_OK);
    assert(attention_pairing_begin_reset() == ESP_OK);
    reboot(); assert(attention_pairing_init() == ESP_OK && attention_pairing_copy(&copy) == ESP_OK);
    assert(copy.state == ATTENTION_PAIRING_RESETTING);
    assert(attention_pairing_install(&record) != ESP_OK);
    assert(attention_pairing_complete_reset(first, second) != ESP_OK);
    assert(attention_pairing_proof(&copy, "revoked", copy.reset_nonce, NULL, mac) == ESP_OK);
    host_fail_commit = true;
    assert(attention_pairing_complete_reset(copy.reset_nonce, mac) != ESP_OK);
    reboot(); host_fail_commit = false;
    assert(attention_pairing_init() == ESP_OK && attention_pairing_copy(&copy) == ESP_OK && copy.state == ATTENTION_PAIRING_RESETTING);
    assert(attention_pairing_complete_reset(copy.reset_nonce, mac) == ESP_OK);
    reboot(); assert(attention_pairing_init() == ESP_OK && attention_pairing_copy(&copy) == ESP_ERR_NOT_FOUND);
    assert(!copy.secret[0] && !copy.certificate[0] && !copy.device_id[0]);
    assert(attention_pairing_install(&record) == ESP_OK);
    unsigned char saved[sizeof(record)]; memcpy(saved, host_flash, sizeof(saved));
    const unsigned writes = host_nvs_writes;
    host_flash[100] ^= 1;
    reboot(); assert(attention_pairing_init() != ESP_OK && !attention_pairing_storage_ready());
    assert(host_nvs_writes == writes);
    memcpy(host_flash, saved, sizeof(saved)); host_flash_size--;
    reboot(); assert(attention_pairing_init() != ESP_OK);
    memcpy(host_flash, saved, sizeof(saved)); host_flash_size = sizeof(saved);
    ((attention_pairing_record_t *)host_flash)->version = 2;
    reboot(); assert(attention_pairing_init() != ESP_OK); assert(host_nvs_writes == writes);
    assert(strcmp(attention_connection_error(ATTENTION_ERR_TLS), attention_connection_error(ATTENTION_ERR_UNAUTHORIZED)));
    assert(strcmp(attention_connection_error(ATTENTION_ERR_UNPAIRED), attention_connection_error(ATTENTION_ERR_WIFI)));
    puts("PASS production pairing storage, physical confirmation, corruption, reset/ack recovery, discovery and canonical HMAC");
    return 0;
}
