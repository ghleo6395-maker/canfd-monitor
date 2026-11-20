#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/recorder/rec_sm.h"

#define MIN_TX_RATE      50000u
#define BUSOFF_MAX_TRIES 5u
#define BUSOFF_CALM_MS   10000u    /* 恢复后稳定这么久，退避次数清零 */
#define TMR_BO(ch)       ((ch) ? TMR_BUSOFF1 : TMR_BUSOFF0)

static chan_t   chs[CH_NUM];
static uint32_t last_recover_ms[CH_NUM];
static bool start_failed[CH_NUM];

static const uint32_t ab_nbr[] = { 500000, 250000, 1000000, 125000, 800000, 100000,
                                   83333, 50000, 33333, 20000, 10000 };
static const uint32_t ab_dbr[] = { 2000000, 5000000, 4000000, 1000000 };
#define AB_NBR_N (sizeof ab_nbr / sizeof ab_nbr[0])
#define AB_DBR_N (sizeof ab_dbr / sizeof ab_dbr[0])

static const char *const st_name[CH_STATE_NUM] = { "待机", "只听", "受限", "正常", "Bus-Off", "自动识别" };

const char *chan_state_name(ch_state_t s) { return s < CH_STATE_NUM ? st_name[s] : "?"; }
const chan_t *chan_get(int ch) { return &chs[ch]; }

bool chan_rate_allows_tx(int ch)
{
    return cfg_get()->nbr[ch] >= MIN_TX_RATE;
}

bool chan_can_tx(int ch)
{
    return chs[ch].state == CH_NORMAL;
}

void chan_clear_first_tx(int ch)
{
    chs[ch].first_tx_pending = false;
}

uint8_t chan_ab_progress(int ch)
{
    const chan_t *c = &chs[ch];
    if (c->state != CH_AUTOBAUD)
        return 0;
    unsigned done = c->ab_phase == 0 ? c->ab_idx : (unsigned)AB_NBR_N;
    return (uint8_t)(done * 100u / AB_NBR_N);
}

static ch_state_t mode_state(can_mode_t m)
{
    return m == CAN_MODE_NORMAL ? CH_NORMAL : m == CAN_MODE_RESTRICTED ? CH_RESTRICTED : CH_LISTEN;
}

/* 停 → 配置 → 启动（需求 3.5 第 2 条）。控制器重启后错误计数清零。 */
static bool apply_mode(int ch, can_mode_t m)
{
    const cfg_t *c = cfg_get();
    chan_t *s = &chs[ch];
    bool started=port_can_start(ch, c->nbr[ch], c->dbr[ch], c->fd[ch], m);
    s->restart_seq++;
    start_failed[ch]=!started;
    if (!started) {
        s->state=CH_STANDBY;s->mode=CAN_MODE_LISTEN;s->first_tx_pending=false;
        return false;
    }
    s->mode = m;
    s->state = mode_state(m);
    s->tec = s->rec = 0;
    s->errst = ERRST_ACTIVE;
    return true;
}

static void cfg_changed(int ch)
{
    evt_post4(EV_NTF_CH_CFG_CHANGED, (uint8_t)ch, SRC_SYS, 0, 0);
}

void chan_init(void)
{
    for (int ch = 0; ch < CH_NUM; ch++) {
        chs[ch] = (chan_t){ .state = CH_STANDBY, .mode = CAN_MODE_LISTEN };
        tmr_stop(TMR_BO(ch));
        last_recover_ms[ch] = 0;
        start_failed[ch] = false;
    }
}

/* ---- 自动识别（F1.5）---- */
static void ab_probe(int ch)
{
    chan_t *s = &chs[ch];
    s->restart_seq++;
    bool started=s->ab_phase==0?
        port_can_probe(ch, ab_nbr[s->ab_idx], 0, false, s->ab_mode):
        port_can_probe(ch, cfg_get()->nbr[ch], ab_dbr[s->ab_idx], true, s->ab_mode);
    start_failed[ch]=!started;
    if (!started) {
        s->state=CH_STANDBY;s->mode=CAN_MODE_LISTEN;s->first_tx_pending=false;
        evt_msg(MSG_AB_FAIL,(uint8_t)ch,0);cfg_changed(ch);
    }
}

static void ab_finish(int ch, bool ok)
{
    chan_t *s = &chs[ch];
    const cfg_t *c = cfg_get();
    if (!ok && !c->rate_saved[ch]) {
        port_can_standby(ch);              /* 从没配置过速率：识别失败回到待机 */
        s->state = CH_STANDBY;
    } else {
        if (!apply_mode(ch, CAN_MODE_LISTEN)) { /* 识别结束一律回到只听 */
            evt_msg(MSG_AB_FAIL,(uint8_t)ch,0);cfg_changed(ch);return;
        }
    }
    if (ok) {
        evt_msg(MSG_AB_OK, (uint8_t)ch, (int32_t)c->nbr[ch]);
        cfg_changed(ch);
    } else if (s->ab_err_total > 0 && s->ab_mode == CAN_MODE_LISTEN) {
        evt_ask(ASK_AB_RESTRICTED, (uint8_t)ch, 0, SRC_UI);   /* 可能只有一个发送节点 */
    } else if (s->ab_err_total == 0) {
        evt_msg(MSG_AB_NO_TRAFFIC, (uint8_t)ch, 0);
    } else {
        evt_msg(MSG_AB_FAIL, (uint8_t)ch, 0);
    }
}

