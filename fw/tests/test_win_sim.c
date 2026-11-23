/* SPDX-License-Identifier: MIT */
/*
 * win_sim 主机单元测试（第 3 版硬件：4 路共用 15 位电阻阵，每路一个空闲保持支路）。
 * Mock HAL 按物理模型仿真每一路 APM 输入：上拉（默认 2.2 kΩ 到 5 V，可改阻值、电压、脉冲供电），
 * 接口侧 356 kΩ 回读负载，可选的原车开关电阻，驱动事先不知道的串联电阻（PTC + 继电器），
 * 以及空闲支路（继电器 + 2.2 kΩ + AD5292 50 kΩ）。
 * 另有两片 TCA9539 的寄存器级模型（U801 @0x74、U808 @0x75）和 AD5292 的 SPI 解码，
 * 测试硬件适配的上电顺序、位映射、通道地址、电位器帧和 I2C 失败处理。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "services/winsim/win_sim.h"
#include "services/winsim/win_sim_tca9539.h"

static int g_checks, g_fail;
#define CHECK(c)                                                                 \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(c)) {                                                              \
            g_fail++;                                                            \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);               \
        }                                                                        \
    } while (0)

static winsim_ctx_t g_ctx;

/* ------------------------------------------------------------------ 物理模型 */

typedef struct {
    float    vs_mv;       /* 这一路 APM 的上拉电源 */
    float    rpu;         /* 上拉电阻 */
    float    rload;       /* 回读分压总阻值 */
    float    rseries;     /* PTC + 继电器，驱动不知道 */
    float    ext_r;       /* 原车开关对地电阻，INFINITY = 没接或松开 */
    uint32_t pulse_period_ms;   /* 非 0：上拉是脉冲供电 */
    uint32_t pulse_on_ms;
    /* 空闲支路 */
    bool     idle_on;
    uint16_t idle_code;
} chan_t;

typedef struct {
    float  rds;        /* 电阻阵开关管 */
    float  short_rds;  /* 直通管 */
    float  rbit[WINSIM_NUM_BITS];
    float  idle_fix;   /* R827 + 继电器 */
    float  pot;        /* AD5292 满量程（含 ±% 误差） */
    float  wiper;
    bool   vpot;       /* 升压已开 */
    chan_t ch[WINSIM_NUM_CH];
} phys_t;

static const float k_bits[WINSIM_NUM_BITS] = {
    200.0f, 390.0f, 820.0f, 1600.0f, 3300.0f, 6200.0f,
    13000.0f, 24000.0f, 47000.0f, 91000.0f, 180000.0f, 360000.0f, 100.0f,
    47.0f, 150000.0f,
};

static phys_t phys_default(void)
{
    phys_t p;
    memset(&p, 0, sizeof p);
    p.rds = 5.0f;
    p.short_rds = 0.05f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        p.rbit[i] = k_bits[i];
    p.idle_fix = 2203.0f;
    p.pot = 50000.0f;
    p.wiper = 85.0f;
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++) {
        p.ch[k].vs_mv = 5000.0f;
        p.ch[k].rpu = 2200.0f;
        p.ch[k].rload = 356000.0f;
        p.ch[k].rseries = 12.0f;
        p.ch[k].ext_r = INFINITY;
    }
    return p;
}

static float idle_ohm(const phys_t *p, unsigned ch)
{
    return p->idle_fix + p->wiper + (float)p->ch[ch].idle_code * p->pot / 1024.0f;
}

static float phys_port_mv(const phys_t *p, unsigned ch, uint16_t mask, bool sh, uint32_t t_ms)
{
    const chan_t *c = &p->ch[ch];
    float g_in = sh ? 1.0f / p->short_rds : 0.0f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if (mask & (1u << i))
            g_in += 1.0f / (p->rbit[i] + p->rds);
    float g = c->rload > 0.0f ? 1.0f / c->rload : 0.0f;
    if (isfinite(c->ext_r))
        g += c->ext_r > 0.0f ? 1.0f / c->ext_r : 1e9f;
    if (g_in > 0.0f)
        g += 1.0f / (c->rseries + 1.0f / g_in);
    if (c->idle_on)
        g += 1.0f / idle_ohm(p, ch);
    float vs = c->vs_mv;
    if (c->pulse_period_ms && (t_ms % c->pulse_period_ms) >= c->pulse_on_ms)
        vs = 0.0f;   /* 脉冲供电的断开期间 */
    if (!(g > 0.0f))
        return vs;
    float rp = 1.0f / g;
    return vs * rp / (rp + c->rpu);
}

static uint16_t clamp_adc_mv(float v)
{
    if (v < 0.0f)
        v = 0.0f;
    if (v > 20978.0f)          /* 300k/56k 分压 × 3.3 V 满量程 */
        v = 20978.0f;
    return (uint16_t)(v + 0.5f);
}

/* ------------------------------------------------------------------ 核心 Mock HAL */

#define LOG_MAX 4096

static struct {
    phys_t   ph;
    bool     oe, sh;
    uint16_t mask;
    uint8_t  ch;
    char     log[LOG_MAX][8];
    unsigned nlog;
    unsigned bad_states;    /* 直通与电阻阵同时导通的次数 */
    unsigned hot_switches;  /* 电阻阵或直通还接着时换通道的次数 */
    unsigned idle_unpowered;/* 升压没开时接通空闲支路的次数 */
    unsigned adc_reads;
    uint32_t delay_total;
    uint16_t adc_min;       /* 记录期间所有回读的最小值 */
    bool     track_min;
} M;

static void mlog(const char *s)
{
    if (M.nlog < LOG_MAX) {
        strncpy(M.log[M.nlog], s, sizeof M.log[0] - 1u);
        M.nlog++;
    }
}

static void m_check(void)
{
    if (M.oe && M.sh && M.mask)
        M.bad_states++;
}

static void m_shift_write(uint16_t mask)
{
    char b[8];
    snprintf(b, sizeof b, "w%04X", (unsigned)mask);
    mlog(b);
    M.mask = mask;
    m_check();
}

static void m_set_oe(bool en)
{
    mlog(en ? "oe1" : "oe0");
    M.oe = en;
    m_check();
}

static void m_set_short(bool en)
{
    mlog(en ? "sh1" : "sh0");
    M.sh = en;
    m_check();
}

static void m_select(uint8_t ch)
{
    char b[8];
    snprintf(b, sizeof b, "c%u", (unsigned)ch);
    mlog(b);
    if (M.oe && (M.mask || M.sh))
        M.hot_switches++;
    M.ch = ch;
}

static uint16_t m_adc(void)
{
    M.adc_reads++;
    if (!M.oe)
        return 0u;   /* 使能关闭时回读开关断开，R819 拉到 0 */
    uint16_t v = clamp_adc_mv(phys_port_mv(&M.ph, M.ch, M.mask, M.sh, M.delay_total));
    if (M.track_min && v < M.adc_min)
        M.adc_min = v;
    return v;
}

static void m_delay(uint32_t ms)
{
    M.delay_total += ms;
}

static void m_idle_power(bool on)
{
    mlog(on ? "vp1" : "vp0");
    M.ph.vpot = on;
}

static void m_idle_write(uint8_t ch, uint16_t code)
{
    char b[8];
    snprintf(b, sizeof b, "p%u", (unsigned)ch);
    mlog(b);
    M.ph.ch[ch].idle_code = (uint16_t)(code & 0x3FFu);
}

static void m_idle_connect(uint8_t ch, bool on)
{
    char b[8];
    snprintf(b, sizeof b, "i%u%u", (unsigned)ch, (unsigned)on);
    mlog(b);
    if (on && !M.ph.vpot)
        M.idle_unpowered++;
    M.ph.ch[ch].idle_on = on;
}

static winsim_hal_t mock_reset(void)
{
    winsim_hal_t h;
    memset(&M, 0, sizeof M);
    M.ph = phys_default();
    memset(&h, 0, sizeof h);
    h.shift_write = m_shift_write;
    h.set_oe = m_set_oe;
    h.set_short_0r = m_set_short;
    h.adc_read_mv = m_adc;
    h.delay_ms = m_delay;
    h.select_channel = m_select;
    return h;
}

