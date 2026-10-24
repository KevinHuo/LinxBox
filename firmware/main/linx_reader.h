#ifndef LINX_READER_H
#define LINX_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linx_navigation.h"

#define LINX_READER_ROWS 12U
#define LINX_READER_LINE_HEIGHT 22U
#define LINX_READER_PARAGRAPH_GAP 8U
#define LINX_READER_LIST_INDENT 12U

typedef enum {
    LINX_READER_BODY = 0,
    LINX_READER_HEADING,
    LINX_READER_LIST
} linx_reader_kind_t;

/* Heading uses the actual bold font; other rows use the regular body font. */
typedef int32_t (*linx_reader_measure_fn)(const char *text, size_t bytes,
                                         linx_reader_kind_t kind, void *context);

typedef struct {
    size_t start;
    size_t length;
    /* A first list row may draw this original marker, or replace it with a dot. */
    size_t marker_start;
    size_t marker_length;
    /* Bullets hang at 12px; numbers reserve their measured marker width +2px. */
    uint16_t x;
    uint16_t y;
    linx_reader_kind_t kind;
    bool first_list_line;
} linx_reader_row_t;

typedef struct {
    /* Contiguous original source interval, including omitted blanks/markers. */
    size_t source_start;
    size_t source_end;
    size_t count;
    linx_reader_row_t rows[LINX_READER_ROWS];
} linx_reader_page_t;

typedef struct {
    size_t offsets[LINX_PAGE_LIMIT + 1U];
    size_t count;
    uint16_t width;
    uint16_t height;
    bool complete;
} linx_reader_layout_t;

/* Fixed storage, no heap. Empty source produces one empty page. False means
 * unsupported geometry, source longer than LINX_TEXT_LIMIT, or >64 pages. */
bool linx_reader_paginate(linx_reader_layout_t *layout, const char *text,
                          uint16_t width, uint16_t height,
                          linx_reader_measure_fn measure, void *context);

/* Replays the small bounded source, so only one page of row storage is needed.
 * Use the same source/font metrics as paginate. Rows contain UTF-8 byte spans,
 * never copied text; all x/y coordinates are relative to the content viewport. */
bool linx_reader_get_page(const linx_reader_layout_t *layout, size_t page,
                          const char *text, linx_reader_measure_fn measure,
                          void *context, linx_reader_page_t *out);

#endif
