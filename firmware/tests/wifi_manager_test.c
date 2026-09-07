#include <assert.h>
#include <stdio.h>
#include "wifi_test_platform.h"
#include "../main/wifi_manager.c"

int main(void)
{
    assert(wifi_manager_start() == ESP_OK);
    assert(memcmp(applied.sta.ssid, CONFIG_CODEX_ATTENTION_WIFI_SSID, 32) == 0);
    assert(memcmp(applied.sta.password, CONFIG_CODEX_ATTENTION_WIFI_PASSWORD, 64) == 0);
    assert(storage == WIFI_STORAGE_RAM);
    on_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    assert(connections == 1 && !wifi_manager_is_connected());
    wifi_event_sta_disconnected_t failure = { .reason = 202 };
    for (unsigned i = 0; i < 8; ++i) {
        on_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &failure);
        unsigned delay = 1000U << i;
        if (delay > 30000) delay = 30000;
        assert(retry_us == (uint64_t)delay * 1000);
        assert(!wifi_manager_is_connected() && connections == 1);
    }
    reconnect(NULL);
    assert(connections == 2);
    ip_event_got_ip_t ip = { .ip_info = { .ip = 1 } };
    on_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(wifi_manager_wait_connected(0) && retry_us == 0);
    on_event(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    assert(!wifi_manager_wait_connected(0));
    on_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    assert(wifi_manager_wait_connected(0));
    on_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &failure);
    assert(retry_us == 1000000 && !wifi_manager_is_connected());
    puts("PASS full-width Wi-Fi credentials, RAM storage, bounded backoff and DHCP recovery");
}
