#ifndef LINX_SDK_STUBS_H
#define LINX_SDK_STUBS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define CONFIG_LINX_WIFI_SSID test_ssid
#define CONFIG_LINX_WIFI_PASSWORD test_password
#define CONFIG_LWIP_SNTP_MAX_SERVERS 3
#ifdef LINX_GATEWAY_TRANSPORT_TEST
#define CONFIG_LINX_GATEWAY_ENABLE test_gateway_enable
#define CONFIG_LINX_GATEWAY_ALLOW_WS test_gateway_allow_ws
#define CONFIG_LINX_GATEWAY_URL test_gateway_url
#define CONFIG_LINX_GATEWAY_TOKEN test_gateway_token
extern bool test_gateway_enable;
extern bool test_gateway_allow_ws;
extern const char *test_gateway_url;
extern const char *test_gateway_token;
#endif
extern const char *test_ssid;
extern const char *test_password;

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef struct {unsigned depth;} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void test_task_enter(portMUX_TYPE *lock);
void test_task_exit(portMUX_TYPE *lock);
#define taskENTER_CRITICAL(lock) test_task_enter(lock)
#define taskEXIT_CRITICAL(lock) test_task_exit(lock)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY UINT32_MAX
typedef struct test_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t size);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *data);
BaseType_t xQueueReceive(QueueHandle_t queue, void *data, TickType_t wait);
BaseType_t xTaskCreate(void (*function)(void *), const char *name, uint32_t stack, void *user, UBaseType_t priority, void *handle);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);

typedef const char *esp_event_base_t;
#define ESP_EVENT_DEFINE_BASE(name) esp_event_base_t name = #name;
extern esp_event_base_t WIFI_EVENT;
extern esp_event_base_t IP_EVENT;
#define ESP_EVENT_ANY_ID (-1)
#define IP_EVENT_STA_GOT_IP 2
#define WIFI_EVENT_STA_DISCONNECTED 5
#define WIFI_EVENT_SCAN_DONE 6
#define WIFI_EVENT_STA_CONNECTED 7
typedef struct {uint8_t ssid[32]; uint8_t ssid_len;} wifi_event_sta_connected_t;
typedef struct {uint8_t ssid[32]; uint8_t ssid_len;} wifi_event_sta_disconnected_t;
typedef struct {uint32_t status; uint8_t number; uint8_t scan_id;} wifi_event_sta_scan_done_t;
typedef void *esp_event_handler_instance_t;
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t size, TickType_t wait);
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_loop_delete_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *user,
                    esp_event_handler_instance_t *instance);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id, esp_event_handler_instance_t instance);

typedef struct { int value; } esp_netif_t;
typedef struct { int value; } esp_netif_config_t;
#define ESP_NETIF_DEFAULT_WIFI_STA() ((esp_netif_config_t){0})
#define ESP_NETIF_DEFAULT_WIFI_AP() ((esp_netif_config_t){1})
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key);
esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif);
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *netif);
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void);
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
void esp_netif_destroy_default_wifi(void *netif);

typedef struct { int value; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct { struct { uint8_t ssid[32]; uint8_t password[64]; } sta;
    struct { uint8_t ssid[32]; uint8_t password[64]; uint8_t ssid_len; int authmode; uint8_t max_connection; } ap; } wifi_config_t;
typedef enum {WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WAPI_PSK, WIFI_AUTH_OWE, WIFI_AUTH_MAX} wifi_auth_mode_t;
typedef struct { uint8_t ssid[33]; int8_t rssi; wifi_auth_mode_t authmode; } wifi_ap_record_t;
typedef struct {bool show_hidden;} wifi_scan_config_t;
#define WIFI_STORAGE_FLASH 1
#define WIFI_STORAGE_RAM 2
#define WIFI_MODE_STA 1
#define WIFI_MODE_APSTA 3
#define WIFI_IF_AP 1
#define WIFI_IF_STA 0
#define ESP_MAC_WIFI_STA 0
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_get_config(int interface, wifi_config_t *config);
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *records);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_read_mac(uint8_t *mac, int type);
void esp_fill_random(void *buffer, size_t size);
int64_t esp_timer_get_time(void);

typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_flash_init(void);
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
#define ESP_SNTP_OPMODE_POLL 0
bool esp_sntp_enabled(void);
void esp_sntp_setoperatingmode(int mode);
void esp_sntp_setservername(unsigned index, const char *name);
void esp_sntp_init(void);
void esp_sntp_stop(void);

