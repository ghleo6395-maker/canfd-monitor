/* 两颗 MCU 可控 LED 的状态编码（需求 F8.7）：
 *  LED_REC ：常亮 = 录制中；1 Hz 闪 = 录制中出现过丢帧；快闪 3 次 = 没卡 / SD 异常时按了 REC
 *  LED_STAT：每 2 s 短闪 = 正常；双闪 = 有通道错误被动或 Bus-Off；慢闪（1 s 亮 1 s 灭）= 低电 */
#ifndef LED_H
#define LED_H

#include "services/core/app_types.h"

void led_init(void);
void led_handle(const app_evt_t *e);
void led_tick(void);
bool led_rec_on(void);
bool led_stat_on(void);
const char *led_stat_pattern(void);
const char *led_rec_pattern(void);

#endif
