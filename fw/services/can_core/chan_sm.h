/* CAN 通道状态机（每路一个，需求 4.1）。
 *
 *            开机(保存过速率)          REQ_CH_MODE（切到正常要确认）
 *   STANDBY ─────────────▶ LISTEN ◀──────────────▶ RESTRICTED / NORMAL
 *      ▲  关机 / 停止        │  ▲                          │
 *      └────────────────────┘  │ 识别结束（成功/失败都回只听）│ TEC>255
 *                       REQ_AUTOBAUD                        ▼
 *                              ▼                         BUS_OFF ──恢复（手动 / 自动退避）──▶ NORMAL
 *                          AUTOBAUD（逐档试；只听失败可改受限重试）
 *
 * 规则：
 *  - 每次开机强制只听（F8.6）；标称速率 < 50k 不能切到正常（F1.1）；
 *  - 切到正常模式要确认；在设置里切过去的，第一次发送还要再确认一次（F1.6）；
 *  - 录制中禁止自动识别；录制中改速率 / 模式会通知录制切段（F3.12）。 */
#ifndef CHAN_SM_H
#define CHAN_SM_H

#include "services/core/app_types.h"

typedef enum { CH_STANDBY, CH_LISTEN, CH_RESTRICTED, CH_NORMAL, CH_BUSOFF, CH_AUTOBAUD, CH_STATE_NUM } ch_state_t;
typedef enum { ERRST_ACTIVE, ERRST_WARNING, ERRST_PASSIVE, ERRST_BUSOFF } err_state_t;

typedef struct {
    ch_state_t  state;
    can_mode_t  mode;            /* 当前生效的模式 */
    uint16_t    tec, rec;
    err_state_t errst;
    bool        first_tx_pending;
    uint32_t    restart_seq;     /* 控制器清空 FIFO 后递增，供发送服务同步记账 */
    /* 自动识别 */
    can_mode_t  ab_mode;
    uint8_t     ab_phase;        /* 0 标称段，1 数据段 */
    uint8_t     ab_idx;
    uint16_t    ab_err_total;
    bool        ab_fd_seen;
    /* Bus-Off */
    uint8_t     busoff_tries;
    uint32_t    busoff_count;
} chan_t;

void          chan_init(void);
void          chan_handle(int ch, const app_evt_t *e);
const chan_t *chan_get(int ch);
bool          chan_can_tx(int ch);            /* 正常模式且不在 Bus-Off */
bool          chan_rate_allows_tx(int ch);    /* 标称速率 ≥ 50k */
void          chan_clear_first_tx(int ch);
const char   *chan_state_name(ch_state_t s);
uint8_t       chan_ab_progress(int ch);       /* 自动识别进度 0..100 */

#endif
