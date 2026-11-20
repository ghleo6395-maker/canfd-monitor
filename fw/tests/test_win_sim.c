/* SPDX-License-Identifier: MIT */
/*
 * win_sim 主机单元测试。Mock HAL 按物理模型仿真 APM 输入：
 * R295 = 2.2 kΩ 上拉到 VCC2_5V_SW，接口侧 400 kΩ 回读负载，12 位电阻阵 + 直通管。
 * 另有一个 TCA9539 寄存器级模型，测试硬件适配的上电顺序、位映射和 I2C 失败处理。
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

static winsim_ctx_t g_ctx;   /* 约 16.5 KB，放在静态区 */

/* ------------------------------------------------------------------ 物理模型 */

typedef struct {
    float vs_mv;       /* APM 内部 VCC2_5V_SW */
    float rpu;         /* R295 */
    float rload;       /* R815 + R816 */
    float rds;         /* 电阻阵开关管 */
    float short_rds;   /* 直通管 */
    float rseries;     /* PTC */
    float rbit[WINSIM_NUM_BITS];
} phys_t;

static const float k_bits[WINSIM_NUM_BITS] = {
    200.0f, 390.0f, 820.0f, 1600.0f, 3300.0f, 6200.0f,
    13000.0f, 24000.0f, 47000.0f, 91000.0f, 180000.0f, 360000.0f,
};

static phys_t phys_default(void)
{
    phys_t p;
    p.vs_mv = 5000.0f;
    p.rpu = 2200.0f;
    p.rload = 400000.0f;
    p.rds = 5.0f;
    p.short_rds = 0.05f;
    p.rseries = 0.0f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        p.rbit[i] = k_bits[i];
    return p;
}

static float phys_port_mv(const phys_t *p, uint16_t mask, bool sh)
{
    float g_in = sh ? 1.0f / p->short_rds : 0.0f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if (mask & (1u << i))
            g_in += 1.0f / (p->rbit[i] + p->rds);
    float g = p->rload > 0.0f ? 1.0f / p->rload : 0.0f;
    if (g_in > 0.0f)
        g += 1.0f / (p->rseries + 1.0f / g_in);
    if (!(g > 0.0f))
        return p->vs_mv;
    float rp = 1.0f / g;
    return p->vs_mv * rp / (rp + p->rpu);
}

static uint16_t clamp_adc_mv(float v)
{
    if (v < 0.0f)
        v = 0.0f;
    if (v > 13200.0f)          /* 1:4 分压 × 3.3 V 满量程 */
        v = 13200.0f;
    return (uint16_t)(v + 0.5f);
}

/* ------------------------------------------------------------------ 核心 Mock HAL */

#define LOG_MAX 512

