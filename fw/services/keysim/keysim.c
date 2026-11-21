/* SPDX-License-Identifier: MIT */
#include "services/keysim/keysim.h"
#include "services/core/port.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* 当前 Rev D/E 目标板没有第 12 页硬件；在这些目标上不分配驱动上下文。 */
#if defined(CANFD_REVD_TARGET) && !defined(CANFD_WINSIM_HW)
#define KEYSIM_HW 0
#else
#define KEYSIM_HW 1
#endif

static const char *const k_state_names[] = { "空闲", "上升", "一键上升", "下降", "一键下降", "故障" };

const char *keysim_state_name(int state)
{
    static char buf[24];   /* "状态 " 占 7 字节 + 最多 11 个字符 + 结尾 */
    if (state >= 0 && state < (int)(sizeof k_state_names / sizeof k_state_names[0]))
        return k_state_names[state];
    if (state < 0)
        return "未知";
    snprintf(buf, sizeof buf, "状态 %d", state);
    return buf;
}

#if !KEYSIM_HW

static const keysim_t k_stub;
void            keysim_init(void) {}
void            keysim_tick(uint32_t ms) { (void)ms; }
const keysim_t *keysim_get(void) { return &k_stub; }
bool            keysim_available(void) { return false; }
bool            keysim_state_known(void) { return false; }
int             keysim_apm_state(void) { return -1; }
winsim_status_t keysim_select(uint8_t ch) { (void)ch; return WINSIM_ERR_PARAM; }
winsim_status_t keysim_recalibrate(void) { return WINSIM_ERR_PARAM; }
winsim_status_t keysim_press(int index) { (void)index; return WINSIM_ERR_PARAM; }
winsim_status_t keysim_manual_toggle(void) { return WINSIM_ERR_PARAM; }
void            keysim_manual_adjust(int delta_mv) { (void)delta_mv; }
void            keysim_release(void) {}
void            keysim_set_press_mode(ks_press_mode_t mode) { (void)mode; }
winsim_status_t keysim_clear_ovp(void) { return WINSIM_ERR_PARAM; }
void            keysim_leave(void) {}
winsim_status_t keysim_learn_start(void) { return WINSIM_ERR_PARAM; }
void            keysim_learn_stop(void) {}
void            keysim_learn_rename(int index) { (void)index; }
void            keysim_learn_clear(void) {}
bool            keysim_learn_save(void) { return false; }
winsim_status_t keysim_scan_start(void) { return WINSIM_ERR_PARAM; }
void            keysim_scan_stop(void) {}
bool            keysim_scan_save(void) { return false; }

#else

#define IDLE_POLL_MS  300u
#define LEARN_POLL_MS 50u

static const char *const k_key_names[] = { "上升", "一键上升", "下降", "一键下降", "对地短路", "自定义" };
#define KEY_NAME_N ((int)(sizeof k_key_names / sizeof k_key_names[0]))

static winsim_ctx_t   ws;            /* 约 33 KB */
static winsim_learn_t learner;
static keysim_t       K;
static uint32_t       idle_t, learn_t, tap_t, scan_t;
static bool           scan_waiting;
static int16_t        scan_prev;
static uint16_t       scan_prev_mv;

const keysim_t *keysim_get(void) { return &K; }
bool keysim_available(void) { return K.available; }

int keysim_apm_state(void)
{
    return K.available ? port_keysim_apm_state(K.ch) : -1;
}

bool keysim_state_known(void)
{
    return keysim_apm_state() >= 0;
}

static void copy_name(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

static void sync_channel(void)
{
    K.ch = ws.channel;
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++) {
        K.calibrated[k] = ws.series_cal[k];
        K.r_series[k] = ws.series_cal[k] ? ws.r_series_ch[k] : ws.cfg.r_series_ohm;
    }
}

static winsim_status_t note(winsim_status_t rc)
{
    K.last_status = rc;
    if (rc == WINSIM_ERR_OVERVOLTAGE || ws.ovp_latched) {
        K.ovp = true;
        K.activity = KS_IDLE;
        K.pressed = -1;
    }
    K.port_mv = ws.current_state.measured_mv ? ws.current_state.measured_mv : K.port_mv;
    return rc;
}

