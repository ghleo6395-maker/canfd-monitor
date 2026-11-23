/* SPDX-License-Identifier: MIT */
#include "services/winsim/win_sim.h"

#include <math.h>
#include <stddef.h>

/* Hardware-C 第 12 页第 3 版：R801–R812，b12 = R831，b13 = R845，b14 = R846。 */
static const float k_bits_default[WINSIM_NUM_BITS] = {
    200.0f, 390.0f, 820.0f, 1600.0f, 3300.0f, 6200.0f,
    13000.0f, 24000.0f, 47000.0f, 91000.0f, 180000.0f, 360000.0f, 100.0f,
    47.0f, 150000.0f,
};

typedef enum {
    VERIFY_NONE,
    VERIFY_TARGET,    /* 回读对比目标电压（set_voltage） */
    VERIFY_EXPECTED   /* 回读对比理论电压（扫描，用来发现硬件故障） */
} verify_mode_t;

winsim_cfg_t winsim_default_cfg(void)
{
    winsim_cfg_t c;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        c.r_bit_ohm[i] = k_bits_default[i];
    c.r_pullup_ohm        = 2200.0f;
    c.r_adc_load_ohm      = 356000.0f;
    c.mos_rds_on_ohm      = 5.0f;
    c.short_rds_on_ohm    = 0.05f;
    c.r_series_ohm        = 10.0f;
    c.r_series_max_ohm    = 100.0f;
    c.idle_pot_ohm        = 50000.0f;
    c.idle_series_ohm     = 2202.0f;
    c.idle_wiper_ohm      = 80.0f;
    c.idle_max_ua         = 1800u;
    c.idle_open_margin_mv = 30u;
    c.idle_power_ms       = 5u;
    c.ovp_threshold_mv    = 5500u;
    c.min_open_mv         = 2000u;
    c.settle_delay_ms     = 10u;
    c.channel_switch_ms   = 5u;
    c.verify_tol_mv       = 80u;
    c.trim_tol_mv         = 6u;
    c.trim_steps          = 2u;
    c.verify_enable       = true;
    c.pulse_detect_mv     = 300u;
    c.num_channels        = WINSIM_NUM_CH;
    c.learn_drop_mv       = 150u;
    c.learn_stable_mv     = 25u;
    c.learn_stable_ms     = 500u;
    return c;
}

const char *winsim_status_str(winsim_status_t status)
{
    switch (status) {
    case WINSIM_OK:              return "OK";
    case WINSIM_ERR_PARAM:       return "ERR_PARAM";
    case WINSIM_ERR_OVERVOLTAGE: return "ERR_OVERVOLTAGE";
    case WINSIM_ERR_NO_SUPPLY:   return "ERR_NO_SUPPLY";
    case WINSIM_ERR_VERIFY_FAIL: return "ERR_VERIFY_FAIL";
    case WINSIM_ERR_CALIBRATION: return "ERR_CALIBRATION";
    case WINSIM_ERR_RANGE:       return "ERR_RANGE";
    case WINSIM_ERR_UNSUPPORTED: return "ERR_UNSUPPORTED";
    }
    return "?";
}

/* ------------------------------------------------------------------ 校验 */

static bool hal_ok(const winsim_hal_t *h)
{
    if (!(h && h->shift_write && h->set_oe && h->set_short_0r && h->adc_read_mv && h->delay_ms &&
          h->select_channel))
        return false;
    /* 空闲保持的三个回调要么都有，要么都没有。 */
    unsigned n = (h->idle_power != NULL) + (h->idle_write != NULL) + (h->idle_connect != NULL);
    return n == 0u || n == 3u;
}

static bool cfg_ok(const winsim_cfg_t *c)
{
    if (!(c->r_pullup_ohm > 0.0f) || !(c->r_adc_load_ohm >= 0.0f) || !(c->mos_rds_on_ohm >= 0.0f) ||
        !(c->short_rds_on_ohm >= 0.0f) || !(c->r_series_ohm >= 0.0f) ||
        !(c->r_series_max_ohm >= c->r_series_ohm) || !isfinite(c->r_series_max_ohm) ||
        !(c->idle_pot_ohm > 0.0f) || !isfinite(c->idle_pot_ohm) || !(c->idle_series_ohm >= 0.0f) ||
        !(c->idle_wiper_ohm >= 0.0f) || c->idle_max_ua == 0u ||
        c->ovp_threshold_mv == 0u || c->min_open_mv >= c->ovp_threshold_mv ||
        c->num_channels == 0u || c->num_channels > WINSIM_NUM_CH ||
        c->learn_drop_mv == 0u || c->learn_stable_ms == 0u)
        return false;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if (!(c->r_bit_ohm[i] > 0.0f) || !isfinite(c->r_bit_ohm[i]))
            return false;
    return true;
}

static bool ready(const winsim_ctx_t *ctx)
{
    return ctx && ctx->initialized;
}

bool winsim_idle_supported(const winsim_ctx_t *ctx)
{
    return ready(ctx) && ctx->hal.idle_write != NULL;
}

/* ------------------------------------------------------------------ 电路模型 */

static float series(const winsim_ctx_t *ctx)
{
    return ctx->series_cal[ctx->channel] ? ctx->r_series_ch[ctx->channel] : ctx->cfg.r_series_ohm;
}