static winsim_hal_t mock_reset_idle(void)
{
    winsim_hal_t h = mock_reset();
    h.idle_power = m_idle_power;
    h.idle_write = m_idle_write;
    h.idle_connect = m_idle_connect;
    return h;
}

static bool mock_is_hiz(void)
{
    return !M.sh && M.mask == 0u;
}

static unsigned udiff(unsigned a, unsigned b)
{
    return a > b ? a - b : b - a;
}

static uint32_t g_seed = 12345u;
static float frand(void)   /* [0, 1) */
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)(g_seed >> 8) / 16777216.0f;
}

/* 原车开关空闲时接在 APM 上的电阻 R 所对应的接口电压（含回读负载）。 */
static float idle_mv_for(const chan_t *c, float r)
{
    float rp = 1.0f / (1.0f / r + 1.0f / c->rload);
    return c->vs_mv * rp / (rp + c->rpu);
}

/* ------------------------------------------------------------------ 测试 */

static void test_init_order(void)
{
    winsim_hal_t h = mock_reset();
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(M.nlog == 5u);
    CHECK(strcmp(M.log[0], "oe0") == 0);
    CHECK(strcmp(M.log[1], "sh0") == 0);
    CHECK(strcmp(M.log[2], "w0000") == 0);
    CHECK(strcmp(M.log[3], "c0") == 0);
    CHECK(strcmp(M.log[4], "oe1") == 0);

    CHECK(isinf(winsim_mask_ohm(&g_ctx, 0u)));
    CHECK(fabsf(winsim_mask_ohm(&g_ctx, 1u) - 205.0f) < 0.01f);
    CHECK(fabsf(winsim_mask_ohm(&g_ctx, 1u << 12) - 105.0f) < 0.01f);    /* b12 = 100 Ω */
    CHECK(fabsf(winsim_mask_ohm(&g_ctx, 1u << 13) - 52.0f) < 0.01f);     /* b13 = 47 Ω */
    CHECK(fabsf(winsim_mask_ohm(&g_ctx, 1u << 14) - 150005.0f) < 1.0f);  /* b14 = 150 kΩ */
    float g = 0.0f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        g += 1.0f / (k_bits[i] + 5.0f);
    CHECK(fabsf(winsim_mask_ohm(&g_ctx, WINSIM_MASK_ALL) * g - 1.0f) < 1e-4f);
    CHECK(winsim_mask_ohm(&g_ctx, WINSIM_MASK_ALL) < 30.0f);             /* 全开约 26 Ω */
    CHECK(g_ctx.current_state.mask == 0u && !g_ctx.current_state.short_0r);
    CHECK(g_ctx.channel == 0u && g_ctx.current_state.channel == 0u);
    CHECK(!g_ctx.ovp_latched && g_ctx.last_open_mv == 0u);
    CHECK(!winsim_idle_supported(&g_ctx));
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++)
        CHECK(!g_ctx.series_cal[k] && !g_ctx.src[k].ok && !g_ctx.idle[k].on);

    /* 有空闲保持硬件：额外断开 4 路继电器、关升压。 */
    h = mock_reset_idle();
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(M.nlog == 10u);
    CHECK(strcmp(M.log[5], "i00") == 0 && strcmp(M.log[8], "i30") == 0 && strcmp(M.log[9], "vp0") == 0);
    CHECK(winsim_idle_supported(&g_ctx));
}

static void test_init_params(void)
{
    winsim_hal_t h = mock_reset();
    winsim_hal_t bad = h;
    bad.adc_read_mv = NULL;
    CHECK(winsim_init(&g_ctx, &bad, NULL) == WINSIM_ERR_PARAM);
    bad = h;
    bad.select_channel = NULL;
    CHECK(winsim_init(&g_ctx, &bad, NULL) == WINSIM_ERR_PARAM);
    bad = mock_reset_idle();
    bad.idle_connect = NULL;     /* 空闲回调只给了一部分 */
    CHECK(winsim_init(&g_ctx, &bad, NULL) == WINSIM_ERR_PARAM);
    h = mock_reset();
    CHECK(winsim_init(NULL, &h, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_init(&g_ctx, NULL, NULL) == WINSIM_ERR_PARAM);

    winsim_cfg_t c = winsim_default_cfg();
    c.min_open_mv = 6000u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c = winsim_default_cfg();
    c.r_bit_ohm[14] = 0.0f;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c = winsim_default_cfg();
    c.num_channels = 0u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c.num_channels = 5u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c = winsim_default_cfg();
    c.r_series_max_ohm = 5.0f;   /* 小于默认串联电阻 */
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c = winsim_default_cfg();
    c.idle_pot_ohm = 0.0f;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    CHECK(M.nlog == 0u);         /* 参数错误时不碰硬件 */

    memset(&g_ctx, 0, sizeof g_ctx);
    CHECK(winsim_set_voltage(&g_ctx, 1000u, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_hiz(&g_ctx) == WINSIM_ERR_PARAM);
    CHECK(winsim_select_channel(&g_ctx, 0u, false, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_measure(&g_ctx, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_identify(&g_ctx, NULL, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_idle_hold(&g_ctx, 0u, 4000u, NULL) == WINSIM_ERR_PARAM);
    CHECK(isnan(winsim_equiv_ohm(&g_ctx, 1000u)));
    CHECK(winsim_auto_sweep(&g_ctx, 0u, 100u, 10u, 0u, NULL, NULL) == WINSIM_ERR_PARAM);

    /* 没有空闲硬件时空闲接口返回 UNSUPPORTED，不碰硬件。 */
    h = mock_reset();
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    unsigned n = M.nlog;
    CHECK(winsim_idle_hold(&g_ctx, 1u, 4000u, NULL) == WINSIM_ERR_UNSUPPORTED);
    CHECK(winsim_idle_release(&g_ctx, 1u) == WINSIM_ERR_UNSUPPORTED);
    CHECK(M.nlog == n);
}

static void test_channel_select(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    float rs = 0.0f;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 2500u, &st) == WINSIM_OK);   /* K1 正在下拉 */
    CHECK(!mock_is_hiz());

    unsigned a = M.nlog;
    uint32_t d0 = M.delay_total;
    CHECK(winsim_select_channel(&g_ctx, 2u, false, &rs) == WINSIM_OK);
    CHECK(M.nlog == a + 3u);
    CHECK(strcmp(M.log[a], "sh0") == 0);
    CHECK(strcmp(M.log[a + 1u], "w0000") == 0);   /* 先断开电阻阵 */
    CHECK(strcmp(M.log[a + 2u], "c2") == 0);      /* 再换通道 */
    CHECK(M.delay_total - d0 >= 5u);              /* 等继电器接通 */
    CHECK(M.hot_switches == 0u);
    CHECK(g_ctx.channel == 2u && g_ctx.current_state.channel == 2u);
    CHECK(g_ctx.last_open_mv == 0u);              /* 空载电压属于上一路 */
    CHECK(fabsf(rs - 10.0f) < 1e-6f);             /* 未标定：用默认值 */

    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_OK);   /* 自动在 K3 上探测 */
    CHECK(st.channel == 2u && M.ch == 2u);

    a = M.nlog;
    CHECK(winsim_select_channel(&g_ctx, 4u, false, NULL) == WINSIM_ERR_PARAM);
    CHECK(M.nlog == a);
    winsim_cfg_t c = winsim_default_cfg();
    c.num_channels = 2u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 1u, false, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 2u, false, NULL) == WINSIM_ERR_PARAM);
    CHECK(M.hot_switches == 0u && M.bad_states == 0u);
}

