/* SPDX-License-Identifier: MIT */
/*
 * 万能车窗按键模拟器（APM 售后测试扩展模块）核心驱动。
 *
 * 思路：APM 只认它引脚上的电压。学习时只记电压（空闲和各档），发出时用电阻把引脚拉到同样的电压。
 * 按 Hardware-C 原理图第 12 页第 3 版：
 *   - 电阻阵 15 位（b0–b12 在 U801，b13 = 47 Ω、b14 = 150 kΩ 在 U808），4 路 K1–K4 共用，
 *     同一时刻只接通一路（硬件译码保证）；没有查找表，每次直接遍历 32767 种组合求解；
 *   - 每路先辨识 APM 一侧的等效电源（开路电压 V0、内阻 Rth，都含 356 kΩ 回读负载）：
 *     高阻、0 Ω 直通、一个已知电阻各量一次，同时解出这一路的串联电阻（PTC + 继电器）。
 *     之后按 V = V0 × R / (R + Rth) 求电阻，再按回读微调。不依赖 APM 的上拉阻值和电压；
 *   - 每路一个空闲保持支路（光耦继电器 + 2.2 kΩ + AD5292 50 kΩ），4 路可以同时保持各自的空闲电平；
 *     建立空闲时只用这条支路本身测量，接口电压始终在开路和空闲之间，不会被 APM 当成按键；
 *   - 回读取在各路接口侧，300k / 56k 分压（÷ 6.36，量程约 21 V），负载 356 kΩ；能识别脉冲供电的上拉，只取接通期间的读数。
 *   硬件映射见 win_sim_tca9539.h。
 *
 * C99，无动态内存，适用于裸机和 RTOS。同一个 ctx 不能被并发调用。
 */
#ifndef WIN_SIM_H
#define WIN_SIM_H

#include <stdbool.h>
#include <stdint.h>

#define WINSIM_NUM_BITS      15u
#define WINSIM_NUM_MASKS     (1u << WINSIM_NUM_BITS)
#define WINSIM_MASK_ALL      ((uint16_t)(WINSIM_NUM_MASKS - 1u))
#define WINSIM_NUM_CH        4u    /* K1–K4 */
#define WINSIM_ADC_SAMPLES   5u    /* 普通回读取 5 次中值 */
#define WINSIM_PULSE_SAMPLES 24u   /* 判断脉冲上拉：每 1 ms 一次，共 24 次 */
#define WINSIM_IDLE_CODES    1024u /* AD5292 */

typedef enum {
    WINSIM_OK = 0,
    WINSIM_ERR_PARAM,
    WINSIM_ERR_OVERVOLTAGE,   /* 接口电压 > ovp_threshold_mv（如误接 12 V），已锁定高阻并断开空闲支路 */
    WINSIM_ERR_NO_SUPPLY,     /* 空载电压 < min_open_mv（APM 未唤醒或未接） */
    WINSIM_ERR_VERIFY_FAIL,   /* 回读与目标（或理论值）偏差超过 verify_tol_mv */
    WINSIM_ERR_CALIBRATION,   /* 辨识结果不合理：串联电阻超出 [0, r_series_max_ohm]（如 PTC 断开）或内阻无解 */
    WINSIM_ERR_RANGE,         /* 空闲电阻超出电位器范围，或电流会超过 idle_max_ua */
    WINSIM_ERR_UNSUPPORTED    /* 板上没有空闲保持硬件（HAL 未提供） */
} winsim_status_t;

/* 硬件抽象。前五项名称沿用 apm.md；最后三项是第 3 版的空闲保持，没有这部分硬件时填 NULL。 */
typedef struct {
    void     (*shift_write)(uint16_t mask);   /* 写 15 位电阻阵控制字 */
    void     (*set_oe)(bool enable);          /* false：全部开关、继电器强制关断；true：按已写入的值输出 */
    void     (*set_short_0r)(bool enable);    /* 0 Ω 对地直通 */
    uint16_t (*adc_read_mv)(void);            /* 所选通道的接口电压，已按分压比折算，单位 mV */
    void     (*delay_ms)(uint32_t ms);
    void     (*select_channel)(uint8_t ch);   /* 0–3 对应 K1–K4；只改通道地址，不碰电阻阵 */
    void     (*idle_power)(bool on);          /* 电位器 VDD 升压（WS_VPOT） */
    void     (*idle_write)(uint8_t ch, uint16_t code);   /* 第 ch 路电位器 RDAC，0–1023 */
    void     (*idle_connect)(uint8_t ch, bool on);       /* 第 ch 路空闲继电器 */
} winsim_hal_t;