static float par(float a, float b)
{
    if (!isfinite(a))
        return b;
    if (!isfinite(b))
        return a;
    return a * b / (a + b);
}

float winsim_mask_ohm(const winsim_ctx_t *ctx, uint16_t mask)
{
    double g = 0.0;
    if (!ctx || mask == 0u)
        return INFINITY;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if (mask & (1u << i))
            g += (double)ctx->g_bit[i];
    return (float)(1.0 / g);
}

/* 电阻阵这一侧的对地电阻（含串联电阻）。 */
static float state_r(const winsim_ctx_t *ctx, uint16_t mask, bool sh)
{
    if (sh)
        return series(ctx) + ctx->cfg.short_rds_on_ohm;
    if (mask == 0u)
        return INFINITY;
    return series(ctx) + winsim_mask_ohm(ctx, mask);
}

/* 辨识之前假定的内阻：配置的上拉并上回读负载。 */
static float fallback_rth(const winsim_cfg_t *c)
{
    return c->r_adc_load_ohm > 0.0f ? par(c->r_pullup_ohm, c->r_adc_load_ohm) : c->r_pullup_ohm;
}

/* 当前通道电阻阵看到的电源：V0、Rth，含回读负载和接着的空闲支路。没有任何数据时返回 false。 */
static bool source_eff(const winsim_ctx_t *ctx, float *v0, float *rth)
{
    const winsim_source_t *s = &ctx->src[ctx->channel];
    const winsim_idle_t *id = &ctx->idle[ctx->channel];
    float v, r;
    if (s->ok) {
        v = s->v0_mv;
        r = s->rth_ohm;
        if (id->on) {                                 /* 把空闲支路并进去 */
            v = v * id->r_ohm / (id->r_ohm + r);
            r = par(r, id->r_ohm);
        }
    } else {
        if (ctx->last_open_mv == 0u)
            return false;
        v = (float)ctx->last_open_mv;                 /* 实测值已经含空闲支路 */
        r = fallback_rth(&ctx->cfg);
        if (id->on)
            r = par(r, id->r_ohm);
    }
    *v0 = v;
    *rth = r;
    return true;
}

static float v_model(float v0, float rth, float r)
{
    if (!(r > 0.0f))
        return 0.0f;
    if (!isfinite(r))
        return v0;
    return v0 * r / (r + rth);
}

/* 接口电压为 v 时所需的对地电阻。 */
static float r_for_v(float v0, float rth, float v)
{
    if (!(v > 0.0f))
        return 0.0f;
    if (!(v < v0))
        return INFINITY;
    return rth * v / (v0 - v);
}

static uint16_t to_mv(float v)
{
    if (!(v > 0.0f))
        return 0u;
    if (v >= 65535.0f)
        return 65535u;
    return (uint16_t)(v + 0.5f);
}

static uint16_t absdiff(uint16_t a, uint16_t b)
{
    return (uint16_t)(a > b ? a - b : b - a);
}

static unsigned ctz32(uint32_t x)
{
    unsigned n = 0u;
    while (!(x & 1u)) {
        x >>= 1;
        n++;
    }
    return n;
}

/* 按格雷码遍历全部 32767 种组合，每步只增减一位的电导（双精度累加，避免误差积累）。
 * 返回误差最小的控制字；best_err 为对应误差。score: 0 = 按电压，1 = 按阻值。 */
static uint16_t search_masks(const winsim_ctx_t *ctx, int score, float v0, float rth, float target,
                             float *best_err)
{
    const float rs = series(ctx);
    double g = 0.0;
    uint16_t m = 0u, best_m = 0u;
    float best = INFINITY;
    for (uint32_t i = 1u; i < WINSIM_NUM_MASKS; i++) {
        unsigned b = ctz32(i);
        m ^= (uint16_t)(1u << b);
        if (m & (1u << b))
            g += (double)ctx->g_bit[b];
        else
            g -= (double)ctx->g_bit[b];
        float r = rs + (float)(1.0 / g);
        float e = score == 0 ? fabsf(v0 * r / (r + rth) - target) : fabsf(r - target);
        if (e < best) {
            best = e;
            best_m = m;
        }
    }
    *best_err = best;
    return best_m;
}

/* 在 直通 / 高阻 / 全部组合 中选接口电压最接近 target 的状态。 */
static void select_by_voltage(const winsim_ctx_t *ctx, float v0, float rth, float target,
                              uint16_t *mask, bool *sh)
{
    if (!(target > 0.0f)) {
        *mask = 0u;
        *sh = true;
        return;
    }
    float best = fabsf(v_model(v0, rth, state_r(ctx, 0u, true)) - target);
    bool best_sh = true;
    uint16_t best_m = 0u;
    float e = fabsf(v0 - target);
    if (e < best) {
        best = e;
        best_sh = false;
    }
    float em;
    uint16_t m = search_masks(ctx, 0, v0, rth, target, &em);
    if (em < best) {
        best_m = m;
        best_sh = false;
    }
    *mask = best_m;
    *sh = best_sh;
}

