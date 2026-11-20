/* SD 卡与 U 盘模式状态机（需求 F3.11、F7.2、F7.3）。
 *
 *   NO_CARD ──插卡──▶ MOUNTING ──成功（顺带修复上次没关好的录制）──▶ READY
 *      ▲                 │ 写保护 / 不支持的文件系统                  │  │ 格式化（要确认）
 *      │                 ▼                                         │  ▼
 *      │◀──拔卡──────── ERROR                         FORMATTING ◀──┘  └─ 进入 U 盘模式（要确认；录制 / 回放中拒绝）
 *      │                                                                    ▼
 *      └──────────────────────────── 拔卡 ◀─────────────────────────────── MSC ── 电脑弹出 / 拔 USB / 设备收回 → MOUNTING */
#ifndef STORAGE_SM_H
#define STORAGE_SM_H

#include "services/core/app_types.h"

typedef enum { ST_NO_CARD, ST_MOUNTING, ST_READY, ST_MSC, ST_FORMATTING, ST_ERROR, ST_STATE_NUM } storage_state_t;

typedef struct {
    storage_state_t state;
    bool            usb_attached;
    sd_result_t     last_err;
} storage_t;

void             storage_init(void);
void             storage_handle(const app_evt_t *e);
const storage_t *storage_get(void);
bool             storage_writable(msg_t *why);   /* 能不能开始录制；不能时给出原因 */
bool             storage_readable(msg_t *why);   /* 能不能回放 / 读文件 */
const char      *storage_state_name(storage_state_t s);

#endif
