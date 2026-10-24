#ifndef LINX_TEST_ESP_HTTP_SERVER_H
#define LINX_TEST_ESP_HTTP_SERVER_H

#include "sdk_stubs.h"
#include <sys/types.h>

typedef void *httpd_handle_t;
typedef struct httpd_req {
    void *user_ctx;
    size_t content_len;
    void *test_data;
} httpd_req_t;
typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *request);
    void *user_ctx;
} httpd_uri_t;
typedef struct {
    unsigned stack_size;
    unsigned max_open_sockets;
    unsigned max_uri_handlers;
    unsigned max_resp_headers;
    unsigned recv_wait_timeout;
    unsigned send_wait_timeout;
    bool lru_purge_enable;
    void *global_user_ctx;
    void (*global_user_ctx_free_fn)(void *context);
    esp_err_t (*open_fn)(httpd_handle_t server, int socket);
    void (*close_fn)(httpd_handle_t server, int socket);
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTP_GET 1
#define HTTP_POST 3
#define HTTPD_RESP_USE_STRLEN (-1)
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
void *httpd_get_global_user_ctx(httpd_handle_t handle);
size_t httpd_req_get_hdr_value_len(httpd_req_t *request, const char *name);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *value, size_t size);
int httpd_req_recv(httpd_req_t *request, char *buffer, size_t size);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t size);

#endif