typedef struct {
    float    r_bit_ohm[WINSIM_NUM_BITS]; /* 电阻阵各位阻值；默认为标称值，可填实测值提高精度 */
    float    r_pullup_ohm;        /* 辨识之前假定的 APM 上拉，默认 2200（参考 APM 的 R295） */
    float    r_adc_load_ohm;      /* 回读分压总阻值 R815–R818 之一 + R819，默认 356000；0 表示忽略 */
    float    mos_rds_on_ohm;      /* 电阻阵开关管导通电阻，默认 5.0（BSS138 @ Vgs 3.3 V） */
    float    short_rds_on_ohm;    /* 直通管导通电阻，默认 0.05（AO3400A） */
    float    r_series_ohm;        /* 未标定通道使用的串联电阻（PTC + 继电器），默认 10 */
    float    r_series_max_ohm;    /* 标定结果的上限，超过视为故障，默认 100 */
    float    idle_pot_ohm;        /* 空闲电位器满量程，默认 50000（AD5292BRUZ-50） */
    float    idle_series_ohm;     /* 空闲支路固定电阻：R827–R830 + 继电器，默认 2202 */
    float    idle_wiper_ohm;      /* 电位器滑臂电阻，默认 80 */
    uint16_t idle_max_ua;         /* 空闲支路电流上限，默认 1800（AD5292 50 kΩ 款持续 ±2 mA） */
    uint16_t idle_open_margin_mv; /* 学到的空闲电压比开路电压低不到这么多，就认为空闲是开路，默认 30 */
    uint16_t idle_power_ms;       /* 打开升压后的等待，默认 5 */
    uint16_t ovp_threshold_mv;    /* 默认 5500；上拉到蓄电池电压的车型调到 16000（量程约 21 V） */
    uint16_t min_open_mv;         /* 默认 2000：低于它认为 APM 没上电（3.3 V 上拉的 APM 也够用） */
    uint16_t settle_delay_ms;     /* 默认 10 */
    uint16_t channel_switch_ms;   /* 换通道后的等待，默认 5（TLP176AM 接通 ≤ 3 ms） */
    uint16_t verify_tol_mv;       /* 默认 80 */
    uint16_t trim_tol_mv;         /* 回读与目标差超过它才微调，默认 6 */
    uint8_t  trim_steps;          /* 最多微调几次，默认 2；0 = 不微调 */
    bool     verify_enable;       /* 默认 true */
    uint16_t pulse_detect_mv;     /* 24 次采样的最大最小差超过它、且最小值低于最大值一半，算脉冲上拉，默认 300 */
    uint8_t  num_channels;        /* 板上实际通道数，1–4，默认 4 */
    uint16_t learn_drop_mv;       /* 学习：低于空闲电压这么多才算按下，默认 150 */
    uint16_t learn_stable_mv;     /* 学习：相邻采样差在此以内算稳定，默认 25 */
    uint16_t learn_stable_ms;     /* 学习：稳定这么久才记录，默认 500 */
} winsim_cfg_t;

/* 一路 APM 一侧的等效电源（不含本板的空闲支路，含 356 kΩ 回读负载）。 */
typedef struct {
    float v0_mv;     /* 开路电压 */
    float rth_ohm;   /* 等效内阻 */
    bool  ok;        /* 已经辨识或由空闲支路测出 */
    bool  pulsed;    /* 上拉是脉冲供电：回读只取接通期间 */
} winsim_source_t;

/* 一路的空闲保持支路。 */
typedef struct {
    bool     on;           /* 继电器接通 */
    uint16_t code;         /* 电位器码值 */
    uint16_t target_mv;    /* 要保持的空闲电压 */
    uint16_t measured_mv;  /* 建立后的回读 */
    float    r_ohm;        /* 支路总电阻 */
} winsim_idle_t;

