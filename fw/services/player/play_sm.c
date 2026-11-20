#include "services/player/play_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/tx_sched/tx_sm.h"

#include <string.h>

#define CHK_CHMAP  0x01u
#define CHK_RATE   0x02u
#define SHIFT_STEP_MS 500u

static player_t pl[CH_NUM];

static const char *const st_name[PL_STATE_NUM] = { "空闲", "检查中", "回放中", "暂停" };
static const char *const speed_name[4] = { "0.5×", "1×", "2×", "尽快" };
static const uint16_t speed_x10[4] = { 5, 10, 20, 0 };

const char *play_state_name(play_state_t s) { return s < PL_STATE_NUM ? st_name[s] : "?"; }
const char *play_speed_name(uint8_t idx) { return speed_name[idx & 3]; }
const player_t *play_get(int ch) { return &pl[ch]; }

bool play_any_active(void)
{
    for (int ch = 0; ch < CH_NUM; ch++)
        if (pl[ch].state != PL_IDLE)
            return true;
    return false;
}

void play_init(void)
{
    for (int ch = 0; ch < CH_NUM; ch++)
        pl[ch] = (player_t){ .state = PL_IDLE, .file = -1 };
}

static void to_idle(int ch, msg_t why)
{
    player_t *p = &pl[ch];
    if (p->state == PL_IDLE)
        return;
    p->state = PL_IDLE;
    p->asking = ASK_NONE;
    evt_msg(why, (uint8_t)ch, (int32_t)p->frames_sent);
    if (why == MSG_PLAY_DONE && p->fd_skipped)
        evt_msg(MSG_PLAY_FD_SKIPPED, (uint8_t)ch, (int32_t)p->fd_skipped);
}

bool play_file_in_use(int idx)
{
    for (int ch = 0; ch < CH_NUM; ch++)
        if (pl[ch].state != PL_IDLE && pl[ch].file == idx) return true;
    return false;
}

void play_file_removed(int idx)
{
    for (int ch = 0; ch < CH_NUM; ch++) {
        if (pl[ch].file == idx) {
            to_idle(ch, MSG_PLAY_STOPPED);
            pl[ch].file = -1;
        } else if (pl[ch].file > idx) pl[ch].file--;
    }
}

static void start(int ch)
{
    player_t *p = &pl[ch];
    p->state = PL_PLAYING;
    p->asking = ASK_NONE;
    p->vt_ms = 0;
    p->vt_us = 0;
    p->vt_remainder = 0;
    p->next_frame = 0;
    p->frames_sent = 0;
    p->fd_skipped = 0;
    p->error_skipped = 0;
    p->shifts = 0;
    p->loops_done = 0;
    p->shift_acc_ms = 0;
    evt_msg(MSG_PLAY_STARTED, (uint8_t)ch, p->file);
}

/* 逐条提问；都问完了再看要不要切到正常模式 */
static void next_question(int ch)
{
    player_t *p = &pl[ch];
    file_info_t fi;
    port_sd_file_info(p->file, &fi);
    if (p->pending & CHK_CHMAP) {
        p->asking = ASK_PLAY_CHMAP;
        evt_ask(ASK_PLAY_CHMAP, (uint8_t)ch, fi.channels, SRC_UI);
        return;
    }
    if (p->pending & CHK_RATE) {
        p->asking = ASK_PLAY_RATE;
        evt_ask(ASK_PLAY_RATE, (uint8_t)ch, (int32_t)fi.nbr, SRC_UI);
        return;
    }
    if (!chan_rate_allows_tx(ch)) {
        to_idle(ch, MSG_CH_LOW_RATE);
        return;
    }
    if (!chan_can_tx(ch) || chan_get(ch)->first_tx_pending) {
        p->asking = ASK_PLAY_NORMAL;
        evt_ask(ASK_PLAY_NORMAL, (uint8_t)ch, 0, SRC_UI);
        return;
    }
    start(ch);
}