/* 选阻值最接近 r_target 的状态（有限正值）。 */
static void select_by_resistance(const winsim_ctx_t *ctx, float r_target, uint16_t *mask, bool *sh)
{
    float best = fabsf(state_r(ctx, 0u, true) - r_target), em;
    uint16_t m = search_masks(ctx, 1, 0.0f, 0.0f, r_target, &em);
    *sh = !(em < best);
    *mask = *sh ? 0u : m;
}

/* ------------------------------------------------------------------ 输出与回读 */

static void record_state(winsim_ctx_t *ctx, uint16_t mask, bool sh)
{
    ctx->current_state.channel = ctx->channel;
    ctx->current_state.mask = mask;
    ctx->current_state.short_0r = sh;
    ctx->current_state.actual_r_ohm = state_r(ctx, mask, sh);
}

static bool is_hiz(const winsim_ctx_t *ctx)
{
    return ctx->current_state.mask == 0u && !ctx->current_state.short_0r;
}

static void go_hiz(winsim_ctx_t *ctx)
{
    ctx->hal.set_short_0r(false);
    ctx->hal.shift_write(0u);
    record_state(ctx, 0u, false);
    ctx->current_state.target_mv = 0u;
    ctx->current_state.expected_mv = ctx->last_open_mv;
    ctx->current_state.measured_mv = 0u;
    ctx->current_state.trims = 0u;
}

static void idle_power(winsim_ctx_t *ctx, bool on)
{
    if (ctx->hal.idle_power && ctx->idle_powered != on) {
        ctx->hal.idle_power(on);
        ctx->idle_powered = on;
        if (on)
            ctx->hal.delay_ms(ctx->cfg.idle_power_ms);
    }
}

static void idle_off(winsim_ctx_t *ctx, uint8_t ch)
{
    if (ctx->hal.idle_connect && ctx->idle[ch].on)
        ctx->hal.idle_connect(ch, false);
    ctx->idle[ch].on = false;
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++)
        if (ctx->idle[k].on)
            return;
    idle_power(ctx, false);
}

static void all_idle_off(winsim_ctx_t *ctx)
{
    for (uint8_t k = 0; k < WINSIM_NUM_CH; k++)
        idle_off(ctx, k);
}

/* 切换顺序：进入直通前先撤掉电阻阵，退出直通时先断开直通再写电阻阵。
 * 电阻阵换组合时，如果既有要断开的位又有要接通的位，先只留两者都有的位，再写新组合：
 * 中间态总是阻值更高（电压更接近空闲），不会出现比两端都低的阻值。 */
static void apply_raw(winsim_ctx_t *ctx, uint16_t mask, bool sh)
{
    uint16_t cur = ctx->current_state.short_0r ? 0u : ctx->current_state.mask;
    if (sh) {
        ctx->hal.shift_write(0u);
        ctx->hal.set_short_0r(true);
        mask = 0u;
    } else {
        ctx->hal.set_short_0r(false);
        if ((cur & (uint16_t)~mask) && (mask & (uint16_t)~cur))
            ctx->hal.shift_write((uint16_t)(cur & mask));
        ctx->hal.shift_write(mask);
    }
    record_state(ctx, mask, sh);
}

static void sort_u16(uint16_t *s, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint16_t v = s[i];
        unsigned j = i;
        while (j > 0u && s[j - 1u] > v) {
            s[j] = s[j - 1u];
            j--;
        }
        s[j] = v;
    }
}

static uint16_t read_median(winsim_ctx_t *ctx)
{
    uint16_t s[WINSIM_ADC_SAMPLES];
    for (unsigned i = 0; i < WINSIM_ADC_SAMPLES; i++)
        s[i] = ctx->hal.adc_read_mv();
    sort_u16(s, WINSIM_ADC_SAMPLES);
    return s[WINSIM_ADC_SAMPLES / 2u];
}

/* 读当前通道的接口电压。detect 为 true 时每 1 ms 采样一次共 24 次，判断上拉是不是脉冲供电；
 * 已知是脉冲供电时也这样采样，只取上拉接通期间（高于最大最小值中点）的平均值。 */
static uint16_t read_level(winsim_ctx_t *ctx, bool detect)
{
    winsim_source_t *s = &ctx->src[ctx->channel];
    if (!detect && !s->pulsed)
        return read_median(ctx);
    uint16_t smp[WINSIM_PULSE_SAMPLES];
    uint16_t lo = 0xFFFFu, hi = 0u;
    for (unsigned i = 0; i < WINSIM_PULSE_SAMPLES; i++) {
        smp[i] = ctx->hal.adc_read_mv();
        if (smp[i] < lo)
            lo = smp[i];
        if (smp[i] > hi)
            hi = smp[i];
        if (i + 1u < WINSIM_PULSE_SAMPLES)
            ctx->hal.delay_ms(1u);
    }
    if (detect)
        s->pulsed = ctx->cfg.pulse_detect_mv != 0u && (uint16_t)(hi - lo) > ctx->cfg.pulse_detect_mv &&
                    lo < hi / 2u;
    if (!s->pulsed) {
        sort_u16(smp, WINSIM_PULSE_SAMPLES);
        return smp[WINSIM_PULSE_SAMPLES / 2u];
    }
    uint32_t sum = 0u, n = 0u;
    uint16_t mid = (uint16_t)(((uint32_t)hi + lo) / 2u);
    for (unsigned i = 0; i < WINSIM_PULSE_SAMPLES; i++)
        if (smp[i] >= mid) {
            sum += smp[i];
            n++;
        }
    return (uint16_t)((sum + n / 2u) / n);
}