typedef struct {
    uint8_t  channel;         /* 0–3 */
    uint16_t mask;            /* 15 位控制字 */
    bool     short_0r;
    float    actual_r_ohm;    /* 电阻阵这一侧对地电阻（含串联电阻，不含回读负载和空闲支路）；高阻时为 INFINITY */
    uint16_t target_mv;       /* 本次目标电压；按阻值设置时为 0 */
    uint16_t expected_mv;     /* 理论接口电压；没有电源模型时为 0 */
    uint16_t measured_mv;     /* 回读电压 */
    uint8_t  trims;           /* 本次微调了几次 */
} winsim_state_t;

typedef struct {
    winsim_hal_t    hal;
    winsim_cfg_t    cfg;
    float           g_bit[WINSIM_NUM_BITS];   /* 各位电导（含开关管） */
    float           r_series_ch[WINSIM_NUM_CH];
    bool            series_cal[WINSIM_NUM_CH];
    winsim_source_t src[WINSIM_NUM_CH];
    winsim_idle_t   idle[WINSIM_NUM_CH];
    bool            idle_powered;
    uint8_t         channel;
    uint16_t        last_open_mv;            /* 当前通道最近一次电阻阵高阻时的接口电压，0 表示未测 */
    winsim_state_t  current_state;
    bool            initialized;
    bool            ovp_latched;
} winsim_ctx_t;

/* 扫描回调。回调里不要再调用本模块的函数。 */
typedef void (*winsim_sweep_cb_t)(const winsim_state_t *state, void *user_ctx);

winsim_cfg_t winsim_default_cfg(void);

/* 安全初始化：set_oe(false) → set_short_0r(false) → shift_write(0) → select_channel(0) → set_oe(true)，
 * 有空闲保持硬件时再断开 4 路空闲继电器、关升压。cfg 为 NULL 时使用默认配置。 */
winsim_status_t winsim_init(winsim_ctx_t *ctx, const winsim_hal_t *hal, const winsim_cfg_t *cfg);

/* 关闭直通、清零控制字，电阻阵恢复高阻。空闲保持不受影响。 */
winsim_status_t winsim_set_hiz(winsim_ctx_t *ctx);

/* 换到通道 ch（0–3）：先把电阻阵切高阻，再改通道地址，等 channel_switch_ms。
 * calibrate 为 true 时接着辨识这一路（winsim_identify），串联电阻写入 r_series_out（可为 NULL）。
 * 过压锁定不会因换通道解除；换到正常的通道后调用 winsim_clear_ovp()。 */
winsim_status_t winsim_select_channel(winsim_ctx_t *ctx, uint8_t ch, bool calibrate, float *r_series_out);

/* 辨识当前通道：高阻量开路电压（同时判断是否脉冲上拉），接 0 Ω 直通量一次，
 * 再接一个阻值接近内阻的组合量一次，解出 APM 一侧的 V0、Rth 和这一路的串联电阻。
 * 直通和已知电阻各接通约 settle_delay_ms。这一路空闲支路接着时照常测，结果里扣掉它。 */
winsim_status_t winsim_identify(winsim_ctx_t *ctx, winsim_source_t *out, float *r_series_out);

/* 兼容第 2 版的名字：等同 winsim_identify，只返回串联电阻。 */
winsim_status_t winsim_calibrate_series(winsim_ctx_t *ctx, float *r_series_out);

/* 切高阻、等待稳定，判断是否脉冲上拉并读空载电压。超过 OVP 门限时锁定高阻。 */
winsim_status_t winsim_probe_open_voltage(winsim_ctx_t *ctx, uint16_t *v_open_mv);

/* 被动测量：电阻阵保持高阻（若正在下拉则先断开并等待稳定），读当前接口电压。
 * 原车开关并在这一路时，读到的就是开关当前档位的电压。超过 OVP 门限时锁定。 */
winsim_status_t winsim_measure(winsim_ctx_t *ctx, uint16_t *mv);

/* 由被动测量的电压反推外部开关对地电阻。需要先有电源模型（辨识或空载电压）；否则返回 NAN。
 * mv ≥ 开路电压时返回 INFINITY，mv 为 0 时返回 0。 */
float winsim_equiv_ohm(const winsim_ctx_t *ctx, uint16_t mv);

/* 电阻阵某个控制字的阻值（不含串联电阻）；0 为 INFINITY。 */
float winsim_mask_ohm(const winsim_ctx_t *ctx, uint16_t mask);

