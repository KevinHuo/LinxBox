#pragma once

typedef struct _lv_obj_t { int identifier; } lv_obj_t;
struct _lv_display_t { int identifier; };
lv_obj_t *lv_screen_active(void);