static bool ovp_trip(winsim_ctx_t *ctx, uint16_t mv)
{
    if (mv <= ctx->cfg.ovp_threshold_mv)
        return false;
    go_hiz(ctx);
    all_idle_off(ctx);
    ctx->ovp_latched = true;
    ctx->current_state.measured_mv = mv;
    return true;
}

static winsim_status_t apply_and_measure(winsim_ctx_t *ctx, uint16_t mask, bool sh,
                                         uint16_t target_mv, verify_mode_t vm,
                                         winsim_state_t *out)
{
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    /* 下拉前先看一次接口电压。 */
    if (ovp_trip(ctx, read_median(ctx))) {
        if (out)
            *out = ctx->current_state;
        return WINSIM_ERR_OVERVOLTAGE;
    }

    apply_raw(ctx, mask, sh);
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t mv = read_level(ctx, false);
    if (ovp_trip(ctx, mv)) {
        if (out)
            *out = ctx->current_state;
        return WINSIM_ERR_OVERVOLTAGE;
    }

    float v0, rth;
    winsim_state_t st = ctx->current_state;
    st.target_mv = target_mv;
    st.expected_mv = source_eff(ctx, &v0, &rth) ? to_mv(v_model(v0, rth, st.actual_r_ohm)) : 0u;
    st.measured_mv = mv;
    st.trims = 0u;
    ctx->current_state = st;
    if (out)
        *out = st;

    if (ctx->cfg.verify_enable) {
        if (vm == VERIFY_TARGET && absdiff(mv, target_mv) > ctx->cfg.verify_tol_mv)
            return WINSIM_ERR_VERIFY_FAIL;
        if (vm == VERIFY_EXPECTED && st.expected_mv && absdiff(mv, st.expected_mv) > ctx->cfg.verify_tol_mv)
            return WINSIM_ERR_VERIFY_FAIL;
    }
    return WINSIM_OK;
}

/* ------------------------------------------------------------------ 公共接口 */

winsim_status_t winsim_init(winsim_ctx_t *ctx, const winsim_hal_t *hal, const winsim_cfg_t *cfg)
{
    winsim_cfg_t c = cfg ? *cfg : winsim_default_cfg();
    if (!ctx || !hal_ok(hal) || !cfg_ok(&c))
        return WINSIM_ERR_PARAM;

    ctx->initialized = false;
    ctx->hal = *hal;
    ctx->cfg = c;

    /* 先禁止输出，锁存全 0、通道 K1，再允许输出。 */
    ctx->hal.set_oe(false);
    ctx->hal.set_short_0r(false);
    ctx->hal.shift_write(0u);
    ctx->hal.select_channel(0u);
    ctx->hal.set_oe(true);
    if (ctx->hal.idle_write) {
        for (uint8_t k = 0; k < WINSIM_NUM_CH; k++)
            ctx->hal.idle_connect(k, false);
        ctx->hal.idle_power(false);
    }

    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        ctx->g_bit[i] = 1.0f / (c.r_bit_ohm[i] + c.mos_rds_on_ohm);
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++) {
        ctx->r_series_ch[k] = c.r_series_ohm;
        ctx->series_cal[k] = false;
        ctx->src[k].v0_mv = 0.0f;
        ctx->src[k].rth_ohm = 0.0f;
        ctx->src[k].ok = false;
        ctx->src[k].pulsed = false;
        ctx->idle[k].on = false;
        ctx->idle[k].code = 0u;
        ctx->idle[k].target_mv = 0u;
        ctx->idle[k].measured_mv = 0u;
        ctx->idle[k].r_ohm = INFINITY;
    }
    ctx->idle_powered = false;
    ctx->channel = 0u;
    ctx->last_open_mv = 0u;
    ctx->ovp_latched = false;
    record_state(ctx, 0u, false);
    ctx->current_state.target_mv = 0u;
    ctx->current_state.expected_mv = 0u;
    ctx->current_state.measured_mv = 0u;
    ctx->current_state.trims = 0u;
    ctx->initialized = true;
    return WINSIM_OK;
}

winsim_status_t winsim_set_hiz(winsim_ctx_t *ctx)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    go_hiz(ctx);
    return WINSIM_OK;
}

/* 空载电压更新到电源模型里（扣掉接着的空闲支路）。 */
static void refresh_v0(winsim_ctx_t *ctx, uint16_t mv)
{
    winsim_source_t *s = &ctx->src[ctx->channel];
    const winsim_idle_t *id = &ctx->idle[ctx->channel];
    if (!s->ok)
        return;
    s->v0_mv = id->on ? (float)mv * (id->r_ohm + s->rth_ohm) / id->r_ohm : (float)mv;
}

winsim_status_t winsim_probe_open_voltage(winsim_ctx_t *ctx, uint16_t *v_open_mv)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    go_hiz(ctx);
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t mv = read_level(ctx, true);
    ctx->current_state.measured_mv = mv;
    if (v_open_mv)
        *v_open_mv = mv;

    if (mv > ctx->cfg.ovp_threshold_mv) {
        all_idle_off(ctx);
        ctx->ovp_latched = true;
        ctx->last_open_mv = 0u;
        return WINSIM_ERR_OVERVOLTAGE;
    }
    if (mv < ctx->cfg.min_open_mv) {
        ctx->last_open_mv = 0u;
        return WINSIM_ERR_NO_SUPPLY;
    }
    ctx->last_open_mv = mv;
    ctx->current_state.expected_mv = mv;
    refresh_v0(ctx, mv);
    return WINSIM_OK;
}