/* 按阻值设置。target_ohm 为电阻阵这一侧的对地电阻（含串联电阻）；0 = 直通，INFINITY = 高阻。不做回读校验。 */
winsim_status_t winsim_set_resistance(winsim_ctx_t *ctx, float target_ohm, winsim_state_t *out_state);

/* 按目标电压设置。没有电源模型时先探测空载电压（按配置的上拉估算内阻）；
 * verify_enable 时按回读微调最多 trim_steps 次，最后检查 |回读 − 目标| ≤ verify_tol_mv。 */
winsim_status_t winsim_set_voltage(winsim_ctx_t *ctx, uint16_t target_mv, winsim_state_t *out_state);

/* 按分压比设置，ratio = 接口电压 / 开路电压，范围 [0, 1]；0 = 直通，1 = 高阻。不做回读校验。 */
winsim_status_t winsim_set_ratio(winsim_ctx_t *ctx, float ratio_0_to_1, winsim_state_t *out_state);

/* 自动扫描当前通道：先重新探测空载电压，再从 start_mv 到 end_mv（升序或降序）按 step_mv 逐档输出。
 * 每档稳定并回读后等待 hold_ms，再调用 on_step。步数 = |end − start| / step + 1。
 * 每档回读与理论值比较（不是与目标比较，避免无档位区间误报），不微调。
 * 正常结束或出错时都恢复高阻。 */
winsim_status_t winsim_auto_sweep(winsim_ctx_t *ctx, uint16_t start_mv, uint16_t end_mv,
                                  uint16_t step_mv, uint16_t hold_ms,
                                  winsim_sweep_cb_t on_step, void *user_ctx);

/* 解除过压锁定：在高阻下重新测量当前通道，低于门限才解除。空闲支路在过压时已全部断开，需要重新建立。 */
winsim_status_t winsim_clear_ovp(winsim_ctx_t *ctx);

/* 在第 ch 路建立空闲保持，使电阻阵高阻时接口电压为 idle_mv。会换到第 ch 路（不辨识）。
 *   - 先断开这一路的空闲支路量开路电压 V0；idle_mv ≥ V0 − idle_open_margin_mv 时空闲就是开路
 *     （原车开关还接着，或者空闲本来就是开路），支路保持断开，返回 OK；
 *   - 否则把电位器放到最大阻值接通，用这一点算出 APM 一侧的内阻，再算码值、按回读微调。
 *     整个过程接口电压都在 V0 和 idle_mv 之间，不会出现比空闲更低的电压；
 *   - 所需电阻超出电位器范围、或电流会超过 idle_max_ua 时返回 WINSIM_ERR_RANGE，支路保持断开。
 * 结果写入 out（可为 NULL）。 */
winsim_status_t winsim_idle_hold(winsim_ctx_t *ctx, uint8_t ch, uint16_t idle_mv, winsim_idle_t *out);

/* 断开第 ch 路的空闲支路；4 路都断开时关升压。 */
winsim_status_t winsim_idle_release(winsim_ctx_t *ctx, uint8_t ch);

/* 板上有没有空闲保持硬件。 */
bool winsim_idle_supported(const winsim_ctx_t *ctx);

const char *winsim_status_str(winsim_status_t status);

/* ------------------------------------------------------------------ 按键学习
 * 纯算法，不碰硬件：调用方周期性地用 winsim_measure() 采样，把结果喂进来。
 * 规则：电压低于 idle − learn_drop_mv 算按下；相邻采样差 ≤ learn_stable_mv 持续
 * learn_stable_ms 后记录一档（取稳定段平均值）；之后必须回到空闲附近才能记录下一档。 */
typedef struct {
    uint16_t idle_mv;
    uint16_t drop_mv, stable_mv, stable_ms;
    bool     armed;
    bool     have_last;
    uint16_t last_mv;
    uint32_t stable_for_ms;
    uint32_t sum_mv;
    uint32_t n;
} winsim_learn_t;

void winsim_learn_begin(winsim_learn_t *l, const winsim_cfg_t *cfg, uint16_t idle_mv);

/* 喂一个采样，dt_ms 为距上一个采样的时间。记录到一档时返回 true，并写入 captured_mv。 */
bool winsim_learn_feed(winsim_learn_t *l, uint16_t mv, uint32_t dt_ms, uint16_t *captured_mv);

#endif /* WIN_SIM_H */
