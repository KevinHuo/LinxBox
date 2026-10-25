#ifndef LINX_GATEWAY_CLIENT_H
#define LINX_GATEWAY_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINX_GATEWAY_ID_CAPACITY 513U
#define LINX_GATEWAY_FRAME_MAX 65536U
#define LINX_GATEWAY_TITLE_CAPACITY 1025U
#define LINX_GATEWAY_PROJECT_CAPACITY 513U
#define LINX_GATEWAY_AGENT_CAPACITY 129U
#define LINX_GATEWAY_SUMMARY_CAPACITY 1025U
#define LINX_GATEWAY_CONTENT_CAPACITY 1025U
#define LINX_GATEWAY_TEXT_MAX_BYTES 16384U
#define LINX_GATEWAY_SESSION_MAX 2U
#define LINX_GATEWAY_HISTORY_MAX 2U
#define LINX_GATEWAY_NAV_MAX 4U
#define LINX_GATEWAY_BLOCK_MAX 16U
#define LINX_GATEWAY_TIMEOUT_MS 10000U

typedef enum {
    LINX_GATEWAY_REQUEST_NONE,
    LINX_GATEWAY_REQUEST_AWAITING_TEXT,
    LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION,
    LINX_GATEWAY_REQUEST_SENDING,
    LINX_GATEWAY_REQUEST_RUNNING,
    LINX_GATEWAY_REQUEST_COMPLETED,
    LINX_GATEWAY_REQUEST_FAILED,
    LINX_GATEWAY_REQUEST_CANCELLED,
    LINX_GATEWAY_REQUEST_UNKNOWN
} linx_gateway_request_status_t;

typedef enum {
    LINX_GATEWAY_ERROR_NONE, LINX_GATEWAY_ERROR_UNCONFIGURED,
    LINX_GATEWAY_ERROR_DISCONNECTED, LINX_GATEWAY_ERROR_INVALID,
    LINX_GATEWAY_ERROR_CAPACITY, LINX_GATEWAY_ERROR_MEMORY,
    LINX_GATEWAY_ERROR_TIMEOUT, LINX_GATEWAY_ERROR_BUSY,
    LINX_GATEWAY_ERROR_REMOTE, LINX_GATEWAY_ERROR_NAVIGATION, LINX_GATEWAY_ERROR_TRANSPORT
} linx_gateway_error_t;

typedef struct {
    char agent_session_id[LINX_GATEWAY_ID_CAPACITY];
    char title[LINX_GATEWAY_TITLE_CAPACITY];
    char project_name[LINX_GATEWAY_PROJECT_CAPACITY];
    char agent[LINX_GATEWAY_AGENT_CAPACITY];
    char status[16];
} linx_gateway_session_t;

typedef struct {
    char message_id[LINX_GATEWAY_ID_CAPACITY];
    char role[10];
} linx_gateway_message_t;

typedef struct {
    bool valid;
    char request_id[LINX_GATEWAY_ID_CAPACITY];
    char agent_session_id[LINX_GATEWAY_ID_CAPACITY];
    char target_title[LINX_GATEWAY_TITLE_CAPACITY];
    linx_gateway_request_status_t status;
    bool text_present;
    bool uncertain;
    bool capture_pending;
    bool capture_claimed;
    bool cancel_requested;
    bool cancel_sent;
    char error_code[32];
    char error_message[LINX_GATEWAY_SUMMARY_CAPACITY];
} linx_gateway_request_t;

typedef struct {
    bool configured, connected, bound, loading;
    uint32_t generation, revision;
    char device_id[18];
    char desktop_client_id[LINX_GATEWAY_ID_CAPACITY];
    linx_gateway_error_t error;
    char error_code[32];
    char error_message[LINX_GATEWAY_SUMMARY_CAPACITY];
    linx_gateway_session_t sessions[LINX_GATEWAY_SESSION_MAX];
    size_t session_count;
    bool sessions_omitted; /* 本批只作前两项预览；next_cursor仍指向下一服务端批。 */
    char sessions_next_cursor[LINX_GATEWAY_ID_CAPACITY];
    char selected_session_id[LINX_GATEWAY_ID_CAPACITY];
    linx_gateway_session_t detail;
    bool detail_valid, target_unavailable, new_reply;
    linx_gateway_message_t messages[LINX_GATEWAY_HISTORY_MAX];
    size_t message_count;
    bool messages_omitted; /* 本批预览可省略后续轮次，不声称本地完整历史分页。 */
    char history_next_cursor[LINX_GATEWAY_ID_CAPACITY];
    char history_session_id[LINX_GATEWAY_ID_CAPACITY];
    char content_session_id[LINX_GATEWAY_ID_CAPACITY];
    char content_message_id[LINX_GATEWAY_ID_CAPACITY];
    char content[LINX_GATEWAY_CONTENT_CAPACITY];
    uint32_t content_offset, content_next_offset;
    bool content_valid, content_has_next;
    linx_gateway_request_t request;
} linx_gateway_view_t;