winsim_status_t winsim_identify(winsim_ctx_t *ctx, winsim_source_t *out, float *r_series_out)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    winsim_status_t rc = winsim_probe_open_voltage(ctx, NULL);   /* 同时完成下拉前的过压检查 */
    if (rc != WINSIM_OK)
        return rc;
    const float v0 = (float)ctx->last_open_mv;
    const float rsh = ctx->cfg.short_rds_on_ohm;

    /* 0 Ω 直通：接口对地 = 串联电阻 + 直通管 */
    apply_raw(ctx, 0u, true);
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t v_sh = read_level(ctx, false);
    if (ovp_trip(ctx, v_sh))
        return WINSIM_ERR_OVERVOLTAGE;

    /* 已知电阻：先按现有模型挑一个接近内阻的组合，读数偏离中间太多就按算出的内阻再挑一次。 */
    float v0e, rth_guess, em;
    if (!source_eff(ctx, &v0e, &rth_guess))
        rth_guess = fallback_rth(&ctx->cfg);
    float rth = 0.0f, rs = 0.0f;
    bool solved = false;
    for (int pass = 0; pass < 2; pass++) {
        uint16_t m1 = search_masks(ctx, 1, 0.0f, 0.0f, rth_guess, &em);
        apply_raw(ctx, m1, false);
        ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
        uint16_t v1 = read_level(ctx, false);
        if (ovp_trip(ctx, v1))
            return WINSIM_ERR_OVERVOLTAGE;
        solved = false;
        if (!(v0 > (float)v1 && v1 > v_sh))
            break;
        float a = (float)v_sh / (v0 - (float)v_sh);   /* (rs + rsh) / Rth */
        float b = (float)v1 / (v0 - (float)v1);       /* (rs + R1) / Rth */
        float r1 = winsim_mask_ohm(ctx, m1);
        rth = (r1 - rsh) / (b - a);
        rs = a * rth - rsh;
        solved = rth > 0.0f && isfinite(rth);
        float ratio = (float)v1 / v0;
        if (!solved || (ratio > 0.2f && ratio < 0.8f))
            break;
        rth_guess = rth;
    }
    go_hiz(ctx);

    if (rs < 0.0f && rs > -2.0f)
        rs = 0.0f;   /* ADC 取整造成的小负值 */
    if (!solved || !(rs >= 0.0f) || !(rs <= ctx->cfg.r_series_max_ohm))
        return WINSIM_ERR_CALIBRATION;

    /* 这一路的空闲支路接着时，把它从测到的内阻里扣掉，存 APM 一侧本身的电源。 */
    float v0_apm = v0, rth_apm = rth;
    const winsim_idle_t *id = &ctx->idle[ctx->channel];
    if (id->on) {
        float g = 1.0f / rth - 1.0f / id->r_ohm;
        if (!(g > 0.0f))
            return WINSIM_ERR_CALIBRATION;
        rth_apm = 1.0f / g;
        v0_apm = v0 * (id->r_ohm + rth_apm) / id->r_ohm;
    }
    winsim_source_t *s = &ctx->src[ctx->channel];
    s->v0_mv = v0_apm;
    s->rth_ohm = rth_apm;
    s->ok = true;
    ctx->r_series_ch[ctx->channel] = rs;
    ctx->series_cal[ctx->channel] = true;
    record_state(ctx, 0u, false);
    if (out)
        *out = *s;
    if (r_series_out)
        *r_series_out = rs;
    return WINSIM_OK;
}

winsim_status_t winsim_calibrate_series(winsim_ctx_t *ctx, float *r_series_out)
{
    return winsim_identify(ctx, NULL, r_series_out);
}

winsim_status_t winsim_select_channel(winsim_ctx_t *ctx, uint8_t ch, bool calibrate, float *r_series_out)
{
    if (!ready(ctx) || ch >= ctx->cfg.num_channels)
        return WINSIM_ERR_PARAM;

    go_hiz(ctx);                       /* 先断开电阻阵，切换过程中两路都只看到高阻 */
    ctx->hal.select_channel(ch);
    ctx->hal.delay_ms(ctx->cfg.channel_switch_ms);
    ctx->channel = ch;
    ctx->last_open_mv = 0u;            /* 空载电压属于上一路 */
    record_state(ctx, 0u, false);
    ctx->current_state.expected_mv = 0u;

    if (!calibrate) {
        if (r_series_out)
            *r_series_out = series(ctx);
        return WINSIM_OK;
    }
    return winsim_identify(ctx, NULL, r_series_out);
}

winsim_status_t winsim_measure(winsim_ctx_t *ctx, uint16_t *mv_out)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    if (!is_hiz(ctx)) {
        go_hiz(ctx);
        ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    }
    uint16_t mv = read_level(ctx, false);
    ctx->current_state.measured_mv = mv;
    if (mv_out)
        *mv_out = mv;
    if (mv > ctx->cfg.ovp_threshold_mv) {
        all_idle_off(ctx);
        ctx->ovp_latched = true;
        return WINSIM_ERR_OVERVOLTAGE;
    }
    return WINSIM_OK;
}

