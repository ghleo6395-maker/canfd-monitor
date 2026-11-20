#include "sim/tx_sequence.h"
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/can_core/bittime.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"

#include <string.h>

typedef struct {
    uint32_t entry_revision[TX_ENTRIES];
    uint32_t boot_count, restart_seq, noack_count;
    uint64_t last_service_ms;
    uint32_t round_limit;
    bool first_submission;
} binding_t;

static txseq_channel_t channels[CH_NUM];
static binding_t binding[CH_NUM];
static uint32_t request_serial;
static uint32_t last_clock_ms;
static uint64_t extended_clock_ms;

static bool valid_channel(int ch) { return ch >= 0 && ch < CH_NUM; }
static uint64_t now_ms(void)
{
    uint32_t sample = app_now_ms();
    extended_clock_ms += (uint32_t)(sample - last_clock_ms);
    last_clock_ms = sample;
    return extended_clock_ms;
}
static bool editable(void) { return power_running() && !power_get()->locked; }
static bool busy(const txseq_channel_t *c) { return c->state != TXSEQ_IDLE; }

static void stopped(int ch, txseq_stop_reason_t reason)
{
    channels[ch].state = TXSEQ_IDLE;
    channels[ch].stop_reason = reason;
    channels[ch].next_due_ms = 0;
}

void txseq_init(void)
{
    memset(channels, 0, sizeof channels);
    memset(binding, 0, sizeof binding);
    last_clock_ms = app_now_ms();
    extended_clock_ms = last_clock_ms;
    /* Never recycle a dialog token merely because the simulator was reset. */
    for (int ch = 0; ch < CH_NUM; ch++) {
        txseq_channel_t *c = &channels[ch];
        c->current_entry = -1;
        c->id_gap_ms = TXSEQ_DEFAULT_ID_GAP_MS;
        for (int i = 0; i < TX_ENTRIES; i++)
            c->entries[i] = (txseq_entry_config_t){ true, 1,
                TXSEQ_DEFAULT_FRAME_GAP_MS, TXSEQ_DEFAULT_CYCLE_MS };
        binding[ch].last_service_ms = UINT64_MAX;
    }
}

const txseq_channel_t *txseq_get(int ch)
{
    return valid_channel(ch) ? &channels[ch] : NULL;
}

bool txseq_any_active(void)
{
    for (int ch = 0; ch < CH_NUM; ch++)
        if (channels[ch].state == TXSEQ_RUNNING || channels[ch].state == TXSEQ_ARMING)
            return true;
    return false;
}

bool txseq_uses_entry(int ch, int entry)
{
    return valid_channel(ch) && entry >= 0 && entry < TX_ENTRIES &&
        busy(&channels[ch]) && channels[ch].entries[entry].enabled;
}

static bool valid_config(const txseq_entry_config_t *entry)
{
    if (entry->repeat_count < 1 || entry->repeat_count > TXSEQ_MAX_COUNT ||
        entry->frame_gap_ms > TXSEQ_MAX_TIME_MS ||
        entry->cycle_ms < 1 || entry->cycle_ms > TXSEQ_MAX_TIME_MS ||
        (entry->repeat_count > 1 && entry->frame_gap_ms < 1)) return false;
    uint64_t span = (uint64_t)(entry->repeat_count - 1) * entry->frame_gap_ms;
    return entry->cycle_ms > span;
}

int txseq_configure(int ch, const txseq_entry_config_t entries[TX_ENTRIES],
                    uint32_t id_gap_ms, uint32_t expected_revision)
{
    if (!valid_channel(ch)) return TXSEQ_ERR_CHANNEL;
    txseq_channel_t *c = &channels[ch];
    if (!editable() || ui_dialog_open()) return TXSEQ_ERR_NOT_READY;
    if (c->revision != expected_revision) return TXSEQ_ERR_CHANGED;
    if (busy(c)) return TXSEQ_ERR_ACTIVE;
    if (!entries || id_gap_ms > TXSEQ_MAX_TIME_MS) return TXSEQ_ERR_ARGUMENT;
    for (int i = 0; i < TX_ENTRIES; i++)
        if (!valid_config(&entries[i])) return TXSEQ_ERR_ARGUMENT;
    memcpy(c->entries, entries, sizeof c->entries);
    c->id_gap_ms = id_gap_ms;
    c->revision++;
    return TXSEQ_OK;
}

