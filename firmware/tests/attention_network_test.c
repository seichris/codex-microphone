#include "attention_host_platform.h"
#include "../main/attention_pairing.h"
#include "../main/attention_connection.h"
#include "../main/attention_client.h"
#include <stdio.h>

static void phase(const char *name)
{
    puts(name); fflush(stdout); assert(getchar() == '\n');
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    attention_pairing_record_t record;
    FILE *input = fopen(argv[1], "rb"); assert(input);
    assert(fread(&record, 1, sizeof(record), input) == sizeof(record)); fclose(input);
    assert(attention_pairing_init() == ESP_OK && attention_pairing_install(&record) == ESP_OK);
    assert(attention_connection_init() == ESP_OK);
    mdns_txt_item_t txt[] = { { "id", record.bridge_id }, { "v", "1" }, { "tls", "1" } };
    uint8_t lengths[] = { 32, 1, 1 };
    mdns_ip_addr_t address = { .addr = { .type = ESP_IPADDR_TYPE_V4, .u_addr.ip4.addr = 0x0100007f } };
    host_mdns_result = (mdns_result_t){ .port = (uint16_t)record.port, .txt = txt, .txt_value_len = lengths, .txt_count = 3, .addr = &address };
    attention_snapshot_t snapshot;
    attention_desktop_state_t state;
    if (!strcmp(argv[2], "fallback")) host_mdns_available = false;
    if (!strcmp(argv[2], "wrong-certificate")) {
        assert(attention_client_fetch(&snapshot) == ATTENTION_ERR_TLS);
        assert(host_http_calls == 1);
        puts("PASS firmware refuses changed certificate"); return 0;
    }
    if (!strcmp(argv[2], "unknown-service")) {
        txt[0].value = "ffffffffffffffffffffffffffffffff";
        assert(attention_client_fetch(&snapshot) == ATTENTION_ERR_DISCOVERY);
        assert(host_http_calls == 0);
        puts("PASS firmware ignores unknown bridge advertisement"); return 0;
    }
    if (!strcmp(argv[2], "protocol-mismatch")) {
        txt[1].value = "2";
        assert(attention_client_fetch(&snapshot) == ATTENTION_ERR_DISCOVERY);
        assert(host_http_calls == 0);
        puts("PASS firmware rejects discovery protocol mismatch"); return 0;
    }
    assert(attention_client_fetch(&snapshot) == ESP_OK);
    assert(snapshot.count == 0 && !snapshot.current_thread.available); // not an authentication failure
    if (!strcmp(argv[2], "fallback")) { puts("PASS paired runtime hostname fallback"); return 0; }
    assert(attention_client_focus("019a-4", "firmware-focus", &state) == ESP_OK);
    assert(!strcmp(state.request_id, "firmware-focus"));
    assert(attention_client_voice("019a-4", "mute", "firmware-voice", &state) == ESP_OK);
    unsigned calls = host_http_calls;
    host_now_us += 286LL * 1000000;
    assert(attention_client_fetch(&snapshot) == ESP_OK);
    assert(host_http_calls == calls + 3); // refresh challenge, token, then GET
    phase("READY_FOR_RESTART");
    calls = host_http_calls;
    assert(attention_client_fetch(&snapshot) == ESP_OK);
    assert(host_http_calls == calls + 4); // one pre-execution 401, fresh auth, retry
    phase("READY_FOR_LOST_REPLY");
    calls = host_http_calls;
    assert(attention_client_focus("019a-4", "lost-reply", &state) == ATTENTION_ERR_UNAVAILABLE);
    assert(host_http_calls == calls + 1); // a possibly executed POST is never replayed
    assert(attention_client_fetch(&snapshot) == ESP_OK);
    phase("READY_FOR_ADDRESS_CHANGE");
    // The Mac moves from 127.0.0.1 to 127.0.0.2, preserving its TLS identity.
    address.addr.u_addr.ip4.addr = 0x0200007f;
    calls = host_http_calls;
    assert(attention_client_fetch(&snapshot) == ATTENTION_ERR_UNAVAILABLE);
    assert(host_http_calls == calls + 1);
    assert(attention_client_fetch(&snapshot) == ESP_OK); // fresh discovery, no re-pair
    host_wifi_connected = false;
    assert(attention_client_fetch(&snapshot) == ATTENTION_ERR_WIFI);
    host_wifi_connected = true;
    assert(attention_client_fetch(&snapshot) == ESP_OK);
    phase("READY_FOR_REVOKE");
    calls = host_http_calls;
    assert(attention_client_fetch(&snapshot) == ATTENTION_ERR_UNAUTHORIZED);
    assert(host_http_calls == calls + 1);
    assert(attention_connection_begin_reset() == ESP_OK);
    assert(attention_client_fetch(&snapshot) == ATTENTION_ERR_RESET_PENDING);
    attention_pairing_record_t cleared;
    assert(attention_pairing_copy(&cleared) == ESP_OK && cleared.state == ATTENTION_PAIRING_RESETTING);
    // The network task only enqueues an ACK; the INTERNAL-stack actor commits.
    assert(attention_pairing_process_reset_ack() == ESP_OK);
    assert(attention_pairing_copy(&cleared) == ESP_ERR_NOT_FOUND);
    calls = host_http_calls;
    assert(attention_client_fetch(&snapshot) == ATTENTION_ERR_UNPAIRED && calls == host_http_calls);
    puts("PASS production firmware HTTPS/auth/expiry/restart/discovery/reset lifecycle against the real Node bridge");
    return 0;
}