static void test_calibration(void)
{
    static const float rs_true[WINSIM_NUM_CH] = { 5.0f, 12.4f, 17.0f, 9.8f };
    winsim_hal_t h = mock_reset();
    float rs = 0.0f;
    winsim_source_t src;
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++)
        M.ph.ch[k].rseries = rs_true[k];
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    float worst = 0.0f;
    for (uint8_t k = 0; k < WINSIM_NUM_CH; k++) {
        CHECK(winsim_select_channel(&g_ctx, k, true, &rs) == WINSIM_OK);
        CHECK(g_ctx.series_cal[k] && g_ctx.src[k].ok);
        float e = fabsf(rs - rs_true[k]);
        if (e > worst)
            worst = e;
        CHECK(e < 0.6f);
        CHECK(fabsf(g_ctx.src[k].rth_ohm - 2188.0f) < 15.0f);   /* 2.2 kΩ ∥ 356 kΩ */
        CHECK(mock_is_hiz());                    /* 辨识后回到高阻 */
        CHECK(g_ctx.last_open_mv > 4900u);
    }
    printf("  series 5 / 12.4 / 17 / 9.8 ohm: worst calibration error %.2f ohm\n", (double)worst);

    /* 回到 K2：沿用 K2 的标定值。 */
    CHECK(winsim_select_channel(&g_ctx, 1u, false, &rs) == WINSIM_OK);
    CHECK(fabsf(rs - g_ctx.r_series_ch[1]) < 1e-6f);

    /* K3 的 PTC 断开：直通和已知电阻时电压几乎不变，辨识失败，保留原值。 */
    M.ph.ch[2].rseries = 1e7f;
    float before = g_ctx.r_series_ch[2];
    CHECK(winsim_select_channel(&g_ctx, 2u, true, &rs) == WINSIM_ERR_CALIBRATION);
    CHECK(fabsf(g_ctx.r_series_ch[2] - before) < 1e-6f);
    CHECK(mock_is_hiz());

    /* K4 的 APM 没上电。 */
    M.ph.ch[3].vs_mv = 0.0f;
    CHECK(winsim_select_channel(&g_ctx, 3u, true, &rs) == WINSIM_ERR_NO_SUPPLY);
    CHECK(mock_is_hiz());
    CHECK(M.bad_states == 0u && M.hot_switches == 0u);

    /* winsim_identify 直接返回模型。 */
    M.ph.ch[3].vs_mv = 5000.0f;
    CHECK(winsim_identify(&g_ctx, &src, &rs) == WINSIM_OK);
    CHECK(src.ok && fabsf(src.v0_mv - 4969.0f) < 3.0f && fabsf(rs - 9.8f) < 0.6f);
}

