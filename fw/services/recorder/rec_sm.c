#include "services/recorder/rec_sm.h"
#include "services/config/cfg.h"
#include "services/bench/bench.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/frame_ring/frame_ring.h"
#include "services/logfmt/candump.h"
#include "services/storage/storage_sm.h"

#define WRITE_ERR_LIMIT     3u      /* 连续写错 3 次；实际恢复成功后清零 */
#define GUARD_MS            3000u   /* 预分配 / 关闭超时保护 */

static rec_t         r;
static ring_reader_t rd;
static bool          guard_armed;
static uint32_t      guard_due;

static const char *const st_name[REC_STATE_NUM] = { "空闲", "准备中", "录制中", "停止中" };
const char *rec_state_name(rec_state_t s) { return s < REC_STATE_NUM ? st_name[s] : "?"; }
const rec_t *rec_get(void) { return &r; }

void rec_init(void)
{
    r = (rec_t){ .state = REC_IDLE };
    guard_armed = false;
    tmr_stop(TMR_REC_GUARD);
    ring_reader_attach(&rd, true);
}

static void start_guard(void)
{
    guard_due = app_now_ms() + GUARD_MS;
    guard_armed = true;
    tmr_start(TMR_REC_GUARD, GUARD_MS);
}

static void stop_guard(void)
{
    guard_armed = false;
    tmr_stop(TMR_REC_GUARD);
}

static void to_stopping(msg_t why)
{
    /* Capture already-delivered frames before placing the close fence. */
    if (r.state == REC_RUNNING) rec_tick();
    r.state = REC_STOPPING;
    r.last_result = why;
    port_sd_close();
    start_guard();
}

static void to_idle(msg_t why)
{
    stop_guard();
    r.state = REC_IDLE;
    r.last_result = why;
    evt_msg(why, 0, (int32_t)r.frames);
}

static void new_segment(void)
{
    r.seg++;
    r.seg_bytes = 0;
    port_sd_next_segment(r.seg);    /* 平台关闭旧段并创建下一段；完成策略由实际 SD 驱动决定。 */
}

static void start(void)
{
    const cfg_t *c = cfg_get();
    msg_t why;
    if (!storage_writable(&why)) {
        evt_msg(why, 0, 0);
        return;
    }
    r = (rec_t){ .state = REC_PREPARING, .session = port_sd_next_session(), .seg = 1 };
    port_sd_prealloc(r.session, r.seg, cfg_rec_seg_kb(), c->nbr[0], c->dbr[0], c->fd[0]);
    start_guard();
}

