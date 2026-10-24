#ifndef LINX_NAVIGATION_H
#define LINX_NAVIGATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINX_PAGE_LIMIT 64U
#define LINX_TEXT_LIMIT 4096U

/* The caller measures the actual display font, including kerning. */
typedef int32_t (*linx_text_width_fn)(const char *text, size_t bytes, void *context);

typedef struct {
    size_t end;
    size_t next;
} linx_text_line_t;

/* Wrap one nonempty UTF-8 span without copying or dropping source bytes.
 * end excludes an explicit newline; next includes it. Both are source offsets. */
bool linx_wrap_span(const char *text, size_t start, size_t limit, int32_t width,
                    linx_text_width_fn measure, void *context, linx_text_line_t *line);

typedef struct {
    size_t offsets[LINX_PAGE_LIMIT + 1U];
    size_t count;
    bool complete;
} linx_pagination_t;

bool linx_paginate(linx_pagination_t *pages, const char *text, int32_t width,
                   size_t lines, linx_text_width_fn measure, void *context);
/* Returns false rather than emitting a partial UTF-8 character or clipped page. */
bool linx_page_text(const linx_pagination_t *pages, size_t page, const char *text,
                    int32_t width, linx_text_width_fn measure, void *context,
                    char *out, size_t capacity);
size_t linx_menu_step(size_t selected, int direction, const bool *enabled, size_t count);

#endif
