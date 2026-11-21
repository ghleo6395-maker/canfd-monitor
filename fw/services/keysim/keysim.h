/* SPDX-License-Identifier: MIT */
/* 按键模拟服务：把 winsim 驱动包装成界面可以直接调用的非阻塞状态机。
 *
 *   - 选通道（自动标定串联电阻）、按键（点按 / 保持）、手动电压、全部松开；
 *   - 学习：电阻阵保持高阻，每 50 ms 测一次接口电压，喂给 winsim_learn_feed()；
 *   - 扫描：从 4800 mV 每 50 mV 往下设，每档停 30 ms 后读 APM 上报的开关状态，记下门限；
 *   - 空闲时每 300 ms 测一次接口电压，用来显示 APM 是否上电、检测过压。
 *
 * 板上没有第 12 页硬件时（当前的 Rev D/E 目标），服务编译成空壳：不分配约 33 KB 的驱动上下文，
 * keysim_available() 返回 false，界面不显示入口。定义 CANFD_WINSIM_HW 可在目标板上启用。
 * 键值表目前只保存在 RAM 里，重启后丢失；保存到 SD 卡待定。 */
#ifndef KEYSIM_H
#define KEYSIM_H

#include <stdbool.h>
#include <stdint.h>

#include "services/winsim/win_sim.h"

#define KS_MAX_KEYS   8
#define KS_MAX_EDGES  8
#define KS_NAME_LEN   16
#define KS_MANUAL     (-2)       /* pressed 的特殊值：手动电压 */
#define KS_SCAN_START_MV 4800u
#define KS_SCAN_STEP_MV  50u
#define KS_SCAN_HOLD_MS  30u

typedef enum { KS_IDLE, KS_PRESSING, KS_LEARNING, KS_SCANNING } ks_activity_t;
typedef enum { KS_TAP, KS_LATCH } ks_press_mode_t;   /* 点按 / 保持（再按一次松开） */

typedef struct {
    char     name[KS_NAME_LEN];
    uint16_t mv;                 /* 0 = 对地短路 */
    float    ohm;                /* 学习时反推的开关电阻；扫描得到的键为 NAN */
} ks_key_t;

typedef struct {
    bool     valid;
    char     name[24];
    uint8_t  n;
    ks_key_t keys[KS_MAX_KEYS];
} ks_table_t;

typedef struct {
    int16_t  from, to;           /* APM 开关状态 */
    uint16_t above_mv, below_mv; /* 状态在这两个电压之间切换 */
} ks_edge_t;

typedef struct {
    bool            available;
    uint8_t         channels;
    uint8_t         ch;
    bool            calibrated[WINSIM_NUM_CH];
    float           r_series[WINSIM_NUM_CH];
    uint16_t        port_mv;         /* 最近一次接口电压 */
    bool            supply_ok;       /* 最近一次空闲测量 ≥ min_open_mv */
    bool            ovp;
    ks_activity_t   activity;
    ks_press_mode_t press_mode;
    uint16_t        tap_ms;
    int8_t          pressed;         /* 键序号，-1 = 无，KS_MANUAL = 手动 */
    uint16_t        pressed_mv;
    uint16_t        manual_mv;
    winsim_status_t last_status;
    /* 学习 */
    uint16_t        learn_idle_mv;
    uint8_t         learn_n;
    uint8_t         learn_progress;  /* 0–100：当前档位稳定了多久 */
    ks_key_t        learn_keys[KS_MAX_KEYS];
    /* 扫描 */
    uint16_t        scan_mv;
    bool            scan_done;
    uint8_t         scan_n;
    ks_edge_t       scan_edges[KS_MAX_EDGES];
    /* 每路的键值表 */
    ks_table_t      tables[WINSIM_NUM_CH];
} keysim_t;

void            keysim_init(void);
void            keysim_tick(uint32_t ms);
const keysim_t *keysim_get(void);
bool            keysim_available(void);
bool            keysim_state_known(void);         /* 当前通道能读到 APM 开关状态 */
int             keysim_apm_state(void);           /* 当前通道 APM 开关状态，未知为 -1 */
const char     *keysim_state_name(int state);

winsim_status_t keysim_select(uint8_t ch);        /* 换通道并标定串联电阻 */
winsim_status_t keysim_recalibrate(void);
winsim_status_t keysim_press(int index);          /* 按 press_mode 按下第 index 个键；保持模式下再按一次松开 */
winsim_status_t keysim_manual_toggle(void);
void            keysim_manual_adjust(int delta_mv);
void            keysim_release(void);             /* 全部松开，回到高阻 */
void            keysim_set_press_mode(ks_press_mode_t mode);
winsim_status_t keysim_clear_ovp(void);
void            keysim_leave(void);               /* 离开按键模拟页：停止学习 / 扫描并松开 */

winsim_status_t keysim_learn_start(void);
void            keysim_learn_stop(void);
void            keysim_learn_rename(int index);   /* 在预设名称间循环 */
void            keysim_learn_clear(void);
bool            keysim_learn_save(void);

winsim_status_t keysim_scan_start(void);
void            keysim_scan_stop(void);
bool            keysim_scan_save(void);           /* 每档取中点存为键值表 */

#endif