static void open_file(int ch, int idx)
{
#ifdef REVD_FREERTOS
    if (rec_get()->state != REC_IDLE) {
        evt_msg(MSG_PLAY_BUSY, (uint8_t)ch, 0); return;
    }
#endif
    player_t *p = &pl[ch];
    msg_t why;
    if (p->state != PL_IDLE) {
        evt_msg(MSG_PLAY_BUSY, (uint8_t)ch, 0);
        return;
    }
    if (!storage_readable(&why)) {
        evt_msg(why, (uint8_t)ch, 0);
        return;
    }
    file_info_t fi;
    if (!port_sd_file_info(idx, &fi) || fi.kind == FK_UNKNOWN) {
        evt_msg(MSG_PLAY_BAD_FILE, (uint8_t)ch, idx);   /* 首行标识认不出（F4.7） */
        return;
    }
    uint32_t count;
    uint64_t duration_us;
    if (!port_sd_replay_info(idx, (uint8_t)ch, &count, &duration_us)) {
        evt_msg(MSG_PLAY_BAD_FILE, (uint8_t)ch, idx);
        return;
    }
    const cfg_t *c = cfg_get();
    *p = (player_t){ .state = PL_CHECKING, .file = idx,
                     .duration_ms = (uint32_t)((duration_us + 999u) / 1000u),
                     .duration_us = duration_us, .frames_total = count, .file_fd = fi.fd };
    if (fi.channels > 2)
        p->pending |= CHK_CHMAP;
    if (fi.nbr && (fi.nbr != c->nbr[ch] || fi.fd != c->fd[ch] || (fi.fd && fi.dbr != c->dbr[ch])))
        p->pending |= CHK_RATE;
    next_question(ch);
}

static void answer(int ch, const app_evt_t *e)
{
    player_t *p = &pl[ch];
    if (p->state != PL_CHECKING || (ask_t)e->a != p->asking)
        return;
    file_info_t fi;
    port_sd_file_info(p->file, &fi);
    switch (p->asking) {
    case ASK_PLAY_CHMAP:
        if (!e->b) {
            to_idle(ch, MSG_PLAY_STOPPED);
            return;
        }
        p->pending &= (uint8_t)~CHK_CHMAP;
        break;
    case ASK_PLAY_RATE:
        if (e->b) {                         /* 套用文件参数；否则按当前参数继续，FD 帧发不了就跳过 */
            evt_post4(EV_REQ_CH_RATE, (uint8_t)ch, SRC_UI, (int32_t)fi.nbr, (int32_t)fi.dbr);
            evt_post4(EV_REQ_CH_FD, (uint8_t)ch, SRC_UI, fi.fd, 0);
            cfg_mut()->nbr[ch] = fi.nbr;    /* 让下面的速率检查看到新值 */
        }
        p->pending &= (uint8_t)~CHK_RATE;
        break;
    case ASK_PLAY_NORMAL:
        if (!e->b) {
            to_idle(ch, MSG_PLAY_STOPPED);
            return;
        }
        evt_post4(EV_REQ_CH_MODE, (uint8_t)ch, SRC_UI, CAN_MODE_NORMAL, 2);
        chan_clear_first_tx(ch);
        start(ch);
        return;
    default:
        return;
    }
    next_question(ch);
}

/* A source channel never migrates to another target implicitly: CH1/CAN0 in
 * the log maps to physical CAN1, CH2/CAN1 maps to physical CAN2. */
static void send_one(int ch)
{
    player_t *p = &pl[ch];
    can_frame_t f;
    if (!port_sd_read_frame(p->file, (uint8_t)ch, p->next_frame, &f)) {
        to_idle(ch, MSG_PLAY_BAD_FILE);
        return;
    }
    if (f.flags & CANF_ERR) {
        p->error_skipped++;
        p->next_frame++;
        return;
    }
    if ((f.flags & CANF_FD) && !cfg_get()->fd[ch]) {
        p->fd_skipped++;
        p->next_frame++;
        return;
    }
    f.ch = (uint8_t)ch;
    /* TX direction and ESI describe the captured controller; target hardware
     * generates its own ESI and TX echo. Preserve payload/ID/FD/BRS/RTR only. */
    f.flags &= CANF_EXT | CANF_FD | CANF_BRS | CANF_RTR;
    if (tx_submit(ch, &f, TXSRC_REPLAY)) {
        p->next_frame++;
        p->frames_sent++;
    }
}