typedef struct { char version[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
int esp_app_get_elf_sha256(char *out, size_t size);
esp_err_t esp_crt_bundle_attach(void *config);
typedef struct test_http *esp_http_client_handle_t;
typedef struct { int event_id; int data_len; void *data; void *user_data; } esp_http_client_event_t;
#define HTTP_EVENT_ON_DATA 3
#define HTTP_METHOD_POST 1
typedef struct {
    const char *url;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void *);
    bool disable_auto_redirect;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, int method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *body, int size);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

typedef struct test_transport *esp_transport_handle_t;
typedef struct test_tls_error *esp_tls_error_handle_t;
esp_tls_error_handle_t esp_transport_get_error_handle(esp_transport_handle_t transport);
int esp_transport_get_errno(esp_transport_handle_t transport);
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t error, int *code, int *flags);
typedef enum {WS_TRANSPORT_OPCODES_CONT = 0, WS_TRANSPORT_OPCODES_TEXT = 1,
    WS_TRANSPORT_OPCODES_BINARY = 2, WS_TRANSPORT_OPCODES_CLOSE = 8,
    WS_TRANSPORT_OPCODES_PING = 9, WS_TRANSPORT_OPCODES_PONG = 10,
    WS_TRANSPORT_OPCODES_FIN = 128, WS_TRANSPORT_OPCODES_NONE = 256} ws_transport_opcodes_t;
typedef struct { const char *ws_path; const char *sub_protocol; const char *user_agent;
    const char *headers; const char *auth; bool propagate_control_frames; } esp_transport_ws_config_t;
esp_transport_handle_t esp_transport_ssl_init(void);
esp_transport_handle_t esp_transport_tcp_init(void);
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t transport, esp_err_t (*attach)(void *));
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent);
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t transport, const esp_transport_ws_config_t *config);
int esp_transport_connect(esp_transport_handle_t transport, const char *host, int port, int timeout);
int esp_transport_poll_read(esp_transport_handle_t transport, int timeout);
int esp_transport_read(esp_transport_handle_t transport, char *buffer, int size, int timeout);
int esp_transport_close(esp_transport_handle_t transport);
esp_err_t esp_transport_destroy(esp_transport_handle_t transport);
int esp_transport_ws_send_raw(esp_transport_handle_t transport, ws_transport_opcodes_t opcode, const char *buffer, int size, int timeout);
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t transport);
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t transport);
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t transport);
typedef int esp_log_level_t;
#define ESP_LOG_NONE 0
#ifndef ESP_LOGI
#define ESP_LOGI(...) ((void)0)
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(...) ((void)0)
#endif
esp_log_level_t esp_log_level_get(const char *tag);
void esp_log_level_set(const char *tag, esp_log_level_t level);

typedef int esp_audio_err_t;
#define ESP_AUDIO_ERR_OK 0
#define ESP_AUDIO_ERR_MEM_LACK (-2)
#define ESP_OPUS_DEC_FRAME_DURATION_120_MS 8
typedef struct {uint32_t sample_rate; uint8_t channel; int frame_duration; bool self_delimited;} esp_opus_dec_cfg_t;
typedef struct {uint8_t *buffer; uint32_t len; uint32_t consumed; int frame_recover;} esp_audio_dec_in_raw_t;
typedef struct {uint8_t *buffer; uint32_t len; uint32_t needed_size; uint32_t decoded_size;} esp_audio_dec_out_frame_t;
typedef struct {uint32_t sample_rate; uint8_t bits_per_sample; uint8_t channel; uint32_t bitrate; uint32_t frame_size;} esp_audio_dec_info_t;
esp_audio_err_t esp_opus_dec_open(void *config, uint32_t size, void **handle);
esp_audio_err_t esp_opus_dec_decode(void *handle, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame, esp_audio_dec_info_t *info);
esp_audio_err_t esp_opus_dec_reset(void *handle);
esp_audio_err_t esp_opus_dec_close(void *handle);
esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_set_format(uint32_t rate, uint8_t bits, uint8_t channels);
esp_err_t bsp_audio_wake(void);
esp_err_t bsp_audio_sleep(void);
void bsp_audio_set_volume(uint8_t volume);
esp_err_t bsp_audio_read(void *data, size_t size);
esp_err_t bsp_audio_write(const void *data, size_t size);
#endif
