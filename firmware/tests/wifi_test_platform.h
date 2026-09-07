#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/time.h>
#include "audio_stubs/platform.h"
#define ESP_ERR_INVALID_STATE -3
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e = (expr); if (e) return e; } while (0)
#define BIT0 1U
#define WIFI_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
#define IP_EVENT_STA_LOST_IP 4
#define ESP_EVENT_ANY_ID -1
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_MODE_STA 1
#define WIFI_STORAGE_RAM 0
#define WIFI_IF_STA 0
#define SNTP_OPMODE_POLL 0
#define IPSTR "%u"
#define IP2STR(ip) (*(ip))
#define CONFIG_CODEX_ATTENTION_WIFI_SSID "12345678901234567890123456789012"
#define CONFIG_CODEX_ATTENTION_WIFI_PASSWORD "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
typedef unsigned EventBits_t;
typedef unsigned *EventGroupHandle_t;
typedef int esp_event_base_t;
typedef int esp_event_handler_instance_t;
typedef int wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() 0
typedef struct { struct { unsigned char ssid[32], password[64]; struct { int authmode; } threshold;
    struct { bool capable, required; } pmf_cfg; } sta; } wifi_config_t;
typedef struct { unsigned reason; } wifi_event_sta_disconnected_t;
typedef struct { struct { unsigned ip; } ip_info; } ip_event_got_ip_t;
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
static unsigned bits, connections, storage = 99;
static uint64_t retry_us;
static wifi_config_t applied;
static inline const char *esp_err_to_name(int e) { (void)e; return "test"; }
static inline int esp_wifi_connect(void) { ++connections; return 0; }
static inline int esp_timer_start_once(void *t, uint64_t delay) { (void)t; retry_us = delay; return 0; }
static inline int esp_timer_stop(void *t) { (void)t; retry_us = 0; return 0; }
static inline int esp_timer_create(const esp_timer_create_args_t *a, void **t) { (void)a; *t = &bits; return 0; }
static inline void esp_sntp_setoperatingmode(int a) { (void)a; }
static inline void esp_sntp_setservername(int a, const char *s) { (void)a; (void)s; }
static inline void esp_sntp_set_time_sync_notification_cb(void (*f)(struct timeval *)) { (void)f; }
static inline void esp_sntp_init(void) {}
static inline EventGroupHandle_t xEventGroupCreate(void) { return &bits; }
static inline unsigned xEventGroupClearBits(unsigned *e, unsigned b) { return *e &= ~b; }
static inline unsigned xEventGroupSetBits(unsigned *e, unsigned b) { return *e |= b; }
static inline unsigned xEventGroupGetBits(unsigned *e) { return *e; }
static inline unsigned xEventGroupWaitBits(unsigned *e, unsigned b, int c, int a, unsigned t) { (void)b;(void)c;(void)a;(void)t;return *e; }
static inline int esp_netif_init(void) { return 0; }
static inline int esp_event_loop_create_default(void) { return 0; }
static inline void *esp_netif_create_default_wifi_sta(void) { return &bits; }
static inline int esp_wifi_init(wifi_init_config_t *c) { (void)c; return 0; }
static inline int esp_event_handler_instance_register(int b, int i, void (*f)(void *, int, int32_t, void *), void *a, int *h) { (void)b;(void)i;(void)f;(void)a;(void)h;return 0; }
static inline int esp_wifi_set_mode(int m) { (void)m; return 0; }
static inline int esp_wifi_set_storage(int s) { storage = s; return 0; }
static inline int esp_wifi_set_config(int i, const wifi_config_t *c) { (void)i; applied = *c; return 0; }
static inline int esp_wifi_start(void) { return 0; }
// Keep log arguments compiled so the fixture also checks their types/usage.
static inline void wifi_test_log(const char *format, ...) { (void)format; }
#undef ESP_LOGI
#undef ESP_LOGW
#define ESP_LOGI(tag, ...) ((void)(tag), wifi_test_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), wifi_test_log(__VA_ARGS__))