void keysim_init(void)
{
    winsim_hal_t hal;
    memset(&K, 0, sizeof K);
    K.pressed = -1;
    K.press_mode = KS_TAP;
    K.tap_ms = 300u;
    K.manual_mv = 2500u;
    idle_t = learn_t = tap_t = scan_t = 0u;
    scan_waiting = false;
    if (!port_keysim_hal(&hal) || winsim_init(&ws, &hal, NULL) != WINSIM_OK)
        return;
    K.available = true;
    K.channels = ws.cfg.num_channels;
    sync_channel();
}

static void stop_activity(void)
{
    if (K.activity == KS_LEARNING || K.activity == KS_SCANNING || K.activity == KS_PRESSING)
        winsim_set_hiz(&ws);
    K.activity = KS_IDLE;
    K.pressed = -1;
    scan_waiting = false;
}

void keysim_release(void)
{
    if (!K.available)
        return;
    if (K.activity == KS_PRESSING)
        stop_activity();
    else
        winsim_set_hiz(&ws);
    K.pressed = -1;
}

void keysim_leave(void)
{
    if (!K.available)
        return;
    stop_activity();
}

winsim_status_t keysim_select(uint8_t ch)
{
    float r = 0.0f;
    if (!K.available || ch >= K.channels)
        return WINSIM_ERR_PARAM;
    stop_activity();
    winsim_status_t rc = winsim_select_channel(&ws, ch, true, &r);
    sync_channel();
    K.supply_ok = ws.last_open_mv != 0u;
    return note(rc);
}

winsim_status_t keysim_recalibrate(void)
{
    if (!K.available)
        return WINSIM_ERR_PARAM;
    stop_activity();
    winsim_status_t rc = winsim_calibrate_series(&ws, NULL);
    sync_channel();
    return note(rc);
}

static winsim_status_t drive(int8_t which, uint16_t mv)
{
    winsim_state_t st;
    winsim_status_t rc = winsim_set_voltage(&ws, mv, &st);
    note(rc);
    if (rc == WINSIM_OK || rc == WINSIM_ERR_VERIFY_FAIL) {
        K.activity = KS_PRESSING;
        K.pressed = which;
        K.pressed_mv = mv;
        K.port_mv = st.measured_mv;
        tap_t = 0u;
    } else {
        winsim_set_hiz(&ws);
        K.activity = KS_IDLE;
        K.pressed = -1;
    }
    if (rc == WINSIM_ERR_NO_SUPPLY)
        K.supply_ok = false;
    return rc;
}

winsim_status_t keysim_press(int index)
{
    const ks_table_t *t = K.available ? &K.tables[K.ch] : NULL;
    if (!t || !t->valid || index < 0 || index >= t->n)
        return WINSIM_ERR_PARAM;
    if (K.activity == KS_LEARNING || K.activity == KS_SCANNING)
        return WINSIM_ERR_PARAM;
    if (K.press_mode == KS_LATCH && K.pressed == index) {
        keysim_release();
        return WINSIM_OK;
    }
    return drive((int8_t)index, t->keys[index].mv);
}

winsim_status_t keysim_manual_toggle(void)
{
    if (!K.available || K.activity == KS_LEARNING || K.activity == KS_SCANNING)
        return WINSIM_ERR_PARAM;
    if (K.pressed == KS_MANUAL) {
        keysim_release();
        return WINSIM_OK;
    }
    winsim_status_t rc = drive(KS_MANUAL, K.manual_mv);
    return rc;
}

void keysim_manual_adjust(int delta_mv)
{
    int v = (int)K.manual_mv + delta_mv;
    if (v < 0)
        v = 0;
    if (v > 4900)
        v = 4900;
    K.manual_mv = (uint16_t)v;
    if (K.available && K.pressed == KS_MANUAL)
        drive(KS_MANUAL, K.manual_mv);
}

void keysim_set_press_mode(ks_press_mode_t mode)
{
    if (!K.available)
        return;
    if (K.pressed >= 0)
        keysim_release();
    K.press_mode = mode;
}

winsim_status_t keysim_clear_ovp(void)
{
    if (!K.available)
        return WINSIM_ERR_PARAM;
    winsim_status_t rc = winsim_clear_ovp(&ws);
    K.last_status = rc;
    K.port_mv = ws.current_state.measured_mv;
    if (rc == WINSIM_OK)
        K.ovp = false;
    return rc;
}

/* ------------------------------------------------------------------ 学习 */