static unsigned run_grid(unsigned *zero_mv, unsigned limit)
{
    unsigned worst = 0u;
    for (unsigned t = 200u; t <= 4600u; t += 200u) {
        winsim_state_t st;
        winsim_status_t rc = winsim_set_voltage(&g_ctx, (uint16_t)t, &st);
        unsigned e = udiff(st.measured_mv, t);
        CHECK(rc == WINSIM_OK);
        if (e >= limit)
            printf("  target %4u mV -> %4u mV (mask %04X short %d, series %.1f, trims %u)\n", t, st.measured_mv,
                   (unsigned)st.mask, (int)st.short_0r, (double)g_ctx.r_series_ch[g_ctx.channel], st.trims);
        CHECK(e < limit);
        if (e > worst)
            worst = e;
    }
    winsim_state_t st;
    CHECK(winsim_set_voltage(&g_ctx, 0u, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    if (zero_mv)
        *zero_mv = st.measured_mv;
    return worst;
}

static void test_full_range_nominal(void)
{
    winsim_hal_t h = mock_reset();
    unsigned zero = 0u;
    M.ph.ch[0].rseries = 12.4f;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);
    CHECK(udiff(g_ctx.last_open_mv, 4969u) <= 1u);   /* 5 V × 356k / 358.2k */
    unsigned worst = run_grid(&zero, 15u);
    CHECK(zero < 40u);
    printf("  200–4600 mV / 200 mV, nominal parts, series 12.4 ohm: worst %u mV; 0 mV -> %u mV\n", worst, zero);
    CHECK(M.bad_states == 0u);
}

/* 1% 电阻、导通电阻 3 Ω（模型按 5 Ω）、APM 上拉 ±1%、每路串联 5–17 Ω：辨识 + 按回读微调。 */
static void test_full_range_tolerance(void)
{
    unsigned worst = 0u, worst_zero = 0u;
    g_seed = 12345u;
    for (int trial = 0; trial < 20; trial++) {
        winsim_hal_t h = mock_reset();
        for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
            M.ph.rbit[i] = k_bits[i] * (0.99f + 0.02f * frand());
        M.ph.rds = 3.0f;
        uint8_t ch = (uint8_t)(trial % WINSIM_NUM_CH);
        M.ph.ch[ch].rseries = 5.0f + 12.0f * frand();
        M.ph.ch[ch].rpu = 2200.0f * (0.99f + 0.02f * frand());
        CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
        CHECK(winsim_select_channel(&g_ctx, ch, true, NULL) == WINSIM_OK);
        unsigned zero = 0u;
        unsigned w = run_grid(&zero, 20u);
        if (w > worst)
            worst = w;
        if (zero > worst_zero)
            worst_zero = zero;
    }
    CHECK(worst_zero < 45u);
    printf("  20 sets: 1%% parts, pull-up ±1%%, series 5–17 ohm: worst %u mV; 0 mV -> ≤ %u mV\n", worst, worst_zero);
}

/* 不同车型：上拉 1–10 kΩ、上拉到 5 V 或 3.3 V，驱动不知道这些参数，靠辨识。 */
static void test_other_pullups(void)
{
    static const struct { float rpu, vs; unsigned limit; } cars[] = {
        { 1000.0f, 5000.0f, 22u }, { 2200.0f, 5000.0f, 20u }, { 4700.0f, 5000.0f, 22u },
        { 10000.0f, 5000.0f, 35u }, { 3300.0f, 3300.0f, 15u }, { 1500.0f, 5000.0f, 20u },
    };
    g_seed = 777u;
    for (unsigned c = 0; c < sizeof cars / sizeof cars[0]; c++) {
        unsigned worst = 0u, worst_rs = 0u;
        float worst_rth = 0.0f;
        for (int trial = 0; trial < 4; trial++) {
            winsim_hal_t h = mock_reset();
            for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
                M.ph.rbit[i] = k_bits[i] * (0.99f + 0.02f * frand());
            uint8_t ch = (uint8_t)(trial % WINSIM_NUM_CH);
            M.ph.ch[ch].rpu = cars[c].rpu;
            M.ph.ch[ch].vs_mv = cars[c].vs;
            M.ph.ch[ch].rseries = 5.0f + 12.0f * frand();
            CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
            float rs = 0.0f;
            CHECK(winsim_select_channel(&g_ctx, ch, true, &rs) == WINSIM_OK);
            float rth_true = 1.0f / (1.0f / cars[c].rpu + 1.0f / 356000.0f);
            float er = fabsf(g_ctx.src[ch].rth_ohm - rth_true) / rth_true;
            if (er > worst_rth)
                worst_rth = er;
            CHECK(er < 0.01f);
            unsigned ers = (unsigned)(fabsf(rs - M.ph.ch[ch].rseries) * 10.0f + 0.5f);
            if (ers > worst_rs)
                worst_rs = ers;
            unsigned hi = (unsigned)(cars[c].vs * 0.92f) / 200u * 200u;   /* 不超过开路电压附近 */
            for (unsigned t = 200u; t <= hi; t += 100u) {
                winsim_state_t st;
                CHECK(winsim_set_voltage(&g_ctx, (uint16_t)t, &st) == WINSIM_OK);
                unsigned e = udiff(st.measured_mv, t);
                CHECK(e < cars[c].limit);
                if (e > worst)
                    worst = e;
            }
        }
        printf("  pull-up %5.0f ohm to %.1f V: R_th within %.2f%%, series within %.1f ohm, worst %u mV\n",
               (double)cars[c].rpu, (double)cars[c].vs / 1000.0, (double)(worst_rth * 100.0f),
               (double)worst_rs / 10.0, worst);
    }
}

static void test_zero(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);

    CHECK(winsim_set_voltage(&g_ctx, 0u, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    CHECK(M.sh && M.mask == 0u);
    CHECK(st.measured_mv < 40u);                 /* 串联电阻 12 Ω：约 27 mV */

    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_OK);
    CHECK(winsim_set_resistance(&g_ctx, 0.0f, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    CHECK(M.sh && M.mask == 0u);

    CHECK(winsim_set_hiz(&g_ctx) == WINSIM_OK);
    CHECK(winsim_set_ratio(&g_ctx, 0.0f, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    CHECK(M.bad_states == 0u);
}

static void test_switch_order(void)
{
    winsim_hal_t h = mock_reset();
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, NULL) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 3000u, NULL) == WINSIM_OK);

    unsigned a = M.nlog;
    CHECK(winsim_set_voltage(&g_ctx, 0u, NULL) == WINSIM_OK);
    CHECK(M.nlog == a + 2u);
    CHECK(strcmp(M.log[a], "w0000") == 0);      /* 先撤电阻阵 */
    CHECK(strcmp(M.log[a + 1u], "sh1") == 0);   /* 再接通直通 */

    unsigned b = M.nlog;
    CHECK(winsim_set_voltage(&g_ctx, 2500u, NULL) == WINSIM_OK);
    CHECK(M.nlog >= b + 2u);
    CHECK(strcmp(M.log[b], "sh0") == 0);        /* 先断直通 */
    CHECK(M.log[b + 1u][0] == 'w' && strcmp(M.log[b + 1u], "w0000") != 0);
    CHECK(M.bad_states == 0u);

    /* 换组合：既有要断开的位又有要接通的位时，先只留两者都有的位。 */
    unsigned bad_order = 0u;
    uint16_t prev = M.mask;
    for (unsigned t = 400u; t <= 4400u; t += 300u) {
        unsigned c = M.nlog;
        CHECK(winsim_set_voltage(&g_ctx, (uint16_t)t, NULL) == WINSIM_OK);
        for (unsigned i = c; i < M.nlog; i++) {
            unsigned m = 0u;
            if (M.log[i][0] != 'w' || sscanf(M.log[i] + 1, "%4X", &m) != 1)
                continue;
            /* 每次写入要么只加位，要么只减位 */
            if ((prev & ~m) && (m & ~prev))
                bad_order++;
            prev = (uint16_t)m;
        }
    }
    CHECK(bad_order == 0u);
}

static void test_ovp(void)
{
    winsim_hal_t h = mock_reset_idle();
    winsim_state_t st;
    uint16_t vo = 0u;
    M.ph.ch[0].vs_mv = 12000.0f;   /* K1 误接 12 V */
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(vo > 5500u);
    CHECK(g_ctx.ovp_latched);
    CHECK(mock_is_hiz());

    unsigned n = M.nlog;
    CHECK(winsim_set_voltage(&g_ctx, 2000u, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_set_resistance(&g_ctx, 1000.0f, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_set_ratio(&g_ctx, 0.5f, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_auto_sweep(&g_ctx, 4000u, 0u, 500u, 0u, NULL, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_calibrate_series(&g_ctx, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_idle_hold(&g_ctx, 1u, 4000u, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(M.nlog == n);      /* 锁定后没有任何下拉动作 */
    CHECK(mock_is_hiz());
    CHECK(winsim_measure(&g_ctx, &vo) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_clear_ovp(&g_ctx) == WINSIM_ERR_OVERVOLTAGE);   /* 仍是 12 V，不解锁 */
    CHECK(g_ctx.ovp_latched);

    /* 换到正常的 K2：换通道本身不解锁，也不能标定；重新检测后解锁。 */
    CHECK(winsim_select_channel(&g_ctx, 1u, true, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(g_ctx.channel == 1u && g_ctx.ovp_latched && mock_is_hiz());
    CHECK(winsim_clear_ovp(&g_ctx) == WINSIM_OK);
    CHECK(!g_ctx.ovp_latched);
    CHECK(winsim_calibrate_series(&g_ctx, NULL) == WINSIM_OK);

    /* K2、K3 保持空闲，然后 K2 电源跳到 12 V：下一次回读触发保护，所有空闲支路断开。 */
    M.ph.ch[1].ext_r = INFINITY;
    CHECK(winsim_idle_hold(&g_ctx, 2u, (uint16_t)idle_mv_for(&M.ph.ch[2], 10000.0f), NULL) == WINSIM_OK);
    CHECK(winsim_idle_hold(&g_ctx, 1u, (uint16_t)idle_mv_for(&M.ph.ch[1], 10000.0f), NULL) == WINSIM_OK);
    CHECK(M.ph.ch[1].idle_on && M.ph.ch[2].idle_on && M.ph.vpot);
    CHECK(winsim_set_voltage(&g_ctx, 2000u, &st) == WINSIM_OK);
    M.ph.ch[1].vs_mv = 12000.0f;
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(g_ctx.ovp_latched);
    CHECK(mock_is_hiz());
    CHECK(!M.ph.ch[1].idle_on && !M.ph.ch[2].idle_on && !M.ph.vpot);
    CHECK(!g_ctx.idle[1].on && !g_ctx.idle[2].on);
}

static void test_no_supply(void)
{
    winsim_hal_t h = mock_reset();
    uint16_t vo = 1u;
    M.ph.ch[0].vs_mv = 0.0f;   /* APM 未唤醒 */
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_ERR_NO_SUPPLY);
    CHECK(vo == 0u);
    CHECK(winsim_set_voltage(&g_ctx, 1000u, NULL) == WINSIM_ERR_NO_SUPPLY);
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 50u, 0u, NULL, NULL) == WINSIM_ERR_NO_SUPPLY);
    CHECK(mock_is_hiz());
    CHECK(!g_ctx.ovp_latched);
}

static void test_verify(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    winsim_cfg_t c = winsim_default_cfg();
    c.trim_steps = 0u;           /* 不微调：硬件故障直接表现为超差 */
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_OK);

    /* 把这一档用到的最低阻值那一位弄断，再设同一个电压：回读应超差。 */
    unsigned broken = WINSIM_NUM_BITS;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if ((st.mask & (1u << i)) && (broken == WINSIM_NUM_BITS || k_bits[i] < k_bits[broken]))
            broken = i;
    CHECK(broken < WINSIM_NUM_BITS);
    M.ph.rbit[broken] = 1e9f;
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_ERR_VERIFY_FAIL);

    /* 允许微调时，用别的组合绕开坏掉的位，回读回到容差内；扫描仍按理论值报故障。 */
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);
    winsim_status_t rc = winsim_set_voltage(&g_ctx, 3000u, &st);
    CHECK(st.trims >= 1u);                    /* 试过微调 */
    CHECK(rc == WINSIM_ERR_VERIFY_FAIL || udiff(st.measured_mv, 3000u) <= 80u);

    c = winsim_default_cfg();
    c.verify_enable = false;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_OK);
}

typedef struct {
    unsigned n;
    uint16_t targets[128];
    unsigned worst_target;     /* |回读 − 目标|，目标 ≥ 200 mV */
    unsigned zero_mv;          /* 目标 0 mV 时的回读 */
    unsigned worst_expected;   /* |回读 − 理论| */
    unsigned mismatched;       /* 回调状态与硬件不一致 */
    unsigned inject_at;        /* 第 N 次回调后把电源改成 12 V，0 = 不注入 */
} sweep_rec_t;

static void sweep_cb(const winsim_state_t *st, void *user)
{
    sweep_rec_t *r = (sweep_rec_t *)user;
    if (r->n < 128u)
        r->targets[r->n] = st->target_mv;
    r->n++;
    unsigned et = udiff(st->measured_mv, st->target_mv);
    unsigned ee = udiff(st->measured_mv, st->expected_mv);
    if (st->target_mv >= 200u && et > r->worst_target)
        r->worst_target = et;
    if (st->target_mv == 0u)
        r->zero_mv = st->measured_mv;
    if (ee > r->worst_expected)
        r->worst_expected = ee;
    if (st->mask != M.mask || st->short_0r != M.sh || st->channel != M.ch)
        r->mismatched++;
    if (r->inject_at && r->n == r->inject_at)
        M.ph.ch[M.ch].vs_mv = 12000.0f;
}

static void test_sweep(void)
{
    winsim_hal_t h = mock_reset();
    sweep_rec_t r;
    M.ph.ch[3].rseries = 15.1f;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 3u, true, NULL) == WINSIM_OK);

    memset(&r, 0, sizeof r);
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 200u, 50u, sweep_cb, &r) == WINSIM_OK);
    CHECK(r.n == 25u);
    for (unsigned i = 0; i < 25u; i++)
        CHECK(r.targets[i] == 4800u - 200u * i);
    CHECK(r.worst_target < 25u);
    CHECK(r.zero_mv < 45u);
    CHECK(r.worst_expected <= 2u);
    CHECK(r.mismatched == 0u);
    CHECK(mock_is_hiz());
    CHECK(M.delay_total >= 25u * 50u);
    printf("  K4 sweep 4800→0 mV / 200 mV: %u steps, worst %u mV (≥ 200 mV), 0 mV -> %u mV\n",
           r.n, r.worst_target, r.zero_mv);

    memset(&r, 0, sizeof r);
    CHECK(winsim_auto_sweep(&g_ctx, 0u, 1000u, 300u, 0u, sweep_cb, &r) == WINSIM_OK);
    CHECK(r.n == 4u);
    CHECK(r.targets[0] == 0u && r.targets[3] == 900u);
    CHECK(mock_is_hiz());

    /* 4800→0 / 50 mV，含无档位区间：按理论值校验，不应误报。 */
    memset(&r, 0, sizeof r);
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 50u, 0u, sweep_cb, &r) == WINSIM_OK);
    CHECK(r.n == 97u);

    CHECK(winsim_auto_sweep(&g_ctx, 0u, 1000u, 0u, 0u, sweep_cb, &r) == WINSIM_ERR_PARAM);

    /* 扫描中途电源变成 12 V：返回过压并恢复高阻。 */
    memset(&r, 0, sizeof r);
    r.inject_at = 3u;
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 200u, 0u, sweep_cb, &r) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(r.n == 3u);
    CHECK(mock_is_hiz());
    CHECK(g_ctx.ovp_latched);
}

