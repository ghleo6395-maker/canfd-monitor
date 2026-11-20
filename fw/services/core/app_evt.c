#include "services/core/app_evt.h"
#ifdef REVD_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#define EVENT_LOCK() taskENTER_CRITICAL()
#define EVENT_UNLOCK() taskEXIT_CRITICAL()
#else
#define EVENT_LOCK() ((void)0)
#define EVENT_UNLOCK() ((void)0)
#endif

static app_evt_t q[APP_EVT_QUEUE_LEN];
static uint32_t  q_head, q_tail, q_dropped;

void evt_init(void)
{
    EVENT_LOCK(); q_head = q_tail = q_dropped = 0; EVENT_UNLOCK();
}

bool evt_post(const app_evt_t *e)
{
    EVENT_LOCK();
    if (q_head - q_tail >= APP_EVT_QUEUE_LEN) {
        q_dropped++;
        EVENT_UNLOCK(); return false;
    }
    q[q_head % APP_EVT_QUEUE_LEN] = *e;
    q_head++;
    EVENT_UNLOCK(); return true;
}

bool evt_post4(ev_type_t t, uint8_t ch, src_t src, int32_t a, int32_t b)
{
    app_evt_t e = { .type=(uint16_t)t, .ch=ch, .src=(uint8_t)src, .a=a, .b=b };
    return evt_post(&e);
}

bool evt_pop(app_evt_t *out)
{
    EVENT_LOCK();
    if (q_tail == q_head) { EVENT_UNLOCK(); return false; }
    *out = q[q_tail % APP_EVT_QUEUE_LEN];
    q_tail++;
    EVENT_UNLOCK(); return true;
}

uint32_t evt_dropped(void)
{
    EVENT_LOCK(); uint32_t dropped = q_dropped; EVENT_UNLOCK(); return dropped;
}

void evt_msg(msg_t m, uint8_t ch, int32_t b)
{
    evt_post4(EV_NTF_MSG, ch, SRC_SYS, (int32_t)m, b);
}

void evt_ask(ask_t q_, uint8_t ch, int32_t b, src_t src)
{
    evt_post4(EV_NTF_ASK, ch, src, (int32_t)q_, b);
}