static void ab_result(int ch, int32_t valid, int32_t errs, bool fd_seen)
{
    chan_t *s = &chs[ch];
    cfg_t *c = cfg_mut();
    s->ab_err_total = (uint16_t)(s->ab_err_total + errs);
    if (fd_seen)
        s->ab_fd_seen = true;
    bool hit = valid >= 2 && errs == 0;

    if (s->ab_phase == 0) {
        if (hit) {
            c->nbr[ch] = ab_nbr[s->ab_idx];
            c->rate_saved[ch] = true;
            if (s->ab_fd_seen) {           /* 见到 FD 帧：继续识别数据段速率 */
                s->ab_phase = 1;
                s->ab_idx = 0;
                ab_probe(ch);
                return;
            }
            c->fd[ch] = false;
            ab_finish(ch, true);
        } else if (++s->ab_idx < AB_NBR_N) {
            ab_probe(ch);
        } else {
            ab_finish(ch, false);
        }
        return;
    }
    if (hit) {
        c->dbr[ch] = ab_dbr[s->ab_idx];
        c->fd[ch] = true;
        ab_finish(ch, true);
    } else if (++s->ab_idx < AB_DBR_N) {
        ab_probe(ch);
    } else {
        c->fd[ch] = true;                  /* 标称段识别出来了，数据段没识别出来，保留原值 */
        ab_finish(ch, true);
    }
}

/* ---- 总线状态、Bus-Off（F1.8、F6.1）---- */
static void on_status(int ch, const app_evt_t *e)
{
    chan_t *s = &chs[ch];
    s->tec = (uint16_t)e->a;
    s->rec = (uint16_t)(e->b & 0xFFFF);
    bool busoff = (e->b >> 16) & 1;
    s->errst = busoff ? ERRST_BUSOFF
             : (s->tec >= 128 || s->rec >= 128) ? ERRST_PASSIVE
             : (s->tec >= 96 || s->rec >= 96) ? ERRST_WARNING : ERRST_ACTIVE;
    if (!busoff || s->state != CH_NORMAL)
        return;
    s->state = CH_BUSOFF;
    s->busoff_count++;
    if (app_now_ms() - last_recover_ms[ch] > BUSOFF_CALM_MS)
        s->busoff_tries = 0;
    evt_msg(MSG_CH_BUSOFF, (uint8_t)ch, 0);
    if (!cfg_get()->busoff_auto)
        return;                            /* 默认手动恢复 */
    if (s->busoff_tries >= BUSOFF_MAX_TRIES) {
        evt_msg(MSG_CH_BUSOFF_GIVEUP, (uint8_t)ch, 0);
        return;
    }
    tmr_start(TMR_BO(ch), 100u << s->busoff_tries);   /* 退避：0.1 / 0.2 / 0.4 / 0.8 / 1.6 s */
}

static void recover(int ch)
{
    chan_t *s = &chs[ch];
    bool recovered=port_can_recover(ch);
    s->restart_seq++; /* Every attempt discards the controller's old TX FIFO. */
    if (!recovered) {
        s->state=CH_BUSOFF;
        if (cfg_get()->busoff_auto && s->busoff_tries<BUSOFF_MAX_TRIES)
            tmr_start(TMR_BO(ch),100u<<s->busoff_tries);
        else if (cfg_get()->busoff_auto)
            evt_msg(MSG_CH_BUSOFF_GIVEUP,(uint8_t)ch,0);
        return;
    }
    start_failed[ch]=false;
    last_recover_ms[ch] = app_now_ms();
    s->state = CH_NORMAL;
    s->tec = s->rec = 0;
    s->errst = ERRST_ACTIVE;
    evt_msg(MSG_CH_RECOVERED, (uint8_t)ch, 0);
}

/* ---- 请求 ---- */
static void req_mode(int ch, const app_evt_t *e)
{
    chan_t *s = &chs[ch];
    can_mode_t m = (can_mode_t)e->a;
    if (s->state == CH_STANDBY && !start_failed[ch])
        return;
    if (m == CAN_MODE_NORMAL) {
        if (!chan_rate_allows_tx(ch)) {
            evt_msg(MSG_CH_LOW_RATE, (uint8_t)ch, 0);
            return;
        }
        if (e->b == 0) {                   /* 切到正常模式要在设备上确认 */
            evt_ask(ASK_CH_NORMAL, (uint8_t)ch, 0, (src_t)e->src);
            return;
        }
    }
    if (s->state == mode_state(m))
        return;
    if (s->state == CH_AUTOBAUD)
        evt_msg(MSG_AB_ABORTED, (uint8_t)ch, 0);
    tmr_stop(TMR_BO(ch));
    if (!apply_mode(ch, m)) {cfg_changed(ch);return;}
    /* b=1：在设置里切过去的，第一次发送还要确认；b=2：发送流程里已经确认过 */
    s->first_tx_pending = (m == CAN_MODE_NORMAL && e->b == 1);
    evt_msg(MSG_CH_MODE, (uint8_t)ch, (int32_t)m);
    cfg_changed(ch);
}

