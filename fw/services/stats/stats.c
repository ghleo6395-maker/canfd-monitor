#include "services/stats/stats.h"
#include "services/can_core/bittime.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/frame_ring/frame_ring.h"

static ch_stats_t    st[CH_NUM];
static ring_reader_t rd;
static uint32_t      win_start;
static uint64_t      win_busy_ns[CH_NUM];
static uint32_t      win_frames[CH_NUM], win_err[CH_NUM], win_valid[CH_NUM];
static bool          abnormal_reported[CH_NUM];

void stats_init(void)
{
    for (int i = 0; i < CH_NUM; i++) {
        st[i] = (ch_stats_t){ 0 };
        win_busy_ns[i] = 0;
        win_frames[i] = win_err[i] = win_valid[i] = 0;
        abnormal_reported[i] = false;
    }
    ring_reader_attach(&rd, true);
    win_start = app_now_ms();
}

void stats_clear(int ch)
{
    ch_stats_t keep = st[ch];
    st[ch] = (ch_stats_t){ 0 };
    st[ch].fps = keep.fps;
    st[ch].load_permille = keep.load_permille;
}

void stats_handle(const app_evt_t *e)
{
    if (e->type == EV_CAN_DROPS && e->ch < CH_NUM) {
        if (e->b == 2) st[e->ch].ingress_drops += (uint32_t)e->a;
        else if (e->b == 3) st[e->ch].ring_drops += (uint32_t)e->a;
        else if (e->b == 4) st[e->ch].tx_echo_drops += (uint32_t)e->a;
        else st[e->ch].hw_drops += (uint32_t)e->a;
    }
}

void stats_tick(void)
{
    const cfg_t *c = cfg_get();
    const can_frame_t *f;
    while ((f = ring_peek(&rd)) != NULL) {
        int ch = f->ch;
        if (ch < CH_NUM) {
            if (f->flags & CANF_ERR) {
                st[ch].err_total++;
                win_err[ch]++;
            } else {
                if (f->flags & CANF_TX)
                    st[ch].tx_total++;
                else {
                    st[ch].rx_total++;
                    win_valid[ch]++;
                }
                if (f->flags & CANF_FD)
                    st[ch].fd_total++;
                if (f->flags & CANF_EXT)
                    st[ch].ext_total++;
                win_frames[ch]++;
                win_busy_ns[ch] += can_frame_time_ns(f->len, f->flags, c->nbr[ch], c->dbr[ch]);
            }
        }
        ring_advance(&rd);
    }

    uint32_t now = app_now_ms();
    if (now - win_start < 1000)
        return;
    uint32_t span = now - win_start;
    win_start = now;
    for (int ch = 0; ch < CH_NUM; ch++) {
        ch_stats_t *s = &st[ch];
        s->fps = win_frames[ch] * 1000u / span;
        uint64_t permille = win_busy_ns[ch] / ((uint64_t)span * 1000u);
        s->load_permille = (uint16_t)(permille > 1000 ? 1000 : permille);
        if (s->load_permille > s->load_peak)
            s->load_peak = s->load_permille;
        s->err_rate = (uint16_t)win_err[ch];
        s->valid_last_s = (uint16_t)win_valid[ch];
        s->load_hist[s->hist_pos] = s->load_permille;
        s->hist_pos = (uint8_t)((s->hist_pos + 1) % 60);
        /* 需求 F6.5：1 s 内错误很多、有效帧为 0 → 提示速率不对或只有一个发送节点 */
        bool abnormal = win_err[ch] >= 20 && win_valid[ch] == 0;
        if (abnormal && !abnormal_reported[ch])
            evt_msg(MSG_BUS_ABNORMAL, (uint8_t)ch, 0);
        abnormal_reported[ch] = abnormal;
        win_busy_ns[ch] = 0;
        win_frames[ch] = win_err[ch] = win_valid[ch] = 0;
    }
}

const ch_stats_t *stats_get(int ch)
{
    return &st[ch];
}

uint32_t stats_ring_drops(void)
{
    return ring_dropped();
}