static struct {
    phys_t   ph;
    bool     oe, sh;
    uint16_t mask;
    char     log[LOG_MAX][8];
    unsigned nlog;
    unsigned bad_states;   /* 直通与电阻阵同时导通的次数 */
    unsigned adc_reads;
    uint32_t delay_total;
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
    snprintf(b, sizeof b, "w%03X", (unsigned)mask);
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

static uint16_t m_adc(void)
{
    M.adc_reads++;
    return clamp_adc_mv(phys_port_mv(&M.ph, M.oe ? M.mask : 0u, M.oe && M.sh));
}

static void m_delay(uint32_t ms)
{
    M.delay_total += ms;
}

static winsim_hal_t mock_reset(void)
{
    winsim_hal_t h;
    memset(&M, 0, sizeof M);
    M.ph = phys_default();
    h.shift_write = m_shift_write;
    h.set_oe = m_set_oe;
    h.set_short_0r = m_set_short;
    h.adc_read_mv = m_adc;
    h.delay_ms = m_delay;
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

/* ------------------------------------------------------------------ 测试 */

static void test_init_order(void)
{
    winsim_hal_t h = mock_reset();
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(M.nlog == 4u);
    CHECK(strcmp(M.log[0], "oe0") == 0);
    CHECK(strcmp(M.log[1], "sh0") == 0);
    CHECK(strcmp(M.log[2], "w000") == 0);
    CHECK(strcmp(M.log[3], "oe1") == 0);

    CHECK(isinf(g_ctx.r_lut[0]));
    CHECK(fabsf(g_ctx.r_lut[1] - 205.0f) < 0.01f);
    float g = 0.0f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        g += 1.0f / (k_bits[i] + 5.0f);
    CHECK(fabsf(g_ctx.r_lut[WINSIM_MASK_ALL] * g - 1.0f) < 1e-4f);
    CHECK(g_ctx.current_state.mask == 0u && !g_ctx.current_state.short_0r);
    CHECK(!g_ctx.ovp_latched && g_ctx.last_open_mv == 0u);
}

static void test_init_params(void)
{
    winsim_hal_t h = mock_reset();
    winsim_hal_t bad = h;
    bad.adc_read_mv = NULL;
    CHECK(winsim_init(&g_ctx, &bad, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_init(NULL, &h, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_init(&g_ctx, NULL, NULL) == WINSIM_ERR_PARAM);

    winsim_cfg_t c = winsim_default_cfg();
    c.min_open_mv = 6000u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c = winsim_default_cfg();
    c.r_bit_ohm[3] = 0.0f;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    CHECK(M.nlog == 0u);   /* 参数错误时不碰硬件 */

    memset(&g_ctx, 0, sizeof g_ctx);
    CHECK(winsim_set_voltage(&g_ctx, 1000u, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_hiz(&g_ctx) == WINSIM_ERR_PARAM);
    CHECK(winsim_auto_sweep(&g_ctx, 0u, 100u, 10u, 0u, NULL, NULL) == WINSIM_ERR_PARAM);
}

static unsigned run_grid(void)
{
    unsigned worst = 0u;
    for (unsigned t = 0u; t <= 4600u; t += 200u) {
        winsim_state_t st;
        winsim_status_t rc = winsim_set_voltage(&g_ctx, (uint16_t)t, &st);
        unsigned e = udiff(st.measured_mv, t);
        CHECK(rc == WINSIM_OK);
        if (e >= 25u)
            printf("  target %4u mV -> %4u mV (mask %03X short %d)\n", t, st.measured_mv,
                   (unsigned)st.mask, (int)st.short_0r);
        CHECK(e < 25u);
        if (e > worst)
            worst = e;
    }
    return worst;
}

static void test_full_range_nominal(void)
{
    winsim_hal_t h = mock_reset();
    uint16_t vo = 0u;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_OK);
    CHECK(udiff(vo, 4973u) <= 1u);   /* 5 V × 400k / 402.2k */
    unsigned worst = run_grid();
    printf("  0–4600 mV / 200 mV, nominal parts: worst %u mV\n", worst);
    CHECK(M.bad_states == 0u);
}

/* 1% 电阻、导通电阻 3 Ω（模型按 5 Ω）：固件仍用标称查找表。 */
static void test_full_range_tolerance(void)
{
    uint32_t seed = 12345u;
    unsigned worst = 0u;
    for (int trial = 0; trial < 20; trial++) {
        winsim_hal_t h = mock_reset();
        for (unsigned i = 0; i < WINSIM_NUM_BITS; i++) {
            seed = seed * 1664525u + 1013904223u;
            float u = (float)(seed >> 8) / 16777216.0f;     /* [0, 1) */
            M.ph.rbit[i] = k_bits[i] * (0.99f + 0.02f * u);
        }
        M.ph.rds = 3.0f;
        CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
        CHECK(winsim_probe_open_voltage(&g_ctx, NULL) == WINSIM_OK);
        unsigned w = run_grid();
        if (w > worst)
            worst = w;
    }
    printf("  0–4600 mV / 200 mV, 20 sets of 1%% parts: worst %u mV\n", worst);
}

static void test_zero(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, NULL) == WINSIM_OK);

    CHECK(winsim_set_voltage(&g_ctx, 0u, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    CHECK(M.sh && M.mask == 0u);
    CHECK(st.measured_mv < 25u);

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
    CHECK(strcmp(M.log[a], "w000") == 0);      /* 先撤电阻阵 */
    CHECK(strcmp(M.log[a + 1u], "sh1") == 0);  /* 再接通直通 */

    unsigned b = M.nlog;
    CHECK(winsim_set_voltage(&g_ctx, 2500u, NULL) == WINSIM_OK);
    CHECK(M.nlog == b + 2u);
    CHECK(strcmp(M.log[b], "sh0") == 0);       /* 先断直通 */
    CHECK(M.log[b + 1u][0] == 'w' && strcmp(M.log[b + 1u], "w000") != 0);
    CHECK(M.bad_states == 0u);
}

static void test_ovp(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    uint16_t vo = 0u;
    M.ph.vs_mv = 12000.0f;   /* 误接 12 V */
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
    CHECK(M.nlog == n);      /* 锁定后没有任何下拉动作 */
    CHECK(mock_is_hiz());

    CHECK(winsim_clear_ovp(&g_ctx) == WINSIM_ERR_OVERVOLTAGE);   /* 仍是 12 V，不解锁 */
    CHECK(g_ctx.ovp_latched);

    M.ph.vs_mv = 5000.0f;
    CHECK(winsim_clear_ovp(&g_ctx) == WINSIM_OK);
    CHECK(!g_ctx.ovp_latched);
    CHECK(winsim_set_voltage(&g_ctx, 2000u, &st) == WINSIM_OK);

    /* 已经下拉时电源跳到 12 V：下一次回读触发保护。 */
    M.ph.vs_mv = 12000.0f;
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(g_ctx.ovp_latched);
    CHECK(mock_is_hiz());
}

static void test_no_supply(void)
{
    winsim_hal_t h = mock_reset();
    uint16_t vo = 1u;
    M.ph.vs_mv = 0.0f;   /* APM 未唤醒 */
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
    M.ph.rbit[7] = 1e9f;   /* 24 kΩ 这一位开路 */
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, NULL) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 4530u, &st) == WINSIM_ERR_VERIFY_FAIL);
    CHECK(st.mask & 0x80u);

    winsim_cfg_t c = winsim_default_cfg();
    c.verify_enable = false;     /* 脉冲供电车型 */
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 4530u, &st) == WINSIM_OK);
}