static void req_rate(int ch, const app_evt_t *e)
{
    chan_t *s = &chs[ch];
    cfg_t *c = cfg_mut();
    if (e->a > 0)
        c->nbr[ch] = (uint32_t)e->a;
    if (e->b > 0)
        c->dbr[ch] = (uint32_t)e->b;
    c->rate_saved[ch] = true;
    if (s->state == CH_STANDBY && !start_failed[ch])
        return;
    if (s->state == CH_AUTOBAUD)
        evt_msg(MSG_AB_ABORTED, (uint8_t)ch, 0);
    can_mode_t m = (s->state == CH_AUTOBAUD || s->state == CH_BUSOFF || s->state == CH_STANDBY) ? CAN_MODE_LISTEN : s->mode;
    if (m == CAN_MODE_NORMAL && !chan_rate_allows_tx(ch)) {
        m = CAN_MODE_LISTEN;
        evt_msg(MSG_CH_LOW_RATE, (uint8_t)ch, 0);
    }
    tmr_stop(TMR_BO(ch));
    if (!apply_mode(ch, m)) {cfg_changed(ch);return;}
    evt_msg(MSG_CH_RATE, (uint8_t)ch, (int32_t)c->nbr[ch]);
    cfg_changed(ch);
}

void chan_handle(int ch, const app_evt_t *e)
{
    chan_t *s = &chs[ch];

    /* Queue congestion may delay a hardware result past a stop/reconfigure.
     * Only observations from this exact controller incarnation are valid. */
    if ((e->type==EV_CAN_STATUS || e->type==EV_CAN_PROBE_RESULT) &&
        e->generation!=port_can_generation(ch)) return;

    switch (e->type) {
    case EV_SYS_STARTED:
        /* 开机强制只听（F8.6）；没保存过速率就保持待机，等用户自动识别 */
        if (cfg_get()->rate_saved[ch])
            apply_mode(ch, CAN_MODE_LISTEN);
        return;
    case EV_SYS_STOP_ALL:
        tmr_stop(TMR_BO(ch));
        port_can_standby(ch);
        s->state = CH_STANDBY;
        start_failed[ch]=false;
        return;
    case EV_TIMER:
        if (e->a == TMR_BO(ch) && s->state == CH_BUSOFF) {
            s->busoff_tries++;             /* 保留次数，下次 Bus-Off 等更久 */
            recover(ch);
        }
        return;
    default:
        break;
    }

    if (e->ch != ch)
        return;

    switch (e->type) {
    case EV_CAN_STATUS:
        on_status(ch, e);
        break;
    case EV_REQ_CH_MODE:
        req_mode(ch, e);
        break;
    case EV_REQ_CH_RATE:
        req_rate(ch, e);
        break;
    case EV_REQ_CH_FD:
        cfg_mut()->fd[ch] = e->a != 0;
        if ((s->state != CH_STANDBY || start_failed[ch]) && s->state != CH_AUTOBAUD) {
            apply_mode(ch, s->state == CH_BUSOFF || s->state == CH_STANDBY ? CAN_MODE_LISTEN : s->mode);
            cfg_changed(ch);
        }
        break;
    case EV_REQ_CH_RECOVER:
        if (s->state == CH_BUSOFF) {
            tmr_stop(TMR_BO(ch));
            s->busoff_tries = 0;
            recover(ch);
        }
        break;
    case EV_REQ_AUTOBAUD:
        if (rec_get()->state != REC_IDLE) {
            evt_msg(MSG_AB_DENY_REC, (uint8_t)ch, 0);   /* 录制中禁止（F1.5） */
            break;
        }
        if (s->state == CH_AUTOBAUD)
            break;
        if (e->a == CAN_MODE_RESTRICTED && e->b == 0) {
            evt_ask(ASK_AB_RESTRICTED, (uint8_t)ch, 0, (src_t)e->src);
            break;
        }
        tmr_stop(TMR_BO(ch));
        s->state = CH_AUTOBAUD;
        s->ab_mode = e->a == CAN_MODE_RESTRICTED ? CAN_MODE_RESTRICTED : CAN_MODE_LISTEN;
        s->ab_phase = 0;
        s->ab_idx = 0;
        s->ab_err_total = 0;
        s->ab_fd_seen = false;
        evt_msg(MSG_AB_START, (uint8_t)ch, s->ab_mode);
        ab_probe(ch);
        break;
    case EV_CAN_PROBE_RESULT:
        if (s->state == CH_AUTOBAUD)
            ab_result(ch, e->a, e->b & 0xFFFF, (e->b >> 16) & 1);
        break;
    default:
        break;
    }
}
