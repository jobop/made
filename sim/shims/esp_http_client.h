/* sim/shims/esp_http_client.h — host 桩：
 * 覆盖 VibeCoding::request() 和 vibe_theme 拉取用到的字段与函数。
 * perform() 永远返回 ESP_FAIL（无网络），UI 走「连接电脑」分支。 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_http_client *esp_http_client_handle_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *user_data;
    char *data;
    int data_len;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *event);
typedef esp_err_t (*esp_http_crt_bundle_attach_t)(void *conf);

typedef enum {
    HTTP_METHOD_GET = 0, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD, HTTP_METHOD_PATCH,
} esp_http_client_method_t;

typedef struct {
    const char *url;
    const char *host;
    const char *username;
    const char *password;
    const char *path;
    const char *query;
    const char *cert_pem;
    const char *client_cert_pem;
    const char *client_key_pem;
    esp_http_client_method_t method;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    int max_authorization_retries;
    bool disable_auto_redirect;
    unsigned max_redirects;
    http_event_handle_cb event_handler;
    void *user_data;
    esp_http_crt_bundle_attach_t crt_bundle_attach;
    bool keep_alive_enable;
    bool disable_keepalive;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_get_content_length(esp_http_client_handle_t client);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
const char *esp_http_client_get_url(esp_http_client_handle_t client);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif
