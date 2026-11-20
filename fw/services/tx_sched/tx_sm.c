#include "services/tx_sched/tx_sm.h"
#include "services/can_core/bittime.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/player/play_sm.h"
#include "sim/tx_sequence.h"

#include <string.h>

#define DWELL_TMR(ch) ((ch) ? TMR_TX_DWELL1 : TMR_TX_DWELL0)

static tx_t t;
static uint32_t restart_seq[CH_NUM];
static tx_result_t last_result[CH_NUM];

const char *tx_result_name(tx_result_t r)
{
    static const char *const names[] = { "ok", "channel", "entry", "null", "id", "flags", "length",
        "fd_disabled", "period", "active", "mode", "fifo_full", "changed", "dynamic" };
    return (unsigned)r < sizeof names / sizeof names[0] ? names[r] : "unknown";
}

const char *tx_result_text(tx_result_t r)
{
    static const char *const names[] = { "已保存", "通道必须为 CAN1 或 CAN2", "发送条目不存在", "数据不能为空",
        "标准 ID 最大 7FF，扩展 ID 最大 1FFFFFFF", "BRS 需要 FD；FD 不支持 RTR；发送标志无效",
        "Classic 长度为 0..8；FD 为 0..8/12/16/20/24/32/48/64",
        "该通道未启用 CAN FD，请先修改通道设置", "周期必须为 0..65535 ms（0 为仅单发）",
        "请先停止该条目的周期发送再编辑", "通道当前不能发送", "发送 FIFO 已满", "条目已被其他界面修改，请取消后重新打开",
        "动态计数或校验参数不合法" };
    return (unsigned)r < sizeof names / sizeof names[0] ? names[r] : "未知错误";
}

tx_result_t tx_last_result(int ch)
{
    return ch >= 0 && ch < CH_NUM ? last_result[ch] : TX_ERR_CHANNEL;
}

tx_result_t tx_entry_update(int ch, int idx, uint32_t id, uint8_t flags,
                           uint8_t len, const uint8_t *data, uint32_t period_ms)
{
    if (ch < 0 || ch >= CH_NUM) return TX_ERR_CHANNEL;
    if (idx < 0 || idx >= TX_ENTRIES) return last_result[ch] = TX_ERR_ENTRY;
    can_frame_t f = { .id = id, .ch = (uint8_t)ch, .flags = flags, .len = len };
    tx_result_t r = tx_validate_frame(ch, &f, false);
    if (r != TX_OK) return last_result[ch] = r;
    if (period_ms > UINT16_MAX) return last_result[ch] = TX_ERR_PERIOD;
    if (len && !(flags & CANF_RTR) && !data) return last_result[ch] = TX_ERR_NULL;
    if (t.e[ch][idx].active) return last_result[ch] = TX_ERR_ACTIVE;
    if (txseq_uses_entry(ch, idx)) return last_result[ch] = TX_ERR_ACTIVE;
    if (data && !(flags & CANF_RTR)) memcpy(f.data, data, len);
    tx_entry_t *en = &t.e[ch][idx];
    en->f = f;
    en->period_ms = (uint16_t)period_ms;
    /* Editing bytes means they are literal. A stale counter/checksum must not
     * silently overwrite a byte entered on the device or host. */
    memset(&en->dyn, 0, sizeof en->dyn);
    en->host_started = false;
    en->next_due = 0;
    en->revision++;
    return last_result[ch] = TX_OK;
}

const tx_t *tx_get(void) { return &t; }

tx_result_t tx_restore_entries(uint8_t mask,const tx_entry_t entries[CH_NUM][TX_ENTRIES])
{
    if(!entries||!mask||(mask&~3u))return TX_ERR_CHANNEL;
    for(int ch=0;ch<CH_NUM;ch++)if(mask&(1u<<ch)) {
        if(t.pending[ch]||play_get(ch)->state!=PL_IDLE)return TX_ERR_ACTIVE;
        if(txseq_get(ch)->state!=TXSEQ_IDLE)return TX_ERR_ACTIVE;
        for(int i=0;i<TX_ENTRIES;i++) {
            if(t.e[ch][i].active)return TX_ERR_ACTIVE;
            tx_result_t rc=tx_validate_frame(ch,&entries[ch][i].f,false);
            if(rc!=TX_OK)return rc;
            rc=tx_validate_dynamic(&entries[ch][i].dyn,&entries[ch][i].f);
            if(rc!=TX_OK)return rc;
        }
    }
    for(int ch=0;ch<CH_NUM;ch++)if(mask&(1u<<ch))for(int i=0;i<TX_ENTRIES;i++) {
        const tx_entry_t *source=&entries[ch][i];tx_entry_t *target=&t.e[ch][i];
        uint32_t revision=target->revision+1;
        memset(target,0,sizeof *target);target->f=source->f;target->f.ch=(uint8_t)ch;
        target->period_ms=source->period_ms;target->dyn=source->dyn;target->revision=revision;
    }
    return TX_OK;
}