winsim_status_t keysim_learn_start(void)
{
    uint16_t idle = 0u;
    if (!K.available || K.ovp)
        return K.ovp ? WINSIM_ERR_OVERVOLTAGE : WINSIM_ERR_PARAM;
    stop_activity();
    winsim_status_t rc = winsim_probe_open_voltage(&ws, &idle);
    note(rc);
    K.port_mv = idle;
    if (rc != WINSIM_OK) {
        K.supply_ok = rc != WINSIM_ERR_NO_SUPPLY && K.supply_ok;
        return rc;
    }
    K.supply_ok = true;
    winsim_learn_begin(&learner, &ws.cfg, idle);
    K.learn_idle_mv = idle;
    K.learn_n = 0u;
    K.learn_progress = 0u;
    K.activity = KS_LEARNING;
    learn_t = 0u;
    return WINSIM_OK;
}

void keysim_learn_stop(void)
{
    if (K.available && K.activity == KS_LEARNING)
        K.activity = KS_IDLE;
}

void keysim_learn_clear(void)
{
    K.learn_n = 0u;
    K.learn_progress = 0u;
    if (K.available && K.activity == KS_LEARNING)
        winsim_learn_begin(&learner, &ws.cfg, K.learn_idle_mv);
}

void keysim_learn_rename(int index)
{
    if (index < 0 || index >= K.learn_n)
        return;
    ks_key_t *k = &K.learn_keys[index];
    int cur = -1;
    for (int i = 0; i < KEY_NAME_N; i++)
        if (strcmp(k->name, k_key_names[i]) == 0)
            cur = i;
    copy_name(k->name, sizeof k->name, k_key_names[(cur + 1) % KEY_NAME_N]);
}

bool keysim_learn_save(void)
{
    if (!K.available || K.learn_n == 0u)
        return false;
    ks_table_t *t = &K.tables[K.ch];
    memset(t, 0, sizeof *t);
    t->valid = true;
    snprintf(t->name, sizeof t->name, "学习 · K%u", (unsigned)K.ch + 1u);
    t->n = K.learn_n;
    memcpy(t->keys, K.learn_keys, sizeof t->keys);
    K.learn_n = 0u;
    keysim_learn_stop();
    return true;
}

static void learn_poll(void)
{
    uint16_t mv = 0u, cap = 0u;
    winsim_status_t rc = winsim_measure(&ws, &mv);
    K.port_mv = mv;
    if (rc != WINSIM_OK) {
        note(rc);
        return;
    }
    if (winsim_learn_feed(&learner, mv, LEARN_POLL_MS, &cap)) {
        bool dup = false;
        for (unsigned i = 0; i < K.learn_n; i++)
            if ((K.learn_keys[i].mv > cap ? K.learn_keys[i].mv - cap : cap - K.learn_keys[i].mv) < 60u)
                dup = true;
        if (!dup && K.learn_n < KS_MAX_KEYS) {
            ks_key_t *k = &K.learn_keys[K.learn_n];
            copy_name(k->name, sizeof k->name, k_key_names[K.learn_n < 4u ? K.learn_n : KEY_NAME_N - 1]);
            k->mv = cap;
            k->ohm = winsim_equiv_ohm(&ws, cap);
            K.learn_n++;
        }
    }
    K.learn_progress = learner.armed && learner.stable_ms
        ? (uint8_t)(learner.stable_for_ms >= learner.stable_ms ? 100u : learner.stable_for_ms * 100u / learner.stable_ms)
        : 0u;
}

/* ------------------------------------------------------------------ 扫描 */

winsim_status_t keysim_scan_start(void)
{
    uint16_t open = 0u;
    if (!K.available || K.ovp)
        return K.ovp ? WINSIM_ERR_OVERVOLTAGE : WINSIM_ERR_PARAM;
    stop_activity();
    winsim_status_t rc = winsim_probe_open_voltage(&ws, &open);
    note(rc);
    if (rc != WINSIM_OK) {
        if (rc == WINSIM_ERR_NO_SUPPLY)
            K.supply_ok = false;
        return rc;
    }
    K.supply_ok = true;
    if (!keysim_state_known())
        return WINSIM_ERR_PARAM;   /* 读不到 APM 开关状态，扫描没有意义 */
    K.scan_mv = KS_SCAN_START_MV;
    K.scan_done = false;
    K.scan_n = 0u;
    scan_prev = (int16_t)keysim_apm_state();
    scan_prev_mv = open;
    K.activity = KS_SCANNING;
    scan_waiting = false;
    scan_t = 0u;
    return WINSIM_OK;
}

