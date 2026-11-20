#ifndef REVD_UI_LVGL_H
#define REVD_UI_LVGL_H
#include <stdint.h>
#include "lvgl.h"
/* Fixed native views over shared UI actions: dual CAN send, typed keyboard,
 * file loading/naming and six-row live tabs. Widgets are reused on refresh. */
void ui_lvgl_init(lv_display_t *display);
void ui_lvgl_task(uint32_t now_ms);
uint32_t ui_lvgl_snapshot_errors(void);
#endif
