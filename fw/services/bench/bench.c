/* SPDX-License-Identifier: MIT */
#include "services/bench/bench.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/tx_sched/tx_sm.h"

static bench_t state;
static uint32_t rotate_due, refresh_due;
static bool was_recording;

static bool valid(const bench_config_t *c)
{
    return c && c->segment_kib >= 64u && c->segment_kib <= 65536u &&
           (!c->rotate_ms || (c->rotate_ms >= 250u && c->rotate_ms <= 3600000u)) &&
           (!c->refresh_ms || (c->refresh_ms >= 50u && c->refresh_ms <= 60000u));
}
static bool tx_busy(void)
{
    if (tx_any_active()) return true;
    for (int ch = 0; ch < CH_NUM; ch++)
        if (tx_get()->pending[ch]) return true;
    return false;
}
static const char *deny(const char *reason)
{
    state.request_rejected++;
    return reason;
}
void bench_init(void)
{
    uint32_t epoch = state.diag_epoch + 1u;
    state = (bench_t){.config = {1024u, 0u, 0u}, .diag_epoch = epoch};
    cfg_rec_seg_override(0);
    rotate_due = refresh_due = 0;
    was_recording = false;
}
const bench_t *bench_get(void) { return &state; }
static bool request_rotate(void)
{
    if (!evt_post4(EV_REQ_REC_ROTATE, 0, SRC_HOST, 0, 0)) return false;
    state.rotations_requested++;
    return true;
}
const char *bench_execute(bench_action_t action, const bench_config_t *config)
{
    if (action == BENCH_ROTATE) {
        if (!state.enabled) return deny("ERR BENCH_DISABLED\r\n");
        if (!power_running() || rec_get()->state != REC_RUNNING)
            return deny("ERR BENCH_NOT_RECORDING\r\n");
        if (!request_rotate()) return deny("ERR BENCH_EVENT_FULL\r\n");
        return "OK BENCH_ROTATE_QUEUED\r\n";
    }
    if (action != BENCH_CONFIG && action != BENCH_ON && action != BENCH_OFF)
        return deny("ERR BENCH_ACTION\r\n");
    if (rec_get()->state != REC_IDLE) return deny("ERR BENCH_REC_BUSY\r\n");
    if (action == BENCH_CONFIG) {
        if (!valid(config)) return deny("ERR BENCH_RANGE\r\n");
        state.config = *config;
        if (state.enabled) cfg_rec_seg_override(state.config.segment_kib);
        rotate_due = app_now_ms() + state.config.rotate_ms;
        refresh_due = app_now_ms() + state.config.refresh_ms;
        return "OK BENCH_CONFIG\r\n";
    }
    if (action == BENCH_OFF) {
        state.enabled = false;
        cfg_rec_seg_override(0);
        was_recording = false;
        return "OK BENCH_OFF\r\n";
    }
    if (!power_running()) return deny("ERR BENCH_NOT_RUNNING\r\n");
    if (storage_get()->state != ST_READY) return deny("ERR BENCH_STORAGE\r\n");
    if (play_any_active() || tx_busy()) return deny("ERR BENCH_TX_BUSY\r\n");
    state.enabled = true;
    cfg_rec_seg_override(state.config.segment_kib);
    rotate_due = app_now_ms() + state.config.rotate_ms;
    refresh_due = app_now_ms() + state.config.refresh_ms;
    was_recording = false;
    power_wake();
    power_activity();
    return "OK BENCH_ON\r\n";
}
void bench_tick(void)
{
    if (!state.enabled || !power_running()) return;
    uint32_t now = app_now_ms();
    power_wake();
    power_activity();
    bool recording = rec_get()->state == REC_RUNNING;
    if (recording && !was_recording) rotate_due = now + state.config.rotate_ms;
    was_recording = recording;
    if (recording && state.config.rotate_ms && (int32_t)(now - rotate_due) >= 0) {
        /* No catch-up burst after card latency: one fence per service call. */
        rotate_due = now + state.config.rotate_ms;
        if (!request_rotate()) state.request_rejected++;
    }
    if (state.config.refresh_ms && (int32_t)(now - refresh_due) >= 0) {
        refresh_due = now + state.config.refresh_ms;
        port_screen_redraw();
        state.redraws_requested++;
    }
}