float winsim_equiv_ohm(const winsim_ctx_t *ctx, uint16_t mv)
{
    float v0, rth;
    if (!ready(ctx) || !source_eff(ctx, &v0, &rth))
        return NAN;
    if ((float)mv >= v0)
        return INFINITY;
    return r_for_v(v0, rth, (float)mv);
}

winsim_status_t winsim_set_resistance(winsim_ctx_t *ctx, float target_ohm, winsim_state_t *out_state)
{
    uint16_t mask = 0u;
    bool sh = false;
    if (!ready(ctx) || !(target_ohm >= 0.0f))
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    if (target_ohm == 0.0f)
        sh = true;
    else if (isfinite(target_ohm))
        select_by_resistance(ctx, target_ohm, &mask, &sh);
    /* INFINITY：保持 mask = 0、sh = false，即高阻 */

    return apply_and_measure(ctx, mask, sh, 0u, VERIFY_NONE, out_state);
}

/* 有可用的电源模型；刚换过通道（空载电压属于上一路）时先探测一次。 */
static winsim_status_t ensure_model(winsim_ctx_t *ctx, float *v0, float *rth)
{
    if (ctx->last_open_mv == 0u) {
        winsim_status_t rc = winsim_probe_open_voltage(ctx, NULL);
        if (rc != WINSIM_OK)
            return rc;
    }
    return source_eff(ctx, v0, rth) ? WINSIM_OK : WINSIM_ERR_NO_SUPPLY;
}

winsim_status_t winsim_set_voltage(winsim_ctx_t *ctx, uint16_t target_mv, winsim_state_t *out_state)
{
    uint16_t mask = 0u;
    bool sh = false;
    float v0, rth;
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    winsim_status_t rc = ensure_model(ctx, &v0, &rth);
    if (rc != WINSIM_OK)
        return rc;

    select_by_voltage(ctx, v0, rth, (float)target_mv, &mask, &sh);
    rc = apply_and_measure(ctx, mask, sh, target_mv, VERIFY_NONE, NULL);
    if (rc != WINSIM_OK) {
        if (out_state)
            *out_state = ctx->current_state;
        return rc;
    }

    /* 按回读微调：用这一档的实测值算出局部等效内阻，重新求解；只接受更好的结果。 */
    winsim_state_t best = ctx->current_state;
    uint8_t trims = 0u;
    while (ctx->cfg.verify_enable && target_mv > 0u && trims < ctx->cfg.trim_steps &&
           absdiff(best.measured_mv, target_mv) > ctx->cfg.trim_tol_mv) {
        float vm = (float)best.measured_mv;
        float r = best.actual_r_ohm;
        if (!(vm > 0.0f && vm < v0) || !isfinite(r))
            break;
        float rth_local = r * (v0 - vm) / vm;
        select_by_voltage(ctx, v0, rth_local, (float)target_mv, &mask, &sh);
        if (mask == best.mask && sh == best.short_0r)
            break;
        trims++;
        rc = apply_and_measure(ctx, mask, sh, target_mv, VERIFY_NONE, NULL);
        if (rc != WINSIM_OK) {
            if (out_state)
                *out_state = ctx->current_state;
            return rc;
        }
        if (absdiff(ctx->current_state.measured_mv, target_mv) < absdiff(best.measured_mv, target_mv)) {
            best = ctx->current_state;
        } else {
            rc = apply_and_measure(ctx, best.mask, best.short_0r, target_mv, VERIFY_NONE, NULL);
            if (rc != WINSIM_OK) {
                if (out_state)
                    *out_state = ctx->current_state;
                return rc;
            }
            best.measured_mv = ctx->current_state.measured_mv;
            break;
        }
    }
    ctx->current_state.trims = trims;
    ctx->current_state.target_mv = target_mv;
    if (out_state)
        *out_state = ctx->current_state;
    if (ctx->cfg.verify_enable && absdiff(ctx->current_state.measured_mv, target_mv) > ctx->cfg.verify_tol_mv)
        return WINSIM_ERR_VERIFY_FAIL;
    return WINSIM_OK;
}

winsim_status_t winsim_set_ratio(winsim_ctx_t *ctx, float ratio_0_to_1, winsim_state_t *out_state)
{
    uint16_t mask = 0u;
    bool sh = false;
    if (!ready(ctx) || !(ratio_0_to_1 >= 0.0f && ratio_0_to_1 <= 1.0f))
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    float v0, rth;
    bool model = source_eff(ctx, &v0, &rth);
    if (!model) {
        v0 = 1000.0f;                     /* 比例与电源绝对值无关 */
        rth = fallback_rth(&ctx->cfg);
    }
    if (ratio_0_to_1 == 0.0f)
        sh = true;
    else if (ratio_0_to_1 < 1.0f)
        select_by_voltage(ctx, v0, rth, ratio_0_to_1 * v0, &mask, &sh);

    uint16_t target_mv = model ? to_mv(ratio_0_to_1 * v0) : 0u;
    return apply_and_measure(ctx, mask, sh, target_mv, VERIFY_NONE, out_state);
}

