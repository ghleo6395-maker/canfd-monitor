/* 按键手势识别：原始按下/松开 → 短按 / 长按 / 连发 / 组合键。
 * 规则（需求 3.1）：
 *  - 长按门限：OK、BACK 1 s；REC、POWER 2 s。方向键按住 0.5 s 后每 150 ms 连发。
 *  - 多个键同时按下时，这些键都不产生短按 / 长按（避免误触发）。
 *  - ←+→ 同时按住 2 s = 按键锁定 / 解锁。
 *  - 开机后 POWER 要先松开一次才生效（armed）。
 *  - 被"消耗"的按键（熄屏时用来唤醒的那一下）不产生任何手势。 */
#ifndef KEYS_H
#define KEYS_H

#include "services/core/app_types.h"

void keys_reset(void); /* 开机时调用：POWER 未 armed */
void keys_cancel(key_id_t key); /* Clear a stale virtual press without a gesture. */
void keys_raw(key_id_t k, bool down, bool consume);
void keys_tick(void);                        /* 每个节拍调用：长按、连发、组合键 */
bool keys_is_down(key_id_t k);
bool keys_power_armed(void);

#endif