static void test_ratio(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_set_ratio(&g_ctx, 0.5f, &st) == WINSIM_OK);   /* 不需要先探测 */
    CHECK(udiff(st.measured_mv, 4969u / 2u) < 25u);           /* 开路电压的一半 */
    CHECK(winsim_set_ratio(&g_ctx, 1.0f, &st) == WINSIM_OK);
    CHECK(st.mask == 0u && !st.short_0r && isinf(st.actual_r_ohm));
    CHECK(winsim_set_ratio(&g_ctx, -0.1f, &st) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_ratio(&g_ctx, 1.5f, &st) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_ratio(&g_ctx, NAN, &st) == WINSIM_ERR_PARAM);
}

static void test_resistance(void)
{
    static const float targets[] = { 40.0f, 150.0f, 1000.0f, 3300.0f, 10000.0f, 22000.0f, 47000.0f, 120000.0f };
    const float g_lsb = 1.0f / (360000.0f + 5.0f);
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);
    for (unsigned i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        CHECK(winsim_set_resistance(&g_ctx, targets[i], &st) == WINSIM_OK);
        float rel = fabsf(st.actual_r_ohm - targets[i]) / targets[i];
        /* 误差不超过一个最低位电导相对目标的比例 */
        CHECK(rel <= g_lsb * targets[i] + 0.001f);
    }
    CHECK(winsim_set_resistance(&g_ctx, INFINITY, &st) == WINSIM_OK);
    CHECK(mock_is_hiz() && isinf(st.actual_r_ohm));
    CHECK(winsim_set_resistance(&g_ctx, -1.0f, &st) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_resistance(&g_ctx, NAN, &st) == WINSIM_ERR_PARAM);
}

/* 被动测量：原车开关并在 K2 上，电阻阵保持高阻，由电压反推开关电阻。 */
static void test_passive_measure(void)
{
    static const float sw[] = { 5100.0f, 2200.0f, 1000.0f, 330.0f, 47.0f };
    winsim_hal_t h = mock_reset();
    uint16_t mv = 0u;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 1u, false, NULL) == WINSIM_OK);
    CHECK(isnan(winsim_equiv_ohm(&g_ctx, 3000u)));            /* 还没有空载电压 */
    CHECK(winsim_probe_open_voltage(&g_ctx, NULL) == WINSIM_OK);   /* 开关松开 = 开路 */

    unsigned writes_before = 0u;
    for (unsigned i = 0; i < M.nlog; i++)
        if (M.log[i][0] == 'w' && strcmp(M.log[i], "w0000") != 0)
            writes_before++;
    float worst = 0.0f;
    for (unsigned i = 0; i < sizeof sw / sizeof sw[0]; i++) {
        M.ph.ch[1].ext_r = sw[i];
        CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
        float r = winsim_equiv_ohm(&g_ctx, mv);
        float rel = fabsf(r - sw[i]) / sw[i];
        if (rel > worst)
            worst = rel;
        CHECK(rel < 0.03f);
    }
    printf("  original switch 47 Ω–5.1 kΩ: worst %.2f%%\n", (double)(worst * 100.0f));
    unsigned writes_after = 0u;
    for (unsigned i = 0; i < M.nlog; i++)
        if (M.log[i][0] == 'w' && strcmp(M.log[i], "w0000") != 0)
            writes_after++;
    CHECK(writes_after == writes_before);   /* 被动测量从不下拉 */

    M.ph.ch[1].ext_r = INFINITY;
    CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
    CHECK(isinf(winsim_equiv_ohm(&g_ctx, mv)));
    CHECK(winsim_equiv_ohm(&g_ctx, 0u) == 0.0f);

    /* 正在下拉时调用：先断开再测。 */
    CHECK(winsim_set_voltage(&g_ctx, 2000u, NULL) == WINSIM_OK);
    CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
    CHECK(mock_is_hiz() && udiff(mv, g_ctx.last_open_mv) <= 2u);
}

/* 脉冲供电的上拉：判断出来，只取接通期间的读数，按键电压照样准。 */
static void test_pulsed(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    uint16_t vo = 0u;
    M.ph.ch[0].pulse_period_ms = 8u;
    M.ph.ch[0].pulse_on_ms = 3u;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_OK);
    CHECK(g_ctx.src[0].pulsed);
    CHECK(udiff(vo, 4969u) <= 2u);
    CHECK(winsim_identify(&g_ctx, NULL, NULL) == WINSIM_OK);
    CHECK(fabsf(g_ctx.src[0].rth_ohm - 2188.0f) < 20.0f);
    unsigned worst = 0u;
    for (unsigned t = 400u; t <= 4400u; t += 500u) {
        CHECK(winsim_set_voltage(&g_ctx, (uint16_t)t, &st) == WINSIM_OK);
        unsigned e = udiff(st.measured_mv, t);
        if (e > worst)
            worst = e;
        CHECK(e < 20u);
    }
    printf("  pulsed pull-up (3 ms on / 8 ms): open %u mV, worst %u mV\n", vo, worst);

    /* 常供电的一路不会被误判。 */
    CHECK(winsim_select_channel(&g_ctx, 1u, true, NULL) == WINSIM_OK);
    CHECK(!g_ctx.src[1].pulsed);
}

/* ------------------------------------------------------------------ 空闲保持 */

