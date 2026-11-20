/* 录制状态机（需求 4.3）。
 *
 *         REQ_REC_START                 预分配成功
 *   IDLE ───────────────▶ PREPARING ───────────────▶ RUNNING ──┐ 分段写满 / 通道参数变了：切段（不停）
 *    ▲   (没卡/U盘模式/卡异常：拒绝)   │ 预分配失败                │ REC 短按：打标记
 *    │                               ▼                          │
 *    │◀──────────────── 提示原因（碎片 / 卡满）                    │ REQ_REC_STOP（上位机来的要设备确认）
 *    │                                                          │ 卡满（默认停）/ 持续写错 / 低电 / 关机
 *    │          关闭完成                                         ▼
 *    └──────────────────────────────────────────────── STOPPING
 *   录制中拔卡：不经过 STOPPING，直接回 IDLE，下次插卡时自动修复。 */
#ifndef REC_SM_H
#define REC_SM_H

#include "services/core/app_types.h"

typedef enum { REC_IDLE, REC_PREPARING, REC_RUNNING, REC_STOPPING, REC_STATE_NUM } rec_state_t;

typedef struct {
    rec_state_t state;
    uint32_t    session, seg;
    uint32_t    start_ms, elapsed_ms;
    uint32_t    frames, drops, markers;
    uint64_t    bytes, seg_bytes;
    uint8_t     write_errs;
    uint32_t    err_window_start;
    msg_t       last_result;   /* 上一次结束的原因 */
} rec_t;

void         rec_init(void);
void         rec_handle(const app_evt_t *e);
void         rec_tick(void);
const rec_t *rec_get(void);
const char  *rec_state_name(rec_state_t s);

#endif