int txseq_defaults(int ch, uint32_t expected_revision)
{
    if (!valid_channel(ch)) return TXSEQ_ERR_CHANNEL;
    const txseq_channel_t *c = &channels[ch];
    txseq_entry_config_t entries[TX_ENTRIES];
    memcpy(entries, c->entries, sizeof entries);
    for (int i = 0; i < TX_ENTRIES; i++) {
        entries[i].frame_gap_ms = TXSEQ_DEFAULT_FRAME_GAP_MS;
        uint32_t span = (entries[i].repeat_count - 1) * TXSEQ_DEFAULT_FRAME_GAP_MS;
        entries[i].cycle_ms = span < TXSEQ_DEFAULT_CYCLE_MS ?
            TXSEQ_DEFAULT_CYCLE_MS : span + 1;
    }
    return txseq_configure(ch, entries, TXSEQ_DEFAULT_ID_GAP_MS, expected_revision);
}

static int first_entry(const txseq_channel_t *c)
{
    for (int i = 0; i < TX_ENTRIES; i++) if (c->entries[i].enabled) return i;
    return -1;
}

static int next_entry(const txseq_channel_t *c, int current)
{
    for (int n = 1; n <= TX_ENTRIES; n++) {
        int i = (current + n) % TX_ENTRIES;
        if (c->entries[i].enabled) return i;
    }
    return -1;
}

uint16_t txseq_load_permille(int ch)
{
    if (!valid_channel(ch)) return 0;
    const txseq_channel_t *c = &channels[ch];
    uint64_t frame_ns = 0, cycle_ms = 0;
    for (int i = 0; i < TX_ENTRIES; i++) {
        if (!c->entries[i].enabled) continue;
        const can_frame_t *a = &tx_get()->e[ch][i].f;
        int next = next_entry(c, i);
        const can_frame_t *z = &tx_get()->e[ch][next].f;
        cycle_ms += c->entries[i].cycle_ms;
        if (a->id != z->id || ((a->flags ^ z->flags) & CANF_EXT)) cycle_ms += c->id_gap_ms;
        frame_ns += (uint64_t)c->entries[i].repeat_count *
            can_frame_time_ns(a->len, a->flags, cfg_get()->nbr[ch], cfg_get()->dbr[ch]);
    }
    if (!cycle_ms) return 0;
    uint64_t load = frame_ns / (cycle_ms * 1000u);
    return (uint16_t)(load > 9999 ? 9999 : load);
}

static bool conflict(int ch)
{
    if (play_get(ch)->state != PL_IDLE) return true;
    for (int i = 0; i < TX_ENTRIES; i++)
        if (tx_get()->e[ch][i].active) return true;
    return false;
}

static int validate_entries(int ch, bool bound)
{
    const txseq_channel_t *c = &channels[ch];
    if (first_entry(c) < 0) return TXSEQ_ERR_EMPTY;
    for (int i = 0; i < TX_ENTRIES; i++) {
        if (!c->entries[i].enabled) continue;
        if (bound && tx_get()->e[ch][i].revision != binding[ch].entry_revision[i])
            return TXSEQ_ERR_CHANGED;
        if (tx_validate_frame(ch, &tx_get()->e[ch][i].f, true) != TX_OK)
            return TXSEQ_ERR_FRAME;
    }
    return TXSEQ_OK;
}

static void activate(int ch)
{
    txseq_channel_t *c = &channels[ch];
    c->state = TXSEQ_RUNNING;
    c->current_entry = first_entry(c);
    c->occurrence = c->rounds = c->sent = c->missed = 0;
    c->group_start_ms = c->next_due_ms = now_ms();
    c->stop_reason = TXSEQ_STOP_NONE;
    binding[ch].restart_seq = chan_get(ch)->restart_seq;
    binding[ch].noack_count = tx_get()->noack_count[ch];
    binding[ch].last_service_ms = UINT64_MAX;
    binding[ch].first_submission = true;
    chan_clear_first_tx(ch);
}

int txseq_start_rounds(int ch, uint32_t expected_revision, uint32_t round_limit)
{
    if (!valid_channel(ch)) return TXSEQ_ERR_CHANNEL;
    txseq_channel_t *c = &channels[ch];
    if (!editable() || ui_dialog_open()) return TXSEQ_ERR_NOT_READY;
    if (c->revision != expected_revision) return TXSEQ_ERR_CHANGED;
    if (busy(c)) return TXSEQ_ERR_ACTIVE;
    if (conflict(ch)) return TXSEQ_ERR_CONFLICT;
    int result = validate_entries(ch, false);
    if (result != TXSEQ_OK) return result;
    if (!chan_rate_allows_tx(ch) || chan_get(ch)->state == CH_BUSOFF ||
        chan_get(ch)->state == CH_AUTOBAUD || chan_get(ch)->state == CH_STANDBY)
        return TXSEQ_ERR_MODE;
    uint16_t load = txseq_load_permille(ch);
    if (load > 1000) {
        evt_msg(MSG_TX_LOAD_DENY, (uint8_t)ch, load);
        return TXSEQ_ERR_LOAD;
    }
    if (load > 800) evt_msg(MSG_TX_LOAD_WARN, (uint8_t)ch, load);
    binding[ch].boot_count = power_get()->boot_count;
    binding[ch].round_limit = round_limit;
    for (int i = 0; i < TX_ENTRIES; i++)
        binding[ch].entry_revision[i] = tx_get()->e[ch][i].revision;
    request_serial = (request_serial + 1) & 0x7FFFFFFFu;
    if (!request_serial) request_serial = 1;
    c->request = request_serial;
    c->stop_reason = TXSEQ_STOP_NONE;
    if (!chan_can_tx(ch) || chan_get(ch)->first_tx_pending) {
        c->state = TXSEQ_CONFIRMING;
        evt_ask(!chan_can_tx(ch) ? ASK_SEQ_NORMAL : ASK_SEQ_FIRST,
            (uint8_t)ch, (int32_t)c->request, SRC_UI);
        return TXSEQ_WAIT_CONFIRM;
    }
    activate(ch);
    return TXSEQ_OK;
}

int txseq_start(int ch, uint32_t expected_revision)
{
    return txseq_start_rounds(ch,expected_revision,0);
}

int txseq_answer(int ch, uint32_t request, bool allow)
{
    if (!valid_channel(ch)) return TXSEQ_ERR_CHANNEL;
    txseq_channel_t *c = &channels[ch];
    if (c->state != TXSEQ_CONFIRMING || c->request != request)
        return TXSEQ_ERR_REQUEST;
    if (!allow) { stopped(ch, TXSEQ_STOP_CANCELLED); return TXSEQ_OK; }
    if (!editable() || power_get()->boot_count != binding[ch].boot_count) {
        stopped(ch, TXSEQ_STOP_POWER); return TXSEQ_ERR_NOT_READY;
    }
    int result = validate_entries(ch, true);
    if (result != TXSEQ_OK) {
        stopped(ch, result == TXSEQ_ERR_CHANGED ? TXSEQ_STOP_CHANGED : TXSEQ_STOP_FRAME);
        return result;
    }
    if (conflict(ch)) {
        stopped(ch, TXSEQ_STOP_CONFLICT); return TXSEQ_ERR_CONFLICT;
    }
    if (!chan_rate_allows_tx(ch) || chan_get(ch)->state == CH_BUSOFF ||
        chan_get(ch)->state == CH_AUTOBAUD || chan_get(ch)->state == CH_STANDBY) {
        stopped(ch, TXSEQ_STOP_MODE); return TXSEQ_ERR_MODE;
    }
    if (txseq_load_permille(ch) > 1000) {
        stopped(ch, TXSEQ_STOP_FRAME); return TXSEQ_ERR_LOAD;
    }
    if (!chan_can_tx(ch)) {
        /* The existing device dialog authorized this exact transition. Mode
         * request b=2 means the subsequent first frame is already authorized. */
        if (!evt_post4(EV_REQ_CH_MODE, (uint8_t)ch, SRC_UI, CAN_MODE_NORMAL, 2)) {
            stopped(ch, TXSEQ_STOP_MODE); return TXSEQ_ERR_MODE;
        }
        c->state = TXSEQ_ARMING;
    } else activate(ch);
    return TXSEQ_OK;
}

void txseq_stop(int ch)
{
    if (valid_channel(ch)) stopped(ch, TXSEQ_STOP_USER);
}

void txseq_stop_all(txseq_stop_reason_t reason)
{
    for (int ch = 0; ch < CH_NUM; ch++)
        if (busy(&channels[ch])) stopped(ch, reason);
}

static void service(bool running_only)
{
    const uint64_t now = now_ms();
    for (int ch = 0; ch < CH_NUM; ch++) {
        txseq_channel_t *c = &channels[ch];
        binding_t *b = &binding[ch];
        if (!busy(c)) continue;
        if (running_only && c->state != TXSEQ_RUNNING) continue;
        if (!power_running() || power_get()->boot_count != b->boot_count) {
            stopped(ch, TXSEQ_STOP_POWER); continue;
        }
        int result = validate_entries(ch, true);
        if (result != TXSEQ_OK) {
            stopped(ch, result == TXSEQ_ERR_CHANGED ? TXSEQ_STOP_CHANGED : TXSEQ_STOP_FRAME);
            continue;
        }
        if (conflict(ch)) { stopped(ch, TXSEQ_STOP_CONFLICT); continue; }
        if (c->state == TXSEQ_CONFIRMING) continue;
        if (!chan_can_tx(ch) || !chan_rate_allows_tx(ch)) {
            stopped(ch, TXSEQ_STOP_MODE); continue;
        }
        if (c->state == TXSEQ_ARMING) activate(ch);
        if (tx_get()->noack_count[ch] != b->noack_count) {
            stopped(ch, TXSEQ_STOP_NOACK); continue;
        }
        if (chan_get(ch)->restart_seq != b->restart_seq || chan_get(ch)->first_tx_pending) {
            stopped(ch, TXSEQ_STOP_MODE); continue;
        }
        if (now < c->next_due_ms || b->last_service_ms == now) continue;
        b->last_service_ms = now;
        /* A start command can precede the next host clock tick. The first
         * group's cycle is measured from its actual first submission, rather
         * than shortening that group by the command-to-service delay. */
        if (b->first_submission) {
            c->group_start_ms = c->next_due_ms = now;
            b->first_submission = false;
        }
        const int entry = c->current_entry;
        const txseq_entry_config_t *cfg = &c->entries[entry];
        /* One submission per channel and timestamp, even if a delayed host
         * tick spans many slots. Expired positions count as missed; subsequent
         * intervals rebase on actual submission time, so there is no burst. */
        if (cfg->frame_gap_ms && c->occurrence + 1 < cfg->repeat_count) {
            uint64_t late_slots = (now - c->next_due_ms) / cfg->frame_gap_ms;
            uint32_t remaining = cfg->repeat_count - c->occurrence - 1;
            uint32_t skip = late_slots > remaining ? remaining : (uint32_t)late_slots;
            c->occurrence += skip;
            c->missed += skip;
        }
        if (tx_entry_submit(ch, entry, TXSRC_CYCLIC)) c->sent++;
        else c->missed++;
        c->occurrence++;
        if (c->occurrence < cfg->repeat_count) {
            c->next_due_ms = now + cfg->frame_gap_ms;
        } else {
            int next = next_entry(c, entry);
            const can_frame_t *a = &tx_get()->e[ch][entry].f;
            const can_frame_t *z = &tx_get()->e[ch][next].f;
            uint32_t gap = a->id != z->id || ((a->flags ^ z->flags) & CANF_EXT) ? c->id_gap_ms : 0;
            uint64_t boundary = c->group_start_ms + cfg->cycle_ms;
            /* A delayed final frame never brings the next group into the past.
             * Ensure progress even for a zero-gap transition after that delay. */
            if (boundary < now) boundary = now;
            c->next_due_ms = boundary + gap;
            if (c->next_due_ms <= now) c->next_due_ms = now + 1;
            c->group_start_ms = c->next_due_ms;
            c->current_entry = next;
            c->occurrence = 0;
            if (next <= entry) {
                c->rounds++;
                if (b->round_limit && c->rounds >= b->round_limit)
                    stopped(ch, TXSEQ_STOP_COMPLETE);
            }
        }
    }
}
void txseq_tick(void){service(false);}
void txseq_realtime_tick(void){service(true);}