typedef struct {
    unsigned n;
    uint16_t targets[64];
    unsigned worst_target;     /* |回读 − 目标| */
    unsigned worst_expected;   /* |回读 − 理论| */
    unsigned mismatched;       /* 回调状态与硬件不一致 */
    unsigned inject_at;        /* 第 N 次回调后把电源改成 12 V，0 = 不注入 */
} sweep_rec_t;

static void sweep_cb(const winsim_state_t *st, void *user)
{
    sweep_rec_t *r = (sweep_rec_t *)user;
    if (r->n < 64u)
        r->targets[r->n] = st->target_mv;
    r->n++;
    unsigned et = udiff(st->measured_mv, st->target_mv);
    unsigned ee = udiff(st->measured_mv, st->expected_mv);
    if (et > r->worst_target)
        r->worst_target = et;
    if (ee > r->worst_expected)
        r->worst_expected = ee;
    if (st->mask != M.mask || st->short_0r != M.sh)
        r->mismatched++;
    if (r->inject_at && r->n == r->inject_at)
        M.ph.vs_mv = 12000.0f;
}

static void test_sweep(void)
{
    winsim_hal_t h = mock_reset();
    sweep_rec_t r;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);

    memset(&r, 0, sizeof r);
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 200u, 50u, sweep_cb, &r) == WINSIM_OK);
    CHECK(r.n == 25u);
    for (unsigned i = 0; i < 25u; i++)
        CHECK(r.targets[i] == 4800u - 200u * i);
    CHECK(r.worst_target < 25u);
    CHECK(r.worst_expected <= 1u);
    CHECK(r.mismatched == 0u);
    CHECK(mock_is_hiz());
    CHECK(M.delay_total >= 25u * 50u);
    printf("  sweep 4800→0 mV / 200 mV: %u steps, worst %u mV\n", r.n, r.worst_target);

    memset(&r, 0, sizeof r);
    CHECK(winsim_auto_sweep(&g_ctx, 0u, 1000u, 300u, 0u, sweep_cb, &r) == WINSIM_OK);
    CHECK(r.n == 4u);
    CHECK(r.targets[0] == 0u && r.targets[3] == 900u);
    CHECK(mock_is_hiz());

    /* 4800→0 / 50 mV，含 0–220 mV 无档位区间：按理论值校验，不应误报。 */
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
    CHECK(udiff(st.measured_mv, 2500u) < 25u);
    CHECK(winsim_set_ratio(&g_ctx, 1.0f, &st) == WINSIM_OK);
    CHECK(st.mask == 0u && !st.short_0r && isinf(st.actual_r_ohm));
    CHECK(winsim_set_ratio(&g_ctx, -0.1f, &st) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_ratio(&g_ctx, 1.5f, &st) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_ratio(&g_ctx, NAN, &st) == WINSIM_ERR_PARAM);
}

