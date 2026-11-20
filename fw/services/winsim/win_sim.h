/* SPDX-License-Identifier: MIT */
/*
 * 万能车窗按键模拟器（APM 售后测试扩展模块）核心驱动。
 *
 * 依据 apm.md，按 Hardware-C 原理图第 12 页调整：
 *   - 电阻阵 12 位（apm.md 为 8 位），控制字为 uint16_t，查找表 4096 项（16 KB）；
 *   - 开关由 TCA9539 + BSS138DW 实现，HAL 名称沿用 apm.md，
 *     硬件映射见 win_sim_tca9539.h；
 *   - 回读分压 1:4，取在 PTC 外侧（接口侧），默认负载 400 kΩ。
 *
 * C99，无动态内存，适用于裸机和 RTOS。同一个 ctx 不能被并发调用；
 * ctx 约 16.5 KB，请放在静态存储区，不要放在栈上。
 */
#ifndef WIN_SIM_H
#define WIN_SIM_H

#include <stdbool.h>
#include <stdint.h>

#define WINSIM_NUM_BITS    12u
#define WINSIM_LUT_SIZE    (1u << WINSIM_NUM_BITS)
#define WINSIM_MASK_ALL    ((uint16_t)(WINSIM_LUT_SIZE - 1u))
#define WINSIM_ADC_SAMPLES 5u   /* 每次回读取 5 次中值 */

typedef enum {
    WINSIM_OK = 0,
    WINSIM_ERR_PARAM,
    WINSIM_ERR_OVERVOLTAGE,   /* 接口电压 > ovp_threshold_mv（如误接 12 V），已锁定高阻 */
    WINSIM_ERR_NO_SUPPLY,     /* 空载电压 < min_open_mv（APM 未唤醒或未接） */
    WINSIM_ERR_VERIFY_FAIL    /* 回读与目标（或理论值）偏差超过 verify_tol_mv */
} winsim_status_t;

/* 硬件抽象。名称沿用 apm.md；控制字放宽到 12 位。 */
typedef struct {
    void     (*shift_write)(uint16_t mask);   /* 写 12 位电阻阵控制字，整体同步生效 */
    void     (*set_oe)(bool enable);          /* false：全部开关强制关断；true：按已写入的值输出 */
    void     (*set_short_0r)(bool enable);    /* 0 Ω 对地直通 */
    uint16_t (*adc_read_mv)(void);            /* 接口侧电压，已按分压比折算，单位 mV */
    void     (*delay_ms)(uint32_t ms);
} winsim_hal_t;

typedef struct {
    float    r_bit_ohm[WINSIM_NUM_BITS]; /* 电阻阵各位阻值；默认为标称值，可填实测值提高精度 */
    float    r_pullup_ohm;        /* APM 内部上拉 R295，默认 2200 */
    float    r_adc_load_ohm;      /* 回读分压总阻值 R815+R816，默认 400000；0 表示忽略 */
    float    mos_rds_on_ohm;      /* 电阻阵开关管导通电阻，默认 5.0（BSS138 @ Vgs 3.3 V） */
    float    short_rds_on_ohm;    /* 直通管导通电阻，默认 0.05（AO3400A） */
    float    r_series_ohm;        /* PTC 等串联电阻，默认 0，实测后填写 */
    uint16_t ovp_threshold_mv;    /* 默认 5500 */
    uint16_t min_open_mv;         /* 默认 4000 */
    uint16_t settle_delay_ms;     /* 默认 10 */
    uint16_t verify_tol_mv;       /* 默认 80 */
    bool     verify_enable;       /* 默认 true；VCC2_5V_SW 为脉冲供电的车型设为 false */
} winsim_cfg_t;

typedef struct {
    uint16_t mask;            /* 12 位控制字（apm.md 中的 mask_595） */
    bool     short_0r;
    float    actual_r_ohm;    /* 接口对地等效电阻，不含回读负载；高阻时为 INFINITY */
    uint16_t target_mv;       /* 本次目标电压；按阻值设置时为 0 */
    uint16_t expected_mv;     /* 理论接口电压；未标定空载电压时为 0 */
    uint16_t measured_mv;     /* 回读电压 */
} winsim_state_t;

typedef struct {
    winsim_hal_t   hal;
    winsim_cfg_t   cfg;
    float          r_lut[WINSIM_LUT_SIZE];  /* r_lut[0] = INFINITY */
    uint16_t       last_open_mv;            /* 最近一次有效的空载电压，0 表示未标定 */
    winsim_state_t current_state;
    bool           initialized;
    bool           ovp_latched;
} winsim_ctx_t;

/* 扫描回调。回调里不要再调用本模块的函数。 */
typedef void (*winsim_sweep_cb_t)(const winsim_state_t *state, void *user_ctx);

winsim_cfg_t winsim_default_cfg(void);

/* 安全初始化：set_oe(false) → set_short_0r(false) → shift_write(0) → set_oe(true)，
 * 然后建查找表。cfg 为 NULL 时使用默认配置。 */
winsim_status_t winsim_init(winsim_ctx_t *ctx, const winsim_hal_t *hal, const winsim_cfg_t *cfg);

/* 关闭直通、清零控制字，接口恢复高阻。 */
winsim_status_t winsim_set_hiz(winsim_ctx_t *ctx);

/* 切高阻、等待稳定、取中值读空载电压。超过 OVP 门限时锁定高阻。 */
winsim_status_t winsim_probe_open_voltage(winsim_ctx_t *ctx, uint16_t *v_open_mv);

/* 按阻值设置。target_ohm 为模拟的按键对地电阻（不含 400 kΩ 回读负载）；
 * 0 = 直通，INFINITY = 高阻。不做回读校验。out_state 可为 NULL。 */
winsim_status_t winsim_set_resistance(winsim_ctx_t *ctx, float target_ohm, winsim_state_t *out_state);

/* 按目标电压设置。未标定时先自动探测空载电压；
 * verify_enable 时检查 |回读 − 目标| ≤ verify_tol_mv。 */
winsim_status_t winsim_set_voltage(winsim_ctx_t *ctx, uint16_t target_mv, winsim_state_t *out_state);

/* 按分压比设置，ratio = 接口电压 / APM 内部电源，范围 [0, 1]；
 * 0 = 直通，1 = 高阻。不依赖电源绝对值。 */
winsim_status_t winsim_set_ratio(winsim_ctx_t *ctx, float ratio_0_to_1, winsim_state_t *out_state);

/* 自动扫描：先重新探测空载电压，再从 start_mv 到 end_mv（升序或降序）按 step_mv 逐档输出。
 * 每档稳定并回读后等待 hold_ms，再调用 on_step。步数 = |end − start| / step + 1。
 * 每档回读与理论值比较（不是与目标比较，避免 0–220 mV 无档位区间误报）。
 * 正常结束或出错时都恢复高阻。 */
winsim_status_t winsim_auto_sweep(winsim_ctx_t *ctx, uint16_t start_mv, uint16_t end_mv,
                                  uint16_t step_mv, uint16_t hold_ms,
                                  winsim_sweep_cb_t on_step, void *user_ctx);

/* 解除过压锁定：在高阻下重新测量，低于门限才解除。 */
winsim_status_t winsim_clear_ovp(winsim_ctx_t *ctx);

const char *winsim_status_str(winsim_status_t status);

#endif /* WIN_SIM_H */