const char *txseq_result_text(int result)
{
    switch (result) {
    case TXSEQ_OK: return "已执行";
    case TXSEQ_WAIT_CONFIRM: return "请在设备屏幕确认循环发送";
    case TXSEQ_ERR_CHANNEL: return "通道必须为 CAN1 或 CAN2";
    case TXSEQ_ERR_ARGUMENT: return "次数需为1..10000；时间最多3600000ms；周期须大于(次数-1)×同ID间隔";
    case TXSEQ_ERR_CHANGED: return "配置或报文已变化，请重新读取后再操作";
    case TXSEQ_ERR_ACTIVE: return "请先停止该通道的循环发送再修改";
    case TXSEQ_ERR_EMPTY: return "请至少选择一条发送报文";
    case TXSEQ_ERR_NOT_READY: return "请先开机、解除按键锁并处理设备确认";
    case TXSEQ_ERR_CONFLICT: return "该通道已有周期发送或文件回放，请先停止";
    case TXSEQ_ERR_FRAME: return "所选报文不适合当前通道，请检查ID、格式、长度和FD开关";
    case TXSEQ_ERR_MODE: return "当前通道不能发送，请检查模式、位速率或Bus-Off状态";
    case TXSEQ_ERR_REQUEST: return "此次确认已失效，请重新发起循环发送";
    case TXSEQ_ERR_LOAD: return "所选循环的估计总线负载超过100%，请增大周期或减少次数";
    default: return "循环发送请求失败";
    }
}

const char *txseq_stop_text(txseq_stop_reason_t reason)
{
    switch (reason) {
    case TXSEQ_STOP_NONE: return "未停止";
    case TXSEQ_STOP_USER: return "手动停止";
    case TXSEQ_STOP_CANCELLED: return "已取消启动";
    case TXSEQ_STOP_POWER: return "设备关机或重新启动";
    case TXSEQ_STOP_MODE: return "通道模式、位速率或控制器状态已变化";
    case TXSEQ_STOP_CHANGED: return "所选报文已变化";
    case TXSEQ_STOP_NOACK: return "发送无应答，循环已停止";
    case TXSEQ_STOP_CONFLICT: return "该通道开始了其他周期发送或回放";
    case TXSEQ_STOP_FRAME: return "所选报文与当前通道配置不兼容";
    case TXSEQ_STOP_COMPLETE: return "已完成指定轮次";
    default: return "未知原因";
    }
}
