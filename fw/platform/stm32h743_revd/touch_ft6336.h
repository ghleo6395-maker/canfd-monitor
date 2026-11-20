#ifndef REVD_TOUCH_FT6336_H
#define REVD_TOUCH_FT6336_H
#include <stdbool.h>
#include <stdint.h>

bool touch_ft6336_init(void);
/* false means transfer/coordinate error, *pressed is always cleared on failure. */
bool touch_ft6336_read(uint16_t *x, uint16_t *y, bool *pressed);
void touch_ft6336_deinit(void);
uint8_t touch_ft6336_chip_id(void);
uint32_t touch_ft6336_errors(void);
#ifdef CANFD_REVE_TARGET
/* Foreground only; returns current presence. Never call after display poweroff. */
bool touch_ft6336_service(uint32_t now_ms);
#endif
#endif
