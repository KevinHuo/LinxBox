#ifndef LINX_ASSETS_H
#define LINX_ASSETS_H

#include "lvgl.h"

LV_FONT_DECLARE(linx_font_12);
LV_FONT_DECLARE(linx_font_14);
LV_FONT_DECLARE(linx_font_16);
LV_FONT_DECLARE(linx_font_20);
LV_FONT_DECLARE(linx_font_16_bold);
LV_FONT_DECLARE(linx_font_20_bold);
LV_FONT_DECLARE(linx_font_24_bold);
/* ASCII-only display size for counters; use a CJK variant for task names. */
LV_FONT_DECLARE(linx_font_28_bold);
/* Shared remote text: source CJK repertoire, Latin and replacement glyph. */
LV_FONT_DECLARE(linx_font_remote_16);

/* View 0: overview; 1 and 2: fixed upper/lower detail. IDs come from content. */
const lv_image_dsc_t *linx_asset_image(unsigned attachment_id, unsigned view);

#endif