static void test_resistance(void)
{
    static const float targets[] = { 1000.0f, 3300.0f, 10000.0f, 22000.0f, 47000.0f };
    const float g_lsb = 1.0f / (360000.0f + 5.0f);
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
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

/* ------------------------------------------------------------------ TCA9539 寄存器模型 */

static struct {
    bool     reset_low;
    uint8_t  out[2], cfg[2];
    int      nack_in;      /* 再成功写 N 次后 NACK 一次；-1 = 不注入 */
    unsigned writes;
    unsigned unsafe;       /* 出现过未被允许的“输出且为高”引脚 */
    uint16_t allowed;
    uint16_t raw_fixed;    /* 非 0 时 ADC 直接返回该码值 */
    phys_t   ph;
} D;

static uint16_t dev_driven_high(void)
{
    if (D.reset_low)
        return 0u;
    uint16_t out = (uint16_t)(D.out[0] | (D.out[1] << 8));
    uint16_t is_out = (uint16_t)~(D.cfg[0] | (D.cfg[1] << 8));
    return (uint16_t)(out & is_out);
}

static void dev_check(void)
{
    if (dev_driven_high() & (uint16_t)~D.allowed)
        D.unsafe++;
}

static void dev_reset_pin(bool high)
{
    D.reset_low = !high;
    if (!high) {
        D.out[0] = D.out[1] = 0xFFu;   /* 数据手册：上电/复位默认值 */
        D.cfg[0] = D.cfg[1] = 0xFFu;
    }
    dev_check();
}

static bool dev_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len)
{
    if (addr != WINSIM_TCA9539_ADDR || D.reset_low)
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
            D.out[r - 0x02u] = data[k];
        else if (r == 0x06u || r == 0x07u)
            D.cfg[r - 0x06u] = data[k];
        dev_check();   /* 每个字节后检查一次 */
    }
    D.writes++;
    return true;
}