void play_handle(const app_evt_t *e)
{
    int ch = e->ch < CH_NUM ? e->ch : 0;
    player_t *p = &pl[ch];
    switch (e->type) {
    case EV_REQ_PLAY_OPEN:
        open_file(ch, e->a);
        break;
    case EV_REQ_PLAY_ANSWER:
        answer(ch, e);
        break;
    case EV_REQ_PLAY_CTRL:
        switch ((play_ctl_t)e->a) {
        case PLAYCTL_PAUSE_TOGGLE:
            if (p->state == PL_PLAYING)
                p->state = PL_PAUSED;
            else if (p->state == PL_PAUSED) {
                if (chan_can_tx(ch))
                    p->state = PL_PLAYING;
                else
                    evt_msg(MSG_PLAY_PAUSED_MODE, (uint8_t)ch, 0);
            }
            break;
        case PLAYCTL_STOP:
            to_idle(ch, MSG_PLAY_STOPPED);
            break;
        case PLAYCTL_STEP:
#ifdef REVD_FREERTOS
            if (rec_get()->state != REC_IDLE) { evt_msg(MSG_PLAY_BUSY, (uint8_t)ch, 0); break; }
#endif
            if (p->state == PL_PAUSED && chan_can_tx(ch) && p->next_frame < p->frames_total) {
                can_frame_t next;
                uint32_t before = p->next_frame;
                bool readable = port_sd_read_frame(p->file, (uint8_t)ch, before, &next);
                send_one(ch);
                if (readable && p->next_frame != before && next.ts_us > p->vt_us) {
                    p->vt_us = next.ts_us;
                    p->vt_ms = (uint32_t)(p->vt_us / 1000u);
                }
                if (p->next_frame >= p->frames_total) {
                    if (cfg_get()->play_loop) {
                        p->loops_done++;
                        p->next_frame = 0;
                        p->vt_us = 0;
                        p->vt_ms = 0;
                        p->vt_remainder = 0;
                    } else to_idle(ch, MSG_PLAY_DONE);
                }
            }
            break;
        case PLAYCTL_SPEED_NEXT:
            cfg_mut()->play_speed_idx = (uint8_t)((cfg_get()->play_speed_idx + 1) & 3);
            break;
        case PLAYCTL_LOOP_TOGGLE:
            cfg_mut()->play_loop = !cfg_get()->play_loop;
            break;
        }
        break;
    case EV_NTF_STORAGE_LOST:
        for (int i = 0; i < CH_NUM; i++)
            to_idle(i, MSG_PLAY_SD_REMOVED);
        break;
    case EV_SYS_STOP_ALL:
        for (int i = 0; i < CH_NUM; i++)
            to_idle(i, MSG_PLAY_STOPPED);
        break;
    default:
        break;
    }
}

void play_tick(uint32_t dt_ms)
{
    const cfg_t *c = cfg_get();
    for (int ch = 0; ch < CH_NUM; ch++) {
        player_t *p = &pl[ch];
        if (p->state != PL_PLAYING)
            continue;
        if (!chan_can_tx(ch)) {
            p->state = PL_PAUSED;            /* 通道离开了正常模式 / Bus-Off */
            evt_msg(MSG_PLAY_PAUSED_MODE, (uint8_t)ch, 0);
            continue;
        }
#ifdef REVD_FREERTOS
        if (rec_get()->state != REC_IDLE || port_sd_busy()) {
#else
        if (rec_get()->state == REC_RUNNING && port_sd_busy()) {
#endif
            p->shift_acc_ms += dt_ms;        /* SD 读不过来：时间轴整体顺延 */
            if (p->shift_acc_ms >= SHIFT_STEP_MS) {
                p->shift_acc_ms = 0;
                p->shifts++;
            }
            continue;
        }
        uint16_t sx = speed_x10[c->play_speed_idx & 3];
        if (sx) {
            uint64_t scaled = (uint64_t)dt_ms * 1000u * sx + p->vt_remainder;
            p->vt_us += scaled / 10u;
            p->vt_remainder = (uint8_t)(scaled % 10u);
            uint64_t ms = p->vt_us / 1000u;
            p->vt_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
        }
        /* A bounded burst respects the scheduler FIFO. Timing uses each file
         * record's relative microseconds instead of frame-count interpolation. */
        for (int n = 0; p->next_frame < p->frames_total && n < (sx ? 8 : 4); n++) {
            can_frame_t next;
            if (!port_sd_read_frame(p->file, (uint8_t)ch, p->next_frame, &next)) {
                to_idle(ch, MSG_PLAY_BAD_FILE);
                break;
            }
            if (sx && next.ts_us > p->vt_us) break;
            uint32_t before = p->next_frame;
            send_one(ch);
            if (p->next_frame == before || p->state != PL_PLAYING)
                break;
        }
        if (p->state != PL_PLAYING) continue;
        if (p->next_frame >= p->frames_total) {
            if (c->play_loop) {
                p->loops_done++;
                p->next_frame = 0;
                p->vt_ms = 0;
                p->vt_us = 0;
                p->vt_remainder = 0;
            } else {
                to_idle(ch, MSG_PLAY_DONE);
            }
        }
    }
}
