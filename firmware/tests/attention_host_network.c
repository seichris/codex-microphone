#include "attention_host_platform.h"
#include <curl/curl.h>
#include <stdio.h>

mdns_result_t host_mdns_result;
bool host_wifi_connected = true, host_mdns_available = true;
unsigned host_discoveries, host_http_calls;

struct host_http_client {
    esp_http_client_config_t config;
    struct curl_slist *headers;
    const char *body;
    int method, status;
};

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(config->transport_type == HTTP_TRANSPORT_OVER_SSL);
    assert(config->disable_auto_redirect && config->max_authorization_retries == -1);
    assert(config->cert_pem && config->common_name && config->timeout_ms <= 8000);
    esp_http_client_handle_t client = calloc(1, sizeof(*client));
    assert(client); client->config = *config; return client;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, int method) { client->method = method; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    char header[1024]; assert(strlen(key) + strlen(value) + 3 < sizeof(header));
    snprintf(header, sizeof(header), "%s: %s", key, value);
    client->headers = curl_slist_append(client->headers, header); assert(client->headers); return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *body, int length)
{
    assert((int)strlen(body) == length); client->body = body; return ESP_OK;
}
static size_t received(char *data, size_t size, size_t count, void *context)
{
    esp_http_client_handle_t client = context;
    esp_http_client_event_t event = { .event_id = HTTP_EVENT_ON_DATA, .user_data = client->config.user_data,
        .data = data, .data_len = (int)(size * count) };
    return client->config.event_handler(&event) == ESP_OK ? size * count : 0;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    ++host_http_calls;
    CURL *curl = curl_easy_init(); assert(curl);
    const esp_http_client_config_t *config = &client->config;
    char url[1024], mapping[512];
    // Check the expected TLS identity even when discovery supplied a different
    // IP. This adapter replaces only the ESP transport, not auth/state logic.
    snprintf(url, sizeof(url), "https://%s:%d%s", config->common_name, config->port, config->path);
    snprintf(mapping, sizeof(mapping), "%s:%d:%s:%d", config->common_name, config->port, config->host, config->port);
    struct curl_slist *connect_to = curl_slist_append(NULL, mapping);
    struct curl_blob ca = { .data = (void *)config->cert_pem, .len = strlen(config->cert_pem), .flags = CURL_BLOB_COPY };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_TO, connect_to);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)config->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, client->headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, received);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, client);
    if (client->method == HTTP_METHOD_POST) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, client->body);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status); client->status = (int)status;
    int tls_error = result == CURLE_PEER_FAILED_VERIFICATION || result == CURLE_SSL_CONNECT_ERROR ? (int)result : 0;
    esp_http_client_event_t event = { .event_id = HTTP_EVENT_DISCONNECTED, .data = &tls_error, .user_data = config->user_data };
    (void)config->event_handler(&event);
    curl_easy_cleanup(curl); curl_slist_free_all(connect_to);
    return result == CURLE_OK ? ESP_OK : ESP_FAIL;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { return client->status; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { curl_slist_free_all(client->headers); free(client); return ESP_OK; }
esp_err_t esp_tls_get_and_clear_last_error(void *handle, int *error, int *flags)
{
    *error = *(int *)handle; *flags = 0; return *error;
}
esp_err_t mdns_init(void) { return ESP_OK; }
esp_err_t mdns_query_ptr(const char *service, const char *proto, uint32_t timeout, size_t max, mdns_result_t **results)
{
    assert(!strcmp(service, "_codex-attention") && !strcmp(proto, "_tcp") && timeout <= 2000 && max <= 8);
    ++host_discoveries; *results = host_mdns_available ? &host_mdns_result : NULL; return ESP_OK;
}
void mdns_query_results_free(mdns_result_t *results) { (void)results; }
bool wifi_manager_is_connected(void) { return host_wifi_connected; }