static uint16_t dev_adc_raw(void)
{
    if (D.raw_fixed)
        return D.raw_fixed;
    uint16_t hi = dev_driven_high();
    uint16_t mask = (uint16_t)((hi & 0xFFu) | (((hi >> 8) & 0x0Fu) << 8));
    bool sh = (hi >> 8) & WINSIM_TCA9539_P1_SHORT;
    float v = phys_port_mv(&D.ph, mask, sh);
    float raw = v / 4.0f / 3300.0f * 65535.0f;
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
    D.nack_in = nack_in;
    D.ph = phys_default();
    D.out[0] = D.out[1] = D.cfg[0] = D.cfg[1] = 0xFFu;
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
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(!D.reset_low);
    CHECK(D.out[0] == 0u && D.out[1] == 0u);
    CHECK(D.cfg[0] == 0u && D.cfg[1] == 0u);
    CHECK(D.unsafe == 0u);                    /* 释放复位过程中没有任何引脚输出高 */

    D.allowed = 0xFFFFu;
    h.shift_write(0x0ABCu);
    CHECK(D.out[0] == 0xBCu && D.out[1] == 0x0Au);
    h.set_short_0r(true);
    CHECK(D.out[1] == 0x1Au);
    h.set_short_0r(false);
    h.shift_write(0u);
    CHECK(D.out[0] == 0u && D.out[1] == 0u);

    /* 端到端：核心 → 适配 → 寄存器 → 物理模型 → ADC 码值 → mV */
    D.allowed = 0x1FFFu;                      /* P00–P14 */
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_OK);
    CHECK(udiff(vo, 4973u) <= 2u);
    for (unsigned t = 500u; t <= 4500u; t += 1000u) {
        CHECK(winsim_set_voltage(&g_ctx, (uint16_t)t, &st) == WINSIM_OK);
        CHECK(udiff(st.measured_mv, t) < 25u);
    }
    CHECK(winsim_set_voltage(&g_ctx, 0u, &st) == WINSIM_OK);
    CHECK((D.out[1] & WINSIM_TCA9539_P1_SHORT) && D.out[0] == 0u && (D.out[1] & 0x0Fu) == 0u);
    CHECK(D.unsafe == 0u);

    h.set_oe(false);
    CHECK(D.reset_low && dev_driven_high() == 0u);
    CHECK(winsim_tca9539_error_count() == 0u);

    /* 释放复位后第一次写输出寄存器失败：不能切输出方向，回到复位。 */
    h = dev_bind(0);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(D.reset_low && dev_driven_high() == 0u && D.unsafe == 0u);
    CHECK(winsim_tca9539_error_count() == 1u);

    /* 写方向寄存器失败：同样回到复位。 */
    h = dev_bind(1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(D.reset_low && dev_driven_high() == 0u && D.unsafe == 0u);
    CHECK(winsim_tca9539_error_count() == 1u);

    /* 运行中写失败：立刻复位关断。 */
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    D.allowed = 0xFFFFu;
    D.nack_in = 0;
    h.shift_write(0x00FFu);
    CHECK(D.reset_low && dev_driven_high() == 0u);

    /* 应急关断 */
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    winsim_tca9539_emergency_off();
    CHECK(D.reset_low);

    /* ADC 折算：满量程 = 3.3 V × 4 */
    D.raw_fixed = 65535u;
    CHECK(h.adc_read_mv() == 13200u);
    D.raw_fixed = 16384u;
    CHECK(udiff(h.adc_read_mv(), 3300u) <= 1u);

    winsim_tca9539_port_t bad = winsim_tca9539_default_port();
    CHECK(!winsim_tca9539_bind(&bad, &h));    /* 回调未填 */
}

int main(void)
{
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "init order", test_init_order },
        { "init params", test_init_params },
        { "full range, nominal", test_full_range_nominal },
        { "full range, 1% parts", test_full_range_tolerance },
        { "0 V / 0 ohm", test_zero },
        { "short switching order", test_switch_order },
        { "over-voltage", test_ovp },
        { "APM asleep", test_no_supply },
        { "readback verify", test_verify },
        { "auto sweep", test_sweep },
        { "ratio", test_ratio },
        { "resistance", test_resistance },
        { "TCA9539 adapter", test_tca9539 },
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
