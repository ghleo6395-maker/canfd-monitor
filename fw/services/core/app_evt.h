/* 核心事件队列：状态机由 core 单所有者运行到完成。
 * FreeRTOS 构建允许其他任务在短临界区中投递；中断只通知所属任务，不入此队列。 */
#ifndef APP_EVT_H
#define APP_EVT_H

#include "services/core/app_types.h"

#define APP_EVT_QUEUE_LEN 128

void     evt_init(void);
bool     evt_post(const app_evt_t *e);
bool     evt_post4(ev_type_t t, uint8_t ch, src_t src, int32_t a, int32_t b);
bool     evt_pop(app_evt_t *out);
uint32_t evt_dropped(void);

/* 便捷函数：给界面发提示 / 请求用户确认 */
void evt_msg(msg_t m, uint8_t ch, int32_t b);
void evt_ask(ask_t q, uint8_t ch, int32_t b, src_t src);

#endif
