/* 应用顶层：初始化、事件分发、节拍、快照。
 * 模拟器和单元测试通过这几个函数驱动整套状态机；固件里由主循环和协作 I/O 等待调用对应部分。 */
#ifndef APP_H
#define APP_H

#include "services/core/app_types.h"
#include "services/core/jsonw.h"

void app_init(void);                     /* MCU 上电前的初始状态：设备关机 */
void app_capture_poll(void);            /* Foreground only; drain frame readers, never dispatch events. */
/* Cooperative SD wait only: advance the shared clock and existing TX/capture.
 * Does not dispatch events, timers, confirmation, storage commands or UI. The
 * caller samples elapsed time with the same accumulator used by app_tick. */
void app_realtime_poll(uint32_t ms);
/* Hardware foreground completion, exactly once per confirmed TX event. Safe
 * during an SD wait; bypasses application dispatch and touches only TX state. */
void app_tx_complete(int ch);
void app_tick(uint32_t ms);              /* 推进时间：定时器 → 事件 → 各服务的周期处理 */
void app_key_raw(key_id_t k, bool down); /* 按键原始电平变化 */
void app_touch(ev_type_t type, int32_t a);
void app_post(const app_evt_t *e);       /* 平台前台事件入口；ISR不得直接调用。 */
void app_json(jw_t *j);                  /* 整机状态快照 */

#endif