static void test_idle_hold(void)
{
    winsim_hal_t h = mock_reset_idle();
    winsim_idle_t id;
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);

    /* K2：原车开关空闲时接 10 kΩ，现在已拔掉；学到的空闲电压就是 10 kΩ 时的电压。 */
    uint16_t idle = (uint16_t)(idle_mv_for(&M.ph.ch[1], 10000.0f) + 0.5f);
    M.track_min = true;
    M.adc_min = 0xFFFFu;
    CHECK(winsim_idle_hold(&g_ctx, 1u, idle, &id) == WINSIM_OK);
    M.track_min = false;
    CHECK(id.on && M.ph.ch[1].idle_on && M.ph.vpot && g_ctx.channel == 1u);
    CHECK(udiff(id.measured_mv, idle) <= 6u);
    CHECK(M.adc_min + 15u >= idle);            /* 建立过程中从没低于空闲 */
    CHECK(M.idle_unpowered == 0u);
    printf("  idle 10 kOhm (%u mV): code %u, %u mV, lowest reading %u mV\n", idle, id.code, id.measured_mv, M.adc_min);
    /* 码值写入在接通继电器之前，升压在两者之前。 */
    int ivp = -1, ip = -1, icon = -1;
    for (unsigned i = 0; i < M.nlog; i++) {
        if (ivp < 0 && strcmp(M.log[i], "vp1") == 0) ivp = (int)i;
        if (ip < 0 && strcmp(M.log[i], "p1") == 0) ip = (int)i;
        if (icon < 0 && strcmp(M.log[i], "i11") == 0) icon = (int)i;
    }
    CHECK(ivp >= 0 && ivp < ip && ip < icon);

    /* 在 K2 上按键：电阻阵并在空闲支路上，闭环到目标；松开回到空闲。 */
    CHECK(winsim_identify(&g_ctx, NULL, NULL) == WINSIM_OK);
    CHECK(fabsf(g_ctx.src[1].rth_ohm - 2188.0f) < 20.0f);   /* 扣掉了空闲支路 */
    CHECK(M.ph.ch[1].idle_on);                              /* 辨识时空闲支路一直接着 */
    for (unsigned t = 300u; t <= 3300u; t += 600u) {
        CHECK(winsim_set_voltage(&g_ctx, (uint16_t)t, &st) == WINSIM_OK);
        CHECK(udiff(st.measured_mv, t) < 15u);
        CHECK(M.ph.ch[1].idle_on);
    }
    CHECK(winsim_set_hiz(&g_ctx) == WINSIM_OK);
    uint16_t mv = 0u;
    CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
    CHECK(udiff(mv, idle) <= 6u);

    /* K1：原车开关还接着（空闲 10 kΩ）：开路电压就等于空闲电压，不接空闲支路。 */
    M.ph.ch[0].ext_r = 10000.0f;
    CHECK(winsim_idle_hold(&g_ctx, 0u, idle, &id) == WINSIM_OK);
    CHECK(!id.on && !M.ph.ch[0].idle_on);
    /* K3：空闲本来就是开路。 */
    CHECK(winsim_idle_hold(&g_ctx, 2u, 4970u, &id) == WINSIM_OK);
    CHECK(!id.on && !M.ph.ch[2].idle_on);

    /* K4：几种空闲电阻都能保持，K2 的保持不受影响。 */
    static const float r_idle[] = { 2700.0f, 4700.0f, 22000.0f, 47000.0f };
    for (unsigned i = 0; i < sizeof r_idle / sizeof r_idle[0]; i++) {
        uint16_t t = (uint16_t)(idle_mv_for(&M.ph.ch[3], r_idle[i]) + 0.5f);
        CHECK(winsim_idle_hold(&g_ctx, 3u, t, &id) == WINSIM_OK);
        CHECK(udiff(id.measured_mv, t) <= 15u);   /* 2.7 kΩ 时一个码值约 22 mV */
        printf("  idle %5.0f ohm: target %u mV, code %u, %u mV\n", (double)r_idle[i], t, id.code, id.measured_mv);
    }
    CHECK(M.ph.ch[1].idle_on && M.ph.ch[3].idle_on);

    /* 超出范围：空闲电阻 1 kΩ（低于 2.2 kΩ 固定电阻）、150 kΩ（高于 52 kΩ 且离开路更近）。 */
    uint16_t low = (uint16_t)idle_mv_for(&M.ph.ch[3], 1000.0f);
    CHECK(winsim_idle_hold(&g_ctx, 3u, low, &id) == WINSIM_ERR_RANGE);
    CHECK(!id.on && !M.ph.ch[3].idle_on);
    uint16_t high = (uint16_t)idle_mv_for(&M.ph.ch[3], 150000.0f);
    CHECK(winsim_idle_hold(&g_ctx, 3u, high, &id) == WINSIM_ERR_RANGE);
    CHECK(!M.ph.ch[3].idle_on);

    /* 上拉又硬又低的车（5.4 V 经 470 Ω）：空闲 2.4 kΩ 时电流约 1.9 mA，超过 1.8 mA，拒绝。 */
    M.ph.ch[3].vs_mv = 5400.0f;
    M.ph.ch[3].rpu = 470.0f;
    uint16_t hi_i = (uint16_t)idle_mv_for(&M.ph.ch[3], 2400.0f);
    CHECK(winsim_idle_hold(&g_ctx, 3u, hi_i, &id) == WINSIM_ERR_RANGE);
    CHECK(!M.ph.ch[3].idle_on);

    /* 释放：最后一路断开时关升压。 */
    CHECK(winsim_idle_release(&g_ctx, 1u) == WINSIM_OK);
    CHECK(!M.ph.ch[1].idle_on && !M.ph.vpot);
    CHECK(M.bad_states == 0u && M.hot_switches == 0u && M.idle_unpowered == 0u);
}

static unsigned feed_for(winsim_learn_t *l, uint16_t mv, int jitter, uint32_t ms, uint16_t *cap)
{
    unsigned hits = 0u;
    for (uint32_t t = 0; t < ms; t += 50u) {
        int v = (int)mv + (int)((t / 50u) % 3u) * jitter - jitter;
        if (winsim_learn_feed(l, (uint16_t)(v < 0 ? 0 : v), 50u, cap))
            hits++;
    }
    return hits;
}

static void test_learn(void)
{
    winsim_learn_t l;
    uint16_t cap = 0u;
    winsim_learn_begin(&l, NULL, 4970u);

    CHECK(feed_for(&l, 4970u, 3, 300u, &cap) == 0u);         /* 空闲 */
    CHECK(feed_for(&l, 3480u, 8, 900u, &cap) == 1u);         /* 按住 0.9 s：记一次 */
    CHECK(udiff(cap, 3480u) <= 8u);
    CHECK(feed_for(&l, 3480u, 8, 900u, &cap) == 0u);         /* 一直按着：不重复记 */
    CHECK(feed_for(&l, 4970u, 3, 200u, &cap) == 0u);         /* 松开 */
    CHECK(feed_for(&l, 2510u, 5, 300u, &cap) == 0u);         /* 只按 0.3 s：不记 */
    CHECK(feed_for(&l, 2510u, 5, 400u, &cap) == 1u);         /* 继续按满 0.5 s 以上 */
    CHECK(udiff(cap, 2510u) <= 5u);
    CHECK(feed_for(&l, 4970u, 3, 200u, &cap) == 0u);
    CHECK(feed_for(&l, 1550u, 120, 2000u, &cap) == 0u);      /* 抖动 120 mV：一直不稳定 */
    CHECK(feed_for(&l, 4900u, 0, 200u, &cap) == 0u);         /* 只低 70 mV：不算按下 */
    CHECK(feed_for(&l, 680u, 0, 600u, &cap) == 1u);
    CHECK(cap == 680u);

    winsim_cfg_t c = winsim_default_cfg();
    c.learn_stable_ms = 200u;
    winsim_learn_begin(&l, &c, 4970u);
    CHECK(feed_for(&l, 3000u, 0, 300u, &cap) == 1u);
}

/* ------------------------------------------------------------------ 两片 TCA9539 + AD5292 寄存器模型 */

typedef struct {
    uint8_t out[2], cfg[2];
} tca_t;

static struct {
    bool     reset_low;
    tca_t    a, b;           /* U801 @0x74, U808 @0x75 */
    int      nack_in;        /* 再成功写 N 次后 NACK 一次；-1 = 不注入 */
    unsigned writes;
    unsigned unsafe;         /* 出现过不允许的“输出且为高”的开关引脚 */
    uint16_t allowed;        /* U801 允许为高的引脚 */
    uint8_t  allowed_b0, allowed_b1;
    uint16_t raw_fixed;      /* 非 0 时 ADC 直接返回该码值 */
    phys_t   ph;
    /* AD5292 SPI 解码 */
    uint8_t  last_b0;
    uint16_t shift[WINSIM_NUM_CH];
    unsigned nbits[WINSIM_NUM_CH];
    bool     wr_en[WINSIM_NUM_CH];
    unsigned frames, bad_frames, rdac_writes;
} D;

#define ADDR_PINS 0xC000u   /* U801 P16/P17 是通道地址线，为高不算危险 */

static uint16_t tca_high(const tca_t *t)
{
    if (D.reset_low)
        return 0u;
    uint16_t out = (uint16_t)(t->out[0] | (t->out[1] << 8));
    uint16_t is_out = (uint16_t)~(t->cfg[0] | (t->cfg[1] << 8));
    return (uint16_t)(out & is_out);
}

/* U808 的引脚电平：复位或输入时 SYNC、IDLE_N 被上拉为高，其余被下拉为低。 */
static uint8_t b_level(unsigned port)
{
    uint8_t pullups = port == 0u ? WINSIM_TCA9539_B0_SYNC_ALL : WINSIM_TCA9539_B1_IDLE_ALL;
    if (D.reset_low)
        return pullups;
    uint8_t is_out = (uint8_t)~D.b.cfg[port];
    return (uint8_t)((D.b.out[port] & is_out) | (pullups & ~is_out));
}