void rec_handle(const app_evt_t *e)
{
    switch (e->type) {
    case EV_REQ_REC_START:
        if (r.state == REC_IDLE)
            start();
        break;
    case EV_REQ_REC_STOP:
        if (r.state != REC_RUNNING && r.state != REC_PREPARING)
            break;
        if (e->src != SRC_UI && e->b == 0) {       /* 上位机 / 命令行停止录制要设备确认（需求 3.5） */
            evt_ask(ASK_REC_STOP, 0, 0, (src_t)e->src);
            break;
        }
        to_stopping(MSG_REC_STOPPED);
        break;
    case EV_REQ_REC_MARK:
        if (r.state == REC_RUNNING) {
            r.markers++;
            evt_msg(MSG_REC_MARK, 0, (int32_t)r.markers);
        }
        break;
    case EV_REQ_REC_ROTATE:
        if (r.state == REC_RUNNING && bench_get()->enabled)
            new_segment();
        break;
    case EV_SD_PREALLOC_DONE:
        if (r.state != REC_PREPARING)
            break;
        stop_guard();
        if (e->a == SD_OK) {
            r.state = REC_RUNNING;
            r.start_ms = app_now_ms();
            evt_msg(MSG_REC_STARTED, 0, (int32_t)r.session);
        } else {
            r.state = REC_IDLE;
            r.last_result = e->a == SD_ERR_FRAG ? MSG_SD_FRAG : MSG_SD_FULL_NOREC;
            evt_msg(r.last_result, 0, 0);
        }
        break;
    case EV_NTF_CH_CFG_CHANGED:
        /* 录制中改了通道参数：切到新分段，新文件头记录新参数（F3.12） */
        if (r.state == REC_RUNNING && (cfg_get()->rec_ch_mask & (1u << e->ch))) {
            new_segment();
            evt_msg(MSG_REC_SEG_CFG, e->ch, (int32_t)r.seg);
        }
        break;
    case EV_CAN_DROPS:
        if (r.state == REC_RUNNING && (cfg_get()->rec_ch_mask & (1u << e->ch)))
            r.drops += (uint32_t)e->a;
        break;
    case EV_SD_WRITE_ERR:
        /* The platform may discover queue loss/write failure while draining
         * a user-requested close. A later successful flush cannot undo it. */
        if (r.state == REC_STOPPING) {
            r.last_result = MSG_REC_WRITE_FAIL;
            break;
        }
        if (r.state != REC_RUNNING)
            break;
        if (e->a == 2) { /* Storage queue loss is irreversible; close accepted prefix. */
            to_stopping(MSG_REC_WRITE_FAIL);
            break;
        }
        /* A failed card can take two seconds per retry. Time passing alone
         * does not mean recovery: only the platform's successful flush does. */
        if (!r.write_errs) {
            r.err_window_start = app_now_ms();
        }
        r.write_errs++;
        if (r.write_errs >= WRITE_ERR_LIMIT) {
            to_stopping(MSG_REC_WRITE_FAIL);
        } else {
            port_sd_reinit();                      /* 重试：重新初始化卡后接着写（F3.11） */
            evt_msg(MSG_REC_WRITE_RETRY, 0, r.write_errs);
        }
        break;
    case EV_SD_WRITE_RECOVERED:
        if (r.state == REC_RUNNING) {
            r.write_errs = 0;
            r.err_window_start = 0;
        }
        break;
    case EV_SD_FULL:
        if (r.state != REC_RUNNING)
            break;
        if (cfg_get()->rec_full_delete)
            port_sd_delete_oldest();               /* 结果见 EV_SD_DELETE_DONE */
        else
            to_stopping(MSG_REC_SD_FULL);          /* 默认：卡满即停止（F3.5） */
        break;
    case EV_SD_DELETE_DONE:
        if (r.state != REC_RUNNING)
            break;
        if (e->a)
            evt_msg(MSG_REC_DELETED_OLDEST, 0, 0);
        else
            to_stopping(MSG_REC_SD_FULL);          /* 都被保护了，没有可删的 */
        break;
    case EV_NTF_STORAGE_LOST:
        /* 录制中拔卡：直接停，文件留给下次挂载时修复 */
        if (r.state != REC_IDLE)
            to_idle(MSG_REC_SD_REMOVED);
        break;
    case EV_SD_CLOSE_DONE:
        if (r.state == REC_STOPPING)
            to_idle(e->a == SD_OK ? r.last_result : MSG_REC_WRITE_FAIL);
        break;
    case EV_SYS_STOP_ALL:
        if (r.state == REC_RUNNING || r.state == REC_PREPARING)
            to_stopping(MSG_REC_STOPPED);
        break;
    case EV_TIMER:
        if (e->a == TMR_REC_GUARD && guard_armed &&
            (int32_t)(app_now_ms() - guard_due) >= 0) {
            /* An expiry queued for a previous phase/session must not end a
             * newly armed guard. A timeout also does not cancel platform I/O:
             * keep SD ownership until CLOSE_DONE or media removal arrives. */
            stop_guard();
            if (r.state == REC_PREPARING) {
                to_stopping(MSG_SD_ERROR);
                evt_msg(MSG_SD_ERROR, 0, 0);
            } else if (r.state == REC_STOPPING) {
                r.last_result = MSG_REC_WRITE_FAIL;
                evt_msg(MSG_REC_WRITE_FAIL, 0, (int32_t)r.frames);
            }
        }
        break;
    default:
        break;
    }
}

/* 消费真实帧；具体编码、批写和同步由平台存储驱动负责。 */
void rec_tick(void)
{
    const cfg_t *c = cfg_get();
    uint32_t batch = 0;
    const can_frame_t *f;
    while ((f = ring_peek(&rd)) != NULL) {
        if (r.state == REC_RUNNING && (c->rec_ch_mask & (1u << f->ch))) {
            int encoded_bytes = logfmt_line_size(f);
            uint32_t rec_bytes = encoded_bytes > 0 ? (uint32_t)encoded_bytes : 0u;
            if (encoded_bytes > 0 && port_sd_record_frame(f)) {
                r.frames++;
                r.bytes += rec_bytes;
                r.seg_bytes += rec_bytes;
                batch += rec_bytes;
                if (r.seg_bytes >= (uint64_t)cfg_rec_seg_kb() * 1024u)
                    new_segment();
            } else {
                r.drops++; /* Rejected frames are never advertised as recorded. */
            }
        }
        ring_advance(&rd);
    }
    if (batch)
        port_sd_write(batch);
    if (r.state == REC_RUNNING)
        r.elapsed_ms = app_now_ms() - r.start_ms;
}
