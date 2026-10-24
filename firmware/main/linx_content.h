#ifndef LINX_CONTENT_H
#define LINX_CONTENT_H

#include <stddef.h>
#include <stdint.h>

/* IDs address read-only templates, never a row or a transient screen index. */
typedef enum {
    LINX_CONTENT_NONE = 0,
    LINX_CONTENT_LOGIN_INITIAL,
    LINX_CONTENT_LOGIN_FOLLOWUP,
    LINX_CONTENT_PAYMENT_INITIAL,
    LINX_CONTENT_PAYMENT_FOLLOWUP,
    LINX_CONTENT_DESIGN_INITIAL,
    LINX_CONTENT_DESIGN_FOLLOWUP,
    LINX_CONTENT_COUNT
} linx_content_id_t;

typedef struct {
    const char *summary;
    const char *body;
    const char *question;
} linx_content_t;

typedef struct {
    uint32_t id;
    const char *name;
    const char *description;
} linx_image_info_t;

#define LINX_IMAGE_TASK_LIST 1U
#define LINX_IMAGE_RESULT 2U
#define LINX_IMAGE_VOICE 3U
#define LINX_IMAGE_COUNT 3U

const linx_content_t *linx_content_get(linx_content_id_t id);
const linx_image_info_t *linx_content_image(uint32_t image_id);
const char *linx_content_initial_user(uint32_t task_id);
const char *linx_content_transcript(uint32_t task_id, uint32_t image_id);
/* Font generation can enumerate all model strings through these accessors. */
size_t linx_content_text_count(void);
const char *linx_content_text_at(size_t index);

#endif
