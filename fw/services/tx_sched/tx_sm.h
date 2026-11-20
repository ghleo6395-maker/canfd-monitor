/* 发送调度（需求 4.2）：单发、周期发送列表、回放共用一条发送路径。
 *  - 发送前检查：通道必须在正常模式，否则请用户确认切换；Bus-Off 时拒绝；
 *  - 在设置里切到正常模式后，第一次发送还要确认一次（F1.6）；
 *  - 启动周期发送前估算负载：> 80% 警告，> 100% 拒绝（F2.3）；
 *  - 到期发不出去（TX FIFO 满）就丢弃本次并计数，不补发；
 *  - 有帧滞留超过 100 ms 没发出去 = 无应答：清空 TX FIFO，暂停该通道周期发送（F2.6）。 */
#ifndef TX_SM_H
#define TX_SM_H

#include "services/core/app_types.h"
#include "services/tx_sched/dynfield.h"

#define TX_ENTRIES   4      /* 模拟器每路 4 条；固件每路 32 条 */
#define TX_FIFO_LEN  8
#define TX_DWELL_MS  100

typedef enum { TXSRC_MANUAL, TXSRC_CYCLIC, TXSRC_REPLAY, TXSRC_HOST } tx_src_t;

/* Screen and host share one atomic editor. Stored FD entries may be prepared while
 * FD is disabled; the current channel capability is always checked at send time. */
typedef enum {
    TX_OK, TX_ERR_CHANNEL, TX_ERR_ENTRY, TX_ERR_NULL, TX_ERR_ID,
    TX_ERR_FLAGS, TX_ERR_LENGTH, TX_ERR_FD_DISABLED, TX_ERR_PERIOD,
    TX_ERR_ACTIVE, TX_ERR_MODE, TX_ERR_FIFO, TX_ERR_CHANGED, TX_ERR_DYNAMIC
} tx_result_t;

bool        tx_length_valid(uint8_t len, bool fd);
tx_result_t tx_validate_frame(int ch, const can_frame_t *f, bool for_send);
tx_result_t tx_entry_update(int ch, int idx, uint32_t id, uint8_t flags,
                           uint8_t len, const uint8_t *data, uint32_t period_ms);
const char *tx_result_name(tx_result_t r);
const char *tx_result_text(tx_result_t r);
tx_result_t tx_last_result(int ch);

typedef struct {
    can_frame_t f;
    uint16_t    period_ms;     /* 0 = 只能单发 */
    bool        active;
    bool        host_started;
    uint32_t    next_due;
    uint32_t    sent, missed;
    uint32_t    revision;      /* increments after an atomic screen/host edit */
    dynfield_t  dyn;
} tx_entry_t;

typedef struct {
    tx_entry_t e[CH_NUM][TX_ENTRIES];
    uint8_t    pending[CH_NUM];      /* 已交给 TX FIFO、还没发出去的帧数 */
    uint32_t   noack_count[CH_NUM];
    uint32_t   echo_count[CH_NUM];
} tx_t;

/* Restores validated configuration for complete selected channels atomically.
 * Runtime counters/active flags from the source are ignored; no frame is sent.
 * A selected channel with active TX, pending FIFO or replay is rejected. */
tx_result_t tx_validate_dynamic(const dynfield_t *dynamic,const can_frame_t *frame);
tx_result_t tx_restore_entries(uint8_t channel_mask,const tx_entry_t entries[CH_NUM][TX_ENTRIES]);

void        tx_init(void);
void        tx_handle(const app_evt_t *e);
void        tx_tick(void);
/* Consume only TX dwell safety deadlines; no application event dispatch. */
void        tx_realtime_tick(void);
const tx_t *tx_get(void);
bool        tx_any_active(void);
bool        tx_submit(int ch, const can_frame_t *f, tx_src_t src);   /* 回放用；TX FIFO 满时返回 false */
/* Caller must obtain normal-mode / first-send permission before submitting.
 * Reuses entry dynamic fields and updates the entry's accepted-send counter. */
bool        tx_entry_submit(int ch, int idx, tx_src_t src);
uint16_t    tx_load_permille(int ch, int extra_entry);               /* 周期列表占的负载 */

#endif