winsim_status_t winsim_auto_sweep(winsim_ctx_t *ctx, uint16_t start_mv, uint16_t end_mv,
                                  uint16_t step_mv, uint16_t hold_ms,
                                  winsim_sweep_cb_t on_step, void *user_ctx)
{
    if (!ready(ctx) || step_mv == 0u)
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    /* 唤醒后 APM 的上拉可能刚建立，扫描前重新探测。 */
    winsim_status_t rc = winsim_probe_open_voltage(ctx, NULL);
    float v0, rth;
    if (rc == WINSIM_OK && !source_eff(ctx, &v0, &rth))
        rc = WINSIM_ERR_NO_SUPPLY;
    if (rc != WINSIM_OK) {
        go_hiz(ctx);
        return rc;
    }

    bool up = end_mv >= start_mv;
    uint32_t span = up ? (uint32_t)end_mv - start_mv : (uint32_t)start_mv - end_mv;
    uint32_t steps = span / step_mv + 1u;

    for (uint32_t i = 0; i < steps; i++) {
        uint32_t d = i * step_mv;
        uint16_t t = (uint16_t)(up ? start_mv + d : start_mv - d);
        uint16_t mask = 0u;
        bool sh = false;
        winsim_state_t st;

        select_by_voltage(ctx, v0, rth, (float)t, &mask, &sh);
        rc = apply_and_measure(ctx, mask, sh, t, VERIFY_EXPECTED, &st);
        if (rc != WINSIM_OK) {
            go_hiz(ctx);
            return rc;
        }
        ctx->hal.delay_ms(hold_ms);
        if (on_step)
            on_step(&st, user_ctx);
    }
    go_hiz(ctx);
    return WINSIM_OK;
}

winsim_status_t winsim_clear_ovp(winsim_ctx_t *ctx)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    go_hiz(ctx);
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t mv = read_median(ctx);
    ctx->current_state.measured_mv = mv;
    if (mv > ctx->cfg.ovp_threshold_mv) {
        ctx->ovp_latched = true;
        return WINSIM_ERR_OVERVOLTAGE;
    }
    ctx->ovp_latched = false;
    ctx->last_open_mv = mv >= ctx->cfg.min_open_mv ? mv : 0u;
    return WINSIM_OK;
}

/* ------------------------------------------------------------------ 空闲保持 */

winsim_status_t winsim_idle_release(winsim_ctx_t *ctx, uint8_t ch)
{
    if (!ready(ctx) || ch >= ctx->cfg.num_channels)
        return WINSIM_ERR_PARAM;
    if (!ctx->hal.idle_write)
        return WINSIM_ERR_UNSUPPORTED;
    idle_off(ctx, ch);
    if (ch == ctx->channel)
        ctx->last_open_mv = 0u;   /* 空载电压变了 */
    return WINSIM_OK;
}

static void idle_set(winsim_ctx_t *ctx, uint8_t ch, uint16_t code)
{
    winsim_idle_t *id = &ctx->idle[ch];
    ctx->hal.idle_write(ch, code);
    id->code = code;
    id->r_ohm = ctx->cfg.idle_series_ohm + ctx->cfg.idle_wiper_ohm +
                (float)code * ctx->cfg.idle_pot_ohm / (float)WINSIM_IDLE_CODES;
}

static uint16_t clamp_code(float c)
{
    if (!(c > 0.0f))
        return 0u;
    if (c >= (float)(WINSIM_IDLE_CODES - 1u))
        return (uint16_t)(WINSIM_IDLE_CODES - 1u);
    return (uint16_t)(c + 0.5f);
}