static void dev_sync_phys(void)
{
    uint8_t b0 = b_level(0u), b1 = b_level(1u);
    D.ph.vpot = (b0 & WINSIM_TCA9539_B0_VPOT) != 0u;
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++)
        D.ph.ch[k].idle_on = !(b1 & WINSIM_TCA9539_B1_IDLE_N(k));
}

static void dev_spi_edges(uint8_t now)
{
    uint8_t prev = D.last_b0;
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++) {
        uint8_t s = WINSIM_TCA9539_B0_SYNC(k);
        if ((prev & s) && !(now & s)) {          /* SYNC 下降：开始一帧 */
            D.shift[k] = 0u;
            D.nbits[k] = 0u;
        }
        if (!(now & s) && (prev & WINSIM_TCA9539_B0_SCLK) && !(now & WINSIM_TCA9539_B0_SCLK)) {
            D.shift[k] = (uint16_t)((D.shift[k] << 1) | ((now & WINSIM_TCA9539_B0_DIN) ? 1u : 0u));
            D.nbits[k]++;
        }
        if (!(prev & s) && (now & s)) {          /* SYNC 上升：执行 */
            D.frames++;
            if (D.nbits[k] != 16u) {
                D.bad_frames++;
                continue;
            }
            uint16_t w = D.shift[k];
            unsigned cmd = (w >> 10) & 0xFu;
            if (cmd == 6u) {
                D.wr_en[k] = (w & 0x2u) != 0u;
            } else if (cmd == 1u) {
                if (!D.wr_en[k] || !D.ph.vpot)
                    D.bad_frames++;              /* 写保护或没上电时写 RDAC 不生效 */
                else {
                    D.ph.ch[k].idle_code = (uint16_t)(w & 0x3FFu);
                    D.rdac_writes++;
                }
            }
        }
    }
    D.last_b0 = now;
}

static void dev_check(void)
{
    if (tca_high(&D.a) & (uint16_t)~(D.allowed | ADDR_PINS))
        D.unsafe++;
    uint16_t hb = tca_high(&D.b);
    if ((hb & 0xFFu) & (uint8_t)~(D.allowed_b0 | WINSIM_TCA9539_B0_SYNC_ALL))
        D.unsafe++;
    if ((hb >> 8) & (uint8_t)~(D.allowed_b1 | WINSIM_TCA9539_B1_IDLE_ALL))
        D.unsafe++;
    /* 继电器接通但 U808 没输出（复位）不可能；接通时要求升压已开 */
    dev_sync_phys();
}

static void dev_reset_pin(bool high)
{
    D.reset_low = !high;
    if (!high) {
        D.a.out[0] = D.a.out[1] = D.b.out[0] = D.b.out[1] = 0xFFu;   /* 数据手册：复位默认值 */
        D.a.cfg[0] = D.a.cfg[1] = D.b.cfg[0] = D.b.cfg[1] = 0xFFu;
        for (unsigned k = 0; k < WINSIM_NUM_CH; k++) {
            D.wr_en[k] = false;
            D.ph.ch[k].idle_code = 512u;         /* AD5292 RESET：恢复中间值 */
        }
    }
    D.last_b0 = b_level(0u);
    dev_check();
}

static bool dev_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len)
{
    tca_t *t = addr == WINSIM_TCA9539_ADDR ? &D.a : addr == WINSIM_TCA9539_ADDR_B ? &D.b : NULL;
    if (!t || D.reset_low)
        return false;
    if (D.nack_in == 0) {
        D.nack_in = -1;
        return false;
    }
    if (D.nack_in > 0)
        D.nack_in--;
    for (uint8_t k = 0; k < len; k++) {
        uint8_t r = (uint8_t)((reg & 0xFEu) | ((reg + k) & 1u));   /* 寄存器对内自动切换 */
        if (r == 0x02u || r == 0x03u)
            t->out[r - 0x02u] = data[k];
        else if (r == 0x06u || r == 0x07u)
            t->cfg[r - 0x06u] = data[k];
        dev_check();   /* 每个字节后检查一次 */
        if (t == &D.b)
            dev_spi_edges(b_level(0u));
    }
    D.writes++;
    return true;
}

static uint16_t dev_mask(void)
{
    uint16_t hi = tca_high(&D.a), hb = tca_high(&D.b);
    uint16_t m = (uint16_t)((hi & 0xFFu) | (((hi >> 8) & 0x0Fu) << 8) | (((hi >> 13) & 1u) << 12));
    if (hb & WINSIM_TCA9539_B0_B13)
        m |= 0x2000u;
    if ((hb >> 8) & WINSIM_TCA9539_B1_B14)
        m |= 0x4000u;
    return m;
}

static uint16_t dev_adc_raw(void)
{
    if (D.raw_fixed)
        return D.raw_fixed;
    if (D.reset_low)
        return 0u;     /* PG2 低：CH_EN_N 高，回读开关全断 */
    /* 地址线在复位后是输入，由 R825/R826 下拉为 0 */
    uint16_t out = (uint16_t)(D.a.out[0] | (D.a.out[1] << 8));
    uint16_t is_out = (uint16_t)~(D.a.cfg[0] | (D.a.cfg[1] << 8));
    unsigned ch = (unsigned)(((out & is_out) >> 14) & 3u);
    bool sh = (tca_high(&D.a) >> 8) & WINSIM_TCA9539_P1_SHORT;
    float v = phys_port_mv(&D.ph, ch, dev_mask(), sh, 0u);
    float raw = v / (356.0f / 56.0f) / 3300.0f * 65535.0f;
    if (raw > 65535.0f)
        raw = 65535.0f;
    return (uint16_t)(raw + 0.5f);
}

static void dev_delay(uint32_t ms)
{
    (void)ms;
}

static winsim_hal_t dev_bind(int nack_in)
{
    winsim_hal_t h;
    winsim_tca9539_port_t p = winsim_tca9539_default_port();
    memset(&D, 0, sizeof D);
    memset(&h, 0, sizeof h);
    D.nack_in = nack_in;
    D.ph = phys_default();
    D.a.out[0] = D.a.out[1] = D.a.cfg[0] = D.a.cfg[1] = 0xFFu;
    D.b.out[0] = D.b.out[1] = D.b.cfg[0] = D.b.cfg[1] = 0xFFu;
    D.last_b0 = WINSIM_TCA9539_B0_SYNC_ALL;
    p.i2c_write = dev_i2c_write;
    p.reset_pin = dev_reset_pin;
    p.adc_read_raw = dev_adc_raw;
    p.delay_ms = dev_delay;
    CHECK(winsim_tca9539_bind(&p, &h));
    return h;
}

