#pragma once
#include "esp_err.h"
esp_err_t attention_connection_begin_reset(void);

#define ATTENTION_ERR_UNAUTHORIZED 0x20001
#define ATTENTION_ERR_UNPAIRED 0x20010
#define ATTENTION_ERR_STORAGE 0x20011
#define ATTENTION_ERR_WIFI 0x20012
#define ATTENTION_ERR_DISCOVERY 0x20013
#define ATTENTION_ERR_TLS 0x20014
#define ATTENTION_ERR_UNAVAILABLE 0x20015
#define ATTENTION_ERR_RESET_PENDING 0x20016

esp_err_t attention_connection_init(void);
// Serialized, bounded, paired HTTPS only. Caller owns *json on success.
esp_err_t attention_connection_request(const char *path, const char *body, char **json);
// Return a bounded, secret-free UI diagnostic (never a URL or credential).
const char *attention_connection_error(esp_err_t error);
