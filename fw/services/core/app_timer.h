/* Shared foreground software timers post EV_TIMER(a=timer ID).
 * Deadlines use wrapping uint32 milliseconds; capture uptime is monotonic.
 * The bare-metal loop advances time, including cooperative I/O waits. */
#ifndef APP_TIMER_H
#define APP_TIMER_H

#include "services/core/app_types.h"

typedef enum {
    TMR_BOOT,          /* 开机按住 POWER 1 s；bootloader 里长按 POWER 关机 */
    TMR_BOOT_COMBO,    /* 开机 BACK+REC 按满 3 s */
    TMR_START,         /* 5V 打开后等待稳定 */
    TMR_SCREEN_OFF,    /* 自动熄屏 */
    TMR_AUTO_OFF,      /* 自动关机空闲计时 */
    TMR_AUTO_OFF_CD,   /* 自动关机倒计时，每秒一次 */
    TMR_SHUTDOWN,      /* 关机流程步进 */
    TMR_REC_GUARD,     /* 录制准备 / 停止超时保护 */
    TMR_TX_DWELL0,     /* 发送滞留检测（无应答），每通道一个 */
    TMR_TX_DWELL1,
    TMR_BUSOFF0,       /* Bus-Off 自动恢复退避，每通道一个 */
    TMR_BUSOFF1,
    TMR_NUM
} timer_id_t;

void     tmr_init(void);
void     tmr_start(timer_id_t id, uint32_t ms);
void     tmr_stop(timer_id_t id);
bool     tmr_active(timer_id_t id);
/* Consume this one due timer without posting events or advancing any other
 * timer. Foreground owners use it for safety work inside cooperative I/O. */
bool     tmr_take_expired(timer_id_t id);
void     tmr_tick(void);            /* 检查到期，投递事件 */

uint32_t app_now_ms(void);
uint64_t app_now_us(void);
void     app_clock_advance(uint32_t ms);

#endif