static void test_tca9539(void)
{
    winsim_state_t st;
    uint16_t vo = 0u;

    winsim_hal_t h = dev_bind(-1);
    CHECK(D.reset_low);                       /* 绑定即保持复位 */
    CHECK(h.idle_write && h.idle_connect && h.idle_power);
    h.select_channel(3u);                     /* 复位期间只改影子寄存器 */
    CHECK(D.writes == 0u);
    h.select_channel(0u);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(!D.reset_low);
    CHECK(D.a.out[0] == 0u && D.a.out[1] == 0u);
    CHECK(D.a.cfg[0] == 0u && D.a.cfg[1] == 0u);
    CHECK(D.b.out[0] == WINSIM_TCA9539_B0_SYNC_ALL && D.b.out[1] == WINSIM_TCA9539_B1_IDLE_ALL);
    CHECK(D.b.cfg[0] == 0u && D.b.cfg[1] == WINSIM_TCA9539_B1_CFG);
    CHECK(D.unsafe == 0u);                    /* 释放复位过程中没有任何开关引脚输出高 */
    CHECK(!D.ph.vpot && !D.ph.ch[0].idle_on);

    D.allowed = 0xFFFFu;
    D.allowed_b0 = 0xFFu;
    D.allowed_b1 = 0xFFu;
    h.shift_write(0x7ABCu);
    CHECK(D.a.out[0] == 0xBCu && D.a.out[1] == 0x2Au);       /* b8–b11 = 0xA，b12 → P15 */
    CHECK((D.b.out[0] & WINSIM_TCA9539_B0_B13) && (D.b.out[1] & WINSIM_TCA9539_B1_B14));
    CHECK(dev_mask() == 0x7ABCu);
    h.set_short_0r(true);
    CHECK(D.a.out[1] == 0x3Au);
    h.select_channel(2u);
    CHECK(D.a.out[1] == 0xBAu);                                 /* P17:P16 = 10 */
    h.set_short_0r(false);
    h.shift_write(0u);
    CHECK(D.a.out[0] == 0u && D.a.out[1] == 0x80u);             /* 通道地址保持 */
    CHECK(!(D.b.out[0] & WINSIM_TCA9539_B0_B13) && !(D.b.out[1] & WINSIM_TCA9539_B1_B14));
    CHECK(D.b.out[0] == WINSIM_TCA9539_B0_SYNC_ALL);            /* SPI 线和使能没被动过 */
    h.select_channel(0u);
    CHECK(D.a.out[1] == 0u);

    /* AD5292：先写控制寄存器再写码值，每帧正好 16 位；升压没开时写码值不生效。 */
    h.idle_write(1u, 300u);
    CHECK(D.bad_frames == 1u && D.rdac_writes == 0u);
    h.idle_power(true);
    CHECK(D.ph.vpot);
    unsigned f0 = D.frames;
    h.idle_write(1u, 300u);
    CHECK(D.frames == f0 + 2u && D.bad_frames == 1u && D.rdac_writes == 1u);
    CHECK(D.ph.ch[1].idle_code == 300u && D.ph.ch[0].idle_code == 512u);
    h.idle_write(3u, 1023u);
    CHECK(D.ph.ch[3].idle_code == 1023u);
    h.idle_connect(1u, true);
    CHECK(D.ph.ch[1].idle_on && !D.ph.ch[3].idle_on);
    CHECK(D.b.out[0] == (WINSIM_TCA9539_B0_SYNC_ALL | WINSIM_TCA9539_B0_VPOT));   /* 帧之间 SYNC 全高、SCLK 低 */
    h.idle_connect(1u, false);
    h.idle_power(false);
    CHECK(!D.ph.ch[1].idle_on && !D.ph.vpot);

    /* 端到端：核心 → 适配 → 寄存器 → 物理模型 → ADC 码值 → mV，每路串联电阻不同 */
    D.allowed = 0x3FFFu;                      /* P00–P15 */
    D.allowed_b0 = WINSIM_TCA9539_B0_SCLK | WINSIM_TCA9539_B0_DIN | WINSIM_TCA9539_B0_VPOT | WINSIM_TCA9539_B0_B13;
    D.allowed_b1 = WINSIM_TCA9539_B1_B14;
    D.ph.ch[0].rseries = 6.0f;
    D.ph.ch[2].rseries = 16.0f;
    float rs = 0.0f;
    CHECK(winsim_select_channel(&g_ctx, 2u, true, &rs) == WINSIM_OK);
    CHECK(fabsf(rs - 16.0f) < 0.8f);
    CHECK((D.a.out[1] & WINSIM_TCA9539_P1_CH_MASK) == 0x80u);
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_OK);
    CHECK(udiff(vo, 4969u) <= 2u);
    for (unsigned t = 300u; t <= 4500u; t += 700u) {
        CHECK(winsim_set_voltage(&g_ctx, (uint16_t)t, &st) == WINSIM_OK);
        CHECK(udiff(st.measured_mv, t) < 15u);
    }
    CHECK(winsim_set_voltage(&g_ctx, 0u, &st) == WINSIM_OK);
    CHECK((D.a.out[1] & WINSIM_TCA9539_P1_SHORT) && D.a.out[0] == 0u && (D.a.out[1] & 0x2Fu) == 0u);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, &rs) == WINSIM_OK);
    CHECK(fabsf(rs - 6.0f) < 0.8f);
    CHECK((D.a.out[1] & (uint8_t)~WINSIM_TCA9539_P1_CH_MASK) == 0u && D.a.out[0] == 0u);   /* 换完是高阻 */
    D.ph.ch[0].ext_r = 3300.0f;               /* K1 上并着原车开关 */
    uint16_t mv = 0u;
    CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
    CHECK(fabsf(winsim_equiv_ohm(&g_ctx, mv) - 3300.0f) < 100.0f);
    D.ph.ch[0].ext_r = INFINITY;

    /* 端到端空闲保持：K4 空闲 15 kΩ，经过 SPI 和继电器。 */
    uint16_t idle = (uint16_t)(idle_mv_for(&D.ph.ch[3], 15000.0f) + 0.5f);
    winsim_idle_t id;
    CHECK(winsim_idle_hold(&g_ctx, 3u, idle, &id) == WINSIM_OK);
    CHECK(id.on && D.ph.ch[3].idle_on && D.ph.vpot && D.ph.ch[3].idle_code == id.code);
    CHECK(udiff(id.measured_mv, idle) <= 6u);
    CHECK(D.bad_frames == 1u);                /* 只有上面故意的那一帧 */
    CHECK(D.unsafe == 0u);

    h.set_oe(false);
    CHECK(D.reset_low && tca_high(&D.a) == 0u && tca_high(&D.b) == 0u);
    CHECK(!D.ph.ch[3].idle_on && !D.ph.vpot);   /* 复位：继电器和升压全断 */
    CHECK(winsim_tca9539_error_count() == 0u);

    /* 释放复位后第一次写输出寄存器失败：不能切输出方向，回到复位。 */
    h = dev_bind(0);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(D.reset_low && tca_high(&D.a) == 0u && D.unsafe == 0u);
    CHECK(winsim_tca9539_error_count() == 1u);

    /* U808 的方向寄存器写失败：同样回到复位。 */
    h = dev_bind(3);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(D.reset_low && tca_high(&D.a) == 0u && tca_high(&D.b) == 0u && D.unsafe == 0u);
    CHECK(winsim_tca9539_error_count() == 1u);

    /* 运行中写失败：立刻复位关断（换通道、写电位器也一样）。 */
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    D.allowed = 0xFFFFu;
    D.nack_in = 0;
    h.shift_write(0x00FFu);
    CHECK(D.reset_low && tca_high(&D.a) == 0u);
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    D.nack_in = 0;
    h.select_channel(1u);
    CHECK(D.reset_low);
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    h.idle_power(true);
    D.nack_in = 5;
    h.idle_write(0u, 100u);
    CHECK(D.reset_low && !D.ph.vpot);

    /* 应急关断 */
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    winsim_tca9539_emergency_off();
    CHECK(D.reset_low);

    /* ADC 折算：满量程 = 3.3 V × 356 / 56 ≈ 20.98 V */
    D.raw_fixed = 65535u;
    CHECK(udiff(h.adc_read_mv(), 20979u) <= 1u);
    D.raw_fixed = 16384u;
    CHECK(udiff(h.adc_read_mv(), 5245u) <= 1u);

    winsim_tca9539_port_t bad = winsim_tca9539_default_port();
    CHECK(!winsim_tca9539_bind(&bad, &h));    /* 回调未填 */
    bad.i2c_write = dev_i2c_write;
    bad.reset_pin = dev_reset_pin;
    bad.adc_read_raw = dev_adc_raw;
    bad.delay_ms = dev_delay;
    bad.addr7_b = bad.addr7;                  /* 两片地址相同 */
    CHECK(!winsim_tca9539_bind(&bad, &h));
}

int main(void)
{
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "init order", test_init_order },
        { "init params", test_init_params },
        { "channel select", test_channel_select },
        { "identification", test_calibration },
        { "full range, nominal", test_full_range_nominal },
        { "full range, tolerances", test_full_range_tolerance },
        { "other cars' pull-ups", test_other_pullups },
        { "0 V / 0 ohm", test_zero },
        { "switching order", test_switch_order },
        { "over-voltage", test_ovp },
        { "APM asleep", test_no_supply },
        { "readback verify", test_verify },
        { "auto sweep", test_sweep },
        { "ratio", test_ratio },
        { "resistance", test_resistance },
        { "passive measure", test_passive_measure },
        { "pulsed pull-up", test_pulsed },
        { "idle hold", test_idle_hold },
        { "key learning", test_learn },
        { "TCA9539 + AD5292 adapter", test_tca9539 },
    };
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_fail;
        printf("[%s]\n", tests[i].name);
        tests[i].fn();
        printf("  %s\n", g_fail == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