static void set_entry(int ch, int i, uint32_t id, uint8_t flags, uint8_t len, const uint8_t *data,
                      uint16_t period, dynfield_t dyn)
{
    tx_entry_t *e = &t.e[ch][i];
    memset(e, 0, sizeof *e);
    e->f.id = id;
    e->f.ch = (uint8_t)ch;
    e->f.flags = flags;
    e->f.len = len;
    if (data)
        memcpy(e->f.data, data, len);
    e->period_ms = period;
    e->dyn = dyn;
}

/* 模拟器里的预设发送列表；固件里从 SD 卡 /CANFD/tx/ 下的 CSV 加载（F2.5） */
static void load_defaults(void)
{
    static const uint8_t d0[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00, 0x00 };
    static const uint8_t d1[8] = { 0xFF, 0xFF, 0x00, 0x7D, 0x00, 0x00, 0xFF, 0xFF };
    static const uint8_t d3[8] = { 0x02, 0x01, 0x0D, 0x55, 0x55, 0x55, 0x55, 0x55 };   /* OBD 读车速 */
    static const uint8_t e0[8] = { 0x00, 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
    static const uint8_t e1[4] = { 0xA5, 0x5A, 0x01, 0x02 };
    uint8_t fd64[64], fd12[12];
    for (int i = 0; i < 64; i++)
        fd64[i] = (uint8_t)i;
    for (int i = 0; i < 12; i++)
        fd12[i] = (uint8_t)(0xC0 + i);

    set_entry(0, 0, 0x123, 0, 8, d0, 100,
              (dynfield_t){ .cnt_byte = 6, .cnt_bits = 4, .cnt_max = 15, .chk = CHK_CRC8_J1850, .chk_byte = 7, .chk_from = 0, .chk_to = 6 });
    set_entry(0, 1, 0x18FF50E5, CANF_EXT, 8, d1, 500, (dynfield_t){ 0 });
    set_entry(0, 2, 0x3A0, CANF_FD | CANF_BRS, 64, fd64, 20,
              (dynfield_t){ .cnt_byte = 0, .cnt_bits = 8, .cnt_max = 255, .chk = CHK_XOR, .chk_byte = 63, .chk_from = 0, .chk_to = 62 });
    set_entry(0, 3, 0x7DF, 0, 8, d3, 0, (dynfield_t){ 0 });
    set_entry(1, 0, 0x200, 0, 8, e0, 10,
              (dynfield_t){ .cnt_byte = 1, .cnt_bits = 4, .cnt_max = 14, .chk = CHK_CRC8_H2F, .chk_byte = 0, .chk_from = 1, .chk_to = 7 });
    set_entry(1, 1, 0x201, 0, 4, e1, 50, (dynfield_t){ 0 });
    set_entry(1, 2, 0x1FFFFFF0, CANF_EXT | CANF_FD, 12, fd12, 100, (dynfield_t){ 0 });
    set_entry(1, 3, 0x100, CANF_RTR, 8, NULL, 0, (dynfield_t){ 0 });
}

void tx_init(void)
{
    memset(&t, 0, sizeof t);
    memset(last_result, 0, sizeof last_result);
    for (int ch = 0; ch < CH_NUM; ch++)
        restart_seq[ch] = chan_get(ch)->restart_seq;
    load_defaults();
    tmr_stop(TMR_TX_DWELL0);
    tmr_stop(TMR_TX_DWELL1);
}

bool tx_any_active(void)
{
    if (txseq_any_active()) return true;
    for (int ch = 0; ch < CH_NUM; ch++)
        for (int i = 0; i < TX_ENTRIES; i++)
            if (t.e[ch][i].active)
                return true;
    return false;
}

uint16_t tx_load_permille(int ch, int extra_entry)
{
    if (ch < 0 || ch >= CH_NUM) return 0;
    const cfg_t *c = cfg_get();
    uint64_t pm = 0;
    for (int i = 0; i < TX_ENTRIES; i++) {
        const tx_entry_t *e = &t.e[ch][i];
        if (!e->period_ms || !(e->active || i == extra_entry))
            continue;
        pm += can_frame_time_ns(e->f.len, e->f.flags, c->nbr[ch], c->dbr[ch]) / (e->period_ms * 1000u);
    }
    return (uint16_t)(pm > 9999 ? 9999 : pm);
}

static bool fifo_put(int ch, can_frame_t *f)
{
    if (t.pending[ch] >= TX_FIFO_LEN)
        return false;
    f->ch = (uint8_t)ch;
    if (!port_can_tx(ch, f))
        return false;
    t.pending[ch]++;
    if (!tmr_active(DWELL_TMR(ch)))
        tmr_start(DWELL_TMR(ch), TX_DWELL_MS);
    return true;
}

bool tx_submit(int ch, const can_frame_t *f, tx_src_t src)
{
    (void)src;
    tx_result_t r = tx_validate_frame(ch, f, true);
    if (r != TX_OK) {
        if (ch >= 0 && ch < CH_NUM) last_result[ch] = r;
        return false;
    }
    if (!chan_can_tx(ch)) {
        last_result[ch] = TX_ERR_MODE;
        return false;
    }
    can_frame_t copy = *f;
    bool ok = fifo_put(ch, &copy);
    last_result[ch] = ok ? TX_OK : TX_ERR_FIFO;
    return ok;
}

bool tx_entry_submit(int ch, int i, tx_src_t src)
{
    if (ch < 0 || ch >= CH_NUM || i < 0 || i >= TX_ENTRIES) return false;
    tx_entry_t *e = &t.e[ch][i];
    can_frame_t f = e->f;
    dynfield_t dyn = e->dyn;
    dyn_apply(&dyn, f.data, f.len);
    if (!tx_submit(ch, &f, src))
        return false;
    e->dyn = dyn;
    e->sent++;
    return true;
}

static int pause_channel(int ch)
{
    int n = 0;
    for (int i = 0; i < TX_ENTRIES; i++) {
        if (t.e[ch][i].active) {
            t.e[ch][i].active = false;
            n++;
        }
    }
    return n;
}

/* 发送前的公共检查；返回 true 表示可以发 */
static bool precheck(int ch, int idx, bool cyclic, bool confirmed, src_t src)
{
    tx_result_t valid = tx_validate_frame(ch, &t.e[ch][idx].f, true);
    last_result[ch] = valid;
    if (valid != TX_OK) return false;
    const chan_t *c = chan_get(ch);
    if (c->state == CH_BUSOFF) {
        evt_msg(MSG_TX_DENY_BUSOFF, (uint8_t)ch, 0);
        return false;
    }
    if (!chan_rate_allows_tx(ch)) {
        evt_msg(MSG_CH_LOW_RATE, (uint8_t)ch, 0);
        return false;
    }
    if (!chan_can_tx(ch)) {
        evt_ask(cyclic ? ASK_CYC_NORMAL : ASK_TX_NORMAL, (uint8_t)ch, idx, src);
        return false;
    }
    if (c->first_tx_pending && !confirmed) {
        evt_ask(ASK_TX_FIRST, (uint8_t)ch, idx | (cyclic ? 0x100 : 0), src);
        return false;
    }
    chan_clear_first_tx(ch);
    return true;
}

void tx_handle(const app_evt_t *e)
{
    /* chan_handle 先处理同一事件；在任何新发送入队前同步控制器重启。
     * 不能只等异步 CFG_CHANGED，否则会误清该通知前刚入队的新发送。 */
    for (int c = 0; c < CH_NUM; c++) {
        if (restart_seq[c] != chan_get(c)->restart_seq) {
            restart_seq[c] = chan_get(c)->restart_seq;
            t.pending[c] = 0;
            tmr_stop(DWELL_TMR(c));
        }
    }
    int ch = e->ch < CH_NUM ? e->ch : 0;
    if ((e->type == EV_REQ_TX_SEND || e->type == EV_REQ_TX_CYC) && e->ch >= CH_NUM)
        return;
    switch (e->type) {
    case EV_REQ_TX_SEND:
        if (e->a < 0 || e->a >= TX_ENTRIES)
            break;
        if (precheck(ch, e->a, false, e->b != 0, (src_t)e->src))
            tx_entry_submit(ch, e->a, TXSRC_MANUAL);
        break;
    case EV_REQ_TX_CYC: {
        if (e->a < 0 || e->a >= TX_ENTRIES)
            break;
        tx_entry_t *en = &t.e[ch][e->a];
        bool on = e->b & 1, confirmed = (e->b & 2) != 0;
        if (!on) {
            en->active = false;
            break;
        }
        if (en->active || en->period_ms == 0)
            break;
        if (!precheck(ch, e->a, true, confirmed, (src_t)e->src))
            break;
        uint16_t load = tx_load_permille(ch, e->a);
        if (load > 1000) {
            evt_msg(MSG_TX_LOAD_DENY, (uint8_t)ch, load);
            break;
        }
        if (load > 800)
            evt_msg(MSG_TX_LOAD_WARN, (uint8_t)ch, load);
        en->active = true;
        en->host_started = e->src == SRC_HOST;
        en->next_due = app_now_ms();
        break;
    }
    case EV_REQ_TX_STOP_ALL:
        txseq_stop_all(TXSEQ_STOP_USER);
        for (int c = 0; c < CH_NUM; c++)
            pause_channel(c);
        break;
    case EV_CAN_TX_DONE:
        if (t.pending[ch])
            t.pending[ch]--;
        t.echo_count[ch]++;
        if (t.pending[ch])
            tmr_start(DWELL_TMR(ch), TX_DWELL_MS);   /* 下一帧重新计滞留时间 */
        else
            tmr_stop(DWELL_TMR(ch));
        break;
    case EV_TIMER:
        for (int c = 0; c < CH_NUM; c++) {
            /* A queued expiry can outlive an SD wait: real completions may
             * retire the old frame and arm a fresh deadline before dispatch.
             * Legitimate timer consumers clear active before delivering it. */
            if (e->a == DWELL_TMR(c) && t.pending[c] && !tmr_active(DWELL_TMR(c))) {
                port_can_tx_abort(c);                /* 无应答：清空 TX FIFO，暂停周期发送 */
                t.pending[c] = 0;
                t.noack_count[c]++;
                evt_msg(MSG_TX_NOACK, (uint8_t)c, pause_channel(c));
            }
        }
        break;
    case EV_NTF_CH_CFG_CHANGED:
        if (!chan_can_tx(ch)) {
            int n = pause_channel(ch);
            if (n)
                evt_msg(MSG_TX_PAUSED, (uint8_t)ch, n);
            t.pending[ch] = 0;
            tmr_stop(DWELL_TMR(ch));
        }
        break;
    case EV_SYS_STOP_ALL:
        txseq_stop_all(TXSEQ_STOP_POWER);
        for (int c = 0; c < CH_NUM; c++) {
            pause_channel(c);
            t.pending[c] = 0;
            tmr_stop(DWELL_TMR(c));
        }
        break;
    default:
        break;
    }
}

void tx_realtime_tick(void)
{
    for(int ch=0;ch<CH_NUM;ch++)if(tmr_take_expired(DWELL_TMR(ch))){
        const app_evt_t event={.type=EV_TIMER,.src=SRC_SYS,.a=DWELL_TMR(ch)};
        tx_handle(&event);
    }
}

void tx_tick(void)
{
    uint32_t now = app_now_ms();
    for (int ch = 0; ch < CH_NUM; ch++) {
        bool any = false;
        for (int i = 0; i < TX_ENTRIES; i++)
            any |= t.e[ch][i].active;
        if (!any)
            continue;
        if (!chan_can_tx(ch)) {                       /* Bus-Off 或模式变了 */
            evt_msg(MSG_TX_PAUSED, (uint8_t)ch, pause_channel(ch));
            continue;
        }
        for (int i = 0; i < TX_ENTRIES; i++) {
            tx_entry_t *e = &t.e[ch][i];
            if (!e->active || (int32_t)(now - e->next_due) < 0)
                continue;
            tx_result_t valid = tx_validate_frame(ch, &e->f, true);
            if (valid != TX_OK) {
                last_result[ch] = valid;
                e->active = false;
                continue;
            }
            if (!tx_entry_submit(ch, i, TXSRC_CYCLIC))
                e->missed++;                          /* TX FIFO 满：丢弃本次，不补发 */
            e->next_due += e->period_ms;
            /* Retire every other expired slot, including one exactly due
             * now. Repeated polls at the same timestamp must not catch up
             * with a second frame; keep the original phase in the future. */
            if ((int32_t)(now - e->next_due) >= 0) {
                uint32_t skip = (now - e->next_due) / e->period_ms + 1u;
                e->missed += skip;
                e->next_due += skip * e->period_ms;
            }
        }
    }
}
