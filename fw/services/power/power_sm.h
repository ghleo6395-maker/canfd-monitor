/* 电源 / 屏幕 / 按键锁状态机（需求 3.1 按键约定、F8.2、F9.x，框架 8.7 开关机时序）。
 *
 *        按下 POWER（硬件上电，立刻拉高 PE2）
 *   OFF ─────────────────────────────▶ BOOT_HOLD ──松手 < 1 s 且没按组合键：误触，断电──▶ OFF
 *    ▲                                    │ 满 1 s
 *    │                                    ├─ 按着 BACK+REC ──▶ BOOT_COMBO ──满 3 s──▶ BOOTLOADER
 *    │                                    ▼                         │ 中途松开
 *    │                                STARTING（CHG_HI → 5V → 等稳定） ◀─┘
 *    │                                    ▼
 *    │                                 RUNNING：屏幕 亮/灭、按键锁、自动熄屏、自动关机倒计时、低电保护
 *    │                                    │ 长按 POWER 2 s / 低电 / 自动关机 / 上位机
 *    │      等 POWER 松开后断开 PE2         ▼
 *    └─────────────────────────────── SHUTDOWN（停录制并关文件 → 关 CAN、屏幕、5V → 等松键 → 断电） */
#ifndef POWER_SM_H
#define POWER_SM_H

#include "services/core/app_types.h"

typedef enum {
    PWR_OFF, PWR_BOOT_HOLD, PWR_BOOT_COMBO, PWR_STARTING, PWR_RUNNING, PWR_SHUTDOWN, PWR_BOOTLOADER, PWR_STATE_NUM
} pwr_state_t;

typedef enum { SH_WAIT_SERVICES, SH_WAIT_KEY } shut_step_t;

typedef struct {
    pwr_state_t       state;
    bool              screen_on;
    bool              locked;
    uint8_t           countdown_s;    /* 自动关机倒计时，0 = 没在倒计时 */
    shutdown_reason_t reason;         /* 最近一次关机原因 */
    shut_step_t       shstep;
    uint32_t          shwait_ms;
    uint16_t          batt_mv;
    bool              ext_power;
    bool              low_warned;
    bool              host_connected;
    bool              eng_boot;       /* 开机时按着 OK+BACK */
    bool              no_screen;      /* 检测不到屏幕：无屏模式（F8.7） */
    uint32_t          boot_count;
} power_t;

void           power_init(void);
bool           power_raw_key(key_id_t k, bool down);   /* 关机 / 开机阶段的按键；返回 true 表示已处理 */
void           power_handle(const app_evt_t *e);
void           power_activity(void);                   /* 有按键 / 触摸：重置熄屏和自动关机计时 */
void           power_wake(void);
const power_t *power_get(void);
bool           power_running(void);
const char    *power_state_name(pwr_state_t s);

#endif