winsim_status_t winsim_idle_hold(winsim_ctx_t *ctx, uint8_t ch, uint16_t idle_mv, winsim_idle_t *out)
{
    if (!ready(ctx) || ch >= ctx->cfg.num_channels)
        return WINSIM_ERR_PARAM;
    if (!ctx->hal.idle_write)
        return WINSIM_ERR_UNSUPPORTED;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;
    winsim_status_t rc;
    if (ctx->channel != ch && (rc = winsim_select_channel(ctx, ch, false, NULL)) != WINSIM_OK)
        return rc;
    winsim_idle_t *id = &ctx->idle[ch];
    const float step = ctx->cfg.idle_pot_ohm / (float)WINSIM_IDLE_CODES;
    const float r_fix = ctx->cfg.idle_series_ohm + ctx->cfg.idle_wiper_ohm;

    /* 1. 断开这一路的空闲支路，量开路电压。 */
    idle_off(ctx, ch);
    rc = winsim_probe_open_voltage(ctx, NULL);
    id->target_mv = idle_mv;
    if (rc != WINSIM_OK)
        return rc;
    const float v0 = (float)ctx->last_open_mv;
    id->measured_mv = ctx->last_open_mv;
    if (idle_mv == 0u || (float)idle_mv + (float)ctx->cfg.idle_open_margin_mv >= v0) {
        if (out)
            *out = *id;
        return WINSIM_OK;                      /* 空闲就是开路 */
    }

    /* 2. 电位器放到最大阻值接通：接口电压只比开路略低，用这一点算 APM 一侧的内阻。 */
    idle_power(ctx, true);
    idle_set(ctx, ch, (uint16_t)(WINSIM_IDLE_CODES - 1u));
    ctx->hal.idle_connect(ch, true);
    id->on = true;
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t v1 = read_level(ctx, false);
    if (ovp_trip(ctx, v1))
        return WINSIM_ERR_OVERVOLTAGE;
    if ((float)v1 <= (float)idle_mv) {
        /* 最大阻值也拉得比空闲低：所需电阻超出范围。取“最大阻值”和“开路”里离空闲近的一个。 */
        if ((float)idle_mv - (float)v1 < v0 - (float)idle_mv && absdiff(v1, idle_mv) <= ctx->cfg.verify_tol_mv) {
            id->measured_mv = v1;
            ctx->last_open_mv = v1;
            if (out)
                *out = *id;
            return WINSIM_OK;
        }
        idle_off(ctx, ch);
        id->measured_mv = (uint16_t)v0;
        if (out)
            *out = *id;
        return WINSIM_ERR_RANGE;
    }
    float rth = id->r_ohm * (v0 - (float)v1) / (float)v1;
    if (!(rth > 0.0f) || !isfinite(rth)) {
        idle_off(ctx, ch);
        return WINSIM_ERR_CALIBRATION;
    }
    winsim_source_t *s = &ctx->src[ch];
    if (!s->ok) {
        s->rth_ohm = rth;
        s->ok = true;
    }
    s->v0_mv = v0;

    /* 3. 所需电阻、电流检查、码值。码值从最大往下改，接口电压从开路一侧接近空闲。 */
    float r_need = r_for_v(v0, rth, (float)idle_mv);
    float ua = v0 * 1000.0f / (rth + r_need);
    float c = (r_need - r_fix) / step;
    if (ua > (float)ctx->cfg.idle_max_ua || c < -0.5f) {
        idle_off(ctx, ch);
        id->measured_mv = (uint16_t)v0;
        if (out)
            *out = *id;
        return WINSIM_ERR_RANGE;
    }
    idle_set(ctx, ch, clamp_code(c));
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t vm = read_level(ctx, false);
    if (ovp_trip(ctx, vm))
        return WINSIM_ERR_OVERVOLTAGE;

    uint16_t best_code = id->code, best_mv = vm;
    for (uint8_t k = 0; k < ctx->cfg.trim_steps && absdiff(best_mv, idle_mv) > ctx->cfg.trim_tol_mv; k++) {
        float r_meas = r_for_v(v0, rth, (float)best_mv);
        if (!isfinite(r_meas))
            break;
        uint16_t code = clamp_code((float)best_code + (r_need - r_meas) / step);
        if (code == best_code)
            break;
        idle_set(ctx, ch, code);
        ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
        vm = read_level(ctx, false);
        if (ovp_trip(ctx, vm))
            return WINSIM_ERR_OVERVOLTAGE;
        if (absdiff(vm, idle_mv) < absdiff(best_mv, idle_mv)) {
            best_code = code;
            best_mv = vm;
        }
    }
    if (id->code != best_code) {
        idle_set(ctx, ch, best_code);
        ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
        best_mv = read_level(ctx, false);
    }
    id->measured_mv = best_mv;
    ctx->last_open_mv = best_mv;
    ctx->current_state.measured_mv = best_mv;
    if (out)
        *out = *id;
    if (absdiff(best_mv, idle_mv) > ctx->cfg.verify_tol_mv)
        return WINSIM_ERR_VERIFY_FAIL;
    return WINSIM_OK;
}

/* ------------------------------------------------------------------ 按键学习 */

void winsim_learn_begin(winsim_learn_t *l, const winsim_cfg_t *cfg, uint16_t idle_mv)
{
    winsim_cfg_t d = winsim_default_cfg();
    if (!cfg)
        cfg = &d;
    l->idle_mv = idle_mv;
    l->drop_mv = cfg->learn_drop_mv;
    l->stable_mv = cfg->learn_stable_mv;
    l->stable_ms = cfg->learn_stable_ms;
    l->armed = true;
    l->have_last = false;
    l->last_mv = 0u;
    l->stable_for_ms = 0u;
    l->sum_mv = 0u;
    l->n = 0u;
}

bool winsim_learn_feed(winsim_learn_t *l, uint16_t mv, uint32_t dt_ms, uint16_t *captured_mv)
{
    bool pressed = (uint32_t)mv + l->drop_mv < l->idle_mv;
    if (!pressed) {
        l->armed = true;               /* 回到空闲，可以学下一档 */
        l->have_last = false;
        l->stable_for_ms = 0u;
        return false;
    }
    if (!l->armed)
        return false;

    if (l->have_last && absdiff(mv, l->last_mv) <= l->stable_mv) {
        l->stable_for_ms += dt_ms;
        l->sum_mv += mv;
        l->n++;
    } else {
        l->stable_for_ms = 0u;         /* 新的一段，从这个采样重新计 */
        l->sum_mv = mv;
        l->n = 1u;
    }
    l->have_last = true;
    l->last_mv = mv;

    if (l->stable_for_ms < l->stable_ms)
        return false;
    if (captured_mv)
        *captured_mv = (uint16_t)((l->sum_mv + l->n / 2u) / l->n);
    l->armed = false;
    l->stable_for_ms = 0u;
    return true;
}