void keysim_scan_stop(void)
{
    if (K.available && K.activity == KS_SCANNING)
        stop_activity();
}

static void scan_finish(void)
{
    winsim_set_hiz(&ws);
    K.activity = KS_IDLE;
    K.scan_done = true;
    scan_waiting = false;
}

static void scan_poll(void)
{
    if (!scan_waiting) {
        winsim_state_t st;
        winsim_status_t rc = winsim_set_voltage(&ws, K.scan_mv, &st);
        if (rc != WINSIM_OK && rc != WINSIM_ERR_VERIFY_FAIL) {
            note(rc);
            stop_activity();
            return;
        }
        K.port_mv = st.measured_mv;
        scan_waiting = true;
        scan_t = 0u;
        return;
    }
    if (scan_t < KS_SCAN_HOLD_MS)
        return;
    int16_t s = (int16_t)keysim_apm_state();
    if (s != scan_prev && K.scan_n < KS_MAX_EDGES) {
        ks_edge_t *e = &K.scan_edges[K.scan_n++];
        e->from = scan_prev;
        e->to = s;
        e->above_mv = scan_prev_mv;
        e->below_mv = K.port_mv;
    }
    scan_prev = s;
    scan_prev_mv = K.port_mv;
    scan_waiting = false;
    if (K.scan_mv < KS_SCAN_STEP_MV) {
        scan_finish();
        return;
    }
    K.scan_mv = (uint16_t)(K.scan_mv - KS_SCAN_STEP_MV);
}

bool keysim_scan_save(void)
{
    if (!K.available || !K.scan_done || K.scan_n == 0u)
        return false;
    ks_table_t *t = &K.tables[K.ch];
    memset(t, 0, sizeof *t);
    t->valid = true;
    snprintf(t->name, sizeof t->name, "扫描 · K%u", (unsigned)K.ch + 1u);
    /* 第 i 个门限以下、第 i+1 个门限以上是状态 edges[i].to 的区间，取中点。 */
    for (unsigned i = 0; i < K.scan_n && t->n < KS_MAX_KEYS; i++) {
        const ks_edge_t *e = &K.scan_edges[i];
        uint32_t hi = ((uint32_t)e->above_mv + e->below_mv) / 2u;
        uint32_t lo = i + 1u < K.scan_n ? ((uint32_t)K.scan_edges[i + 1u].above_mv + K.scan_edges[i + 1u].below_mv) / 2u : 0u;
        ks_key_t *k = &t->keys[t->n++];
        bool last = i + 1u == K.scan_n;
        copy_name(k->name, sizeof k->name, last && lo == 0u && e->to == 5 ? "对地短路" : keysim_state_name(e->to));
        k->mv = last && lo == 0u && e->to == 5 ? 0u : (uint16_t)(((hi + lo) / 2u + 5u) / 10u * 10u);
        k->ohm = NAN;
    }
    K.scan_done = false;
    return true;
}

/* ------------------------------------------------------------------ 节拍 */

void keysim_tick(uint32_t ms)
{
    if (!K.available)
        return;
    switch (K.activity) {
    case KS_PRESSING:
        if (K.press_mode == KS_TAP && K.pressed >= 0) {
            tap_t += ms;
            if (tap_t >= K.tap_ms)
                keysim_release();
        }
        break;
    case KS_LEARNING:
        learn_t += ms;
        while (learn_t >= LEARN_POLL_MS && K.activity == KS_LEARNING) {
            learn_t -= LEARN_POLL_MS;
            learn_poll();
        }
        break;
    case KS_SCANNING:
        scan_t += ms;
        scan_poll();
        break;
    case KS_IDLE:
        idle_t += ms;
        if (idle_t >= IDLE_POLL_MS) {
            uint16_t mv = 0u;
            idle_t = 0u;
            winsim_status_t rc = winsim_measure(&ws, &mv);
            K.port_mv = mv;
            if (rc == WINSIM_ERR_OVERVOLTAGE)
                K.ovp = true;
            else
                K.supply_ok = mv >= ws.cfg.min_open_mv || (K.supply_ok && mv > 0u);
        }
        break;
    }
}

#endif /* KEYSIM_HW */