typedef bool (*linx_gateway_send_fn)(uint32_t generation, const char *json,
                                     size_t size, void *user);
typedef struct {
    bool configured;
    uint64_t instance_id; /* 每次进程/设备启动生成的非零随机值；重连保持。 */
    linx_gateway_send_fn send;
    void *user;
} linx_gateway_client_config_t;

typedef enum {
    LINX_GATEWAY_ACTION_REFRESH,
    LINX_GATEWAY_ACTION_SESSIONS_NEXT,
    LINX_GATEWAY_ACTION_SESSIONS_PREVIOUS,
    LINX_GATEWAY_ACTION_SELECT,
    LINX_GATEWAY_ACTION_HISTORY,
    LINX_GATEWAY_ACTION_HISTORY_NEXT,
    LINX_GATEWAY_ACTION_HISTORY_PREVIOUS,
    LINX_GATEWAY_ACTION_CONTENT,
    LINX_GATEWAY_ACTION_CONTENT_NEXT,
    LINX_GATEWAY_ACTION_CONTENT_PREVIOUS,
    LINX_GATEWAY_ACTION_PREPARE,
    LINX_GATEWAY_ACTION_CANCEL,
    LINX_GATEWAY_ACTION_REQUEST_GET
} linx_gateway_action_t;

/* UI 主循环独占。除 init 外所有调用可递增 view.revision；不得跨任务读写。 */
typedef struct {
    linx_gateway_view_t view;
    linx_gateway_client_config_t config;
    struct {
        unsigned operation;
        char id[64];
        char session[LINX_GATEWAY_ID_CAPACITY];
        char message[LINX_GATEWAY_ID_CAPACITY];
        char request[LINX_GATEWAY_ID_CAPACITY];
        char cursor[LINX_GATEWAY_ID_CAPACITY];
        uint32_t offset, generation, sent_at;
    } pending;
    struct {
        unsigned operation;
        char session[LINX_GATEWAY_ID_CAPACITY];
        char message[LINX_GATEWAY_ID_CAPACITY];
        char cursor[LINX_GATEWAY_ID_CAPACITY];
        uint32_t offset;
    } intent;
    char sessions_cursor[LINX_GATEWAY_NAV_MAX][LINX_GATEWAY_ID_CAPACITY];
    char history_cursor[LINX_GATEWAY_NAV_MAX][LINX_GATEWAY_ID_CAPACITY];
    size_t sessions_depth, history_depth;
    uint32_t block_offsets[LINX_GATEWAY_BLOCK_MAX];
    size_t block_count, block_index;
    uint32_t sequence;
    unsigned refresh_turn;
    bool hello_needed, recovery_needed, sessions_dirty, detail_dirty, history_dirty;
    bool prepare_sent, prepare_rejected;
} linx_gateway_client_t;

/* init accepts uninitialized storage; deinit an initialized client before reusing it. */
void linx_gateway_client_init(linx_gateway_client_t *client,
                              const linx_gateway_client_config_t *config);
void linx_gateway_client_deinit(linx_gateway_client_t *client);
void linx_gateway_client_connected(linx_gateway_client_t *client, uint32_t generation,
                                   const char *mac, uint32_t now);
void linx_gateway_client_disconnected(linx_gateway_client_t *client, uint32_t generation);
void linx_gateway_client_transport_error(linx_gateway_client_t *client, uint32_t generation);
bool linx_gateway_client_receive(linx_gateway_client_t *client, uint32_t generation,
                                 const char *bytes, size_t size, uint32_t now);
void linx_gateway_client_tick(linx_gateway_client_t *client, uint32_t now);
/* SELECT/PREPARE/HISTORY 的 target 为 session ID；CONTENT 的 target 为 message ID。
 * 其余动作 target=NULL。动作只登记有界意图；tick/receive 自动调度。 */
bool linx_gateway_client_action(linx_gateway_client_t *client, linx_gateway_action_t action,
                                const char *target, uint32_t now);
const linx_gateway_view_t *linx_gateway_client_view(const linx_gateway_client_t *client);
/* 仅原始 prepare 首次成功可领取一次。get、重连、语音事件不会重新授予。 */
bool linx_gateway_client_claim_capture(linx_gateway_client_t *client);
/* 清理离开的显示页。history=true 同时丢弃历史和游标；保留列表身份、目标和活动请求。 */
void linx_gateway_client_drop_transient(linx_gateway_client_t *client, bool history);
/* 背景查询不阻止正文导航；仅已有正文查询/意图时阻止重复跨块。 */
bool linx_gateway_client_content_pending(const linx_gateway_client_t *client);
/* 同一严格解析器供共享契约 fixtures 一致性测试使用。 */
bool linx_gateway_client_validate_json(const char *definition, const char *bytes, size_t size);

#endif
