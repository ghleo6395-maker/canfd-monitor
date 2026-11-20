#include "services/core/app_timer.h"
#include "services/core/app_evt.h"

static struct {
    bool     active;
    uint32_t due;
} tmr[TMR_NUM];

/* Keep capture time monotonic beyond the 49.7-day deadline-clock rollover. */
static uint64_t uptime_ms;

void tmr_init(void)
{
    uptime_ms = 0;
    for (int i = 0; i < TMR_NUM; i++)
        tmr[i].active = false;
}

void tmr_start(timer_id_t id, uint32_t ms)
{
    tmr[id].active = true;
    tmr[id].due = app_now_ms() + ms;
}

void tmr_stop(timer_id_t id)
{
    tmr[id].active = false;
}

bool tmr_active(timer_id_t id)
{
    return tmr[id].active;
}

bool tmr_take_expired(timer_id_t id)
{
    if((unsigned)id>=TMR_NUM||!tmr[id].active||
       (int32_t)(app_now_ms()-tmr[id].due)<0)return false;
    tmr[id].active=false;
    return true;
}

void tmr_tick(void)
{
    uint32_t now_ms = app_now_ms();
    for (int i = 0; i < TMR_NUM; i++) {
        if (tmr[i].active && (int32_t)(now_ms - tmr[i].due) >= 0) {
            /* A full application queue must not permanently lose a safety
             * deadline. Retain its due state until delivery succeeds. */
            if(evt_post4(EV_TIMER, 0, SRC_SYS, i, 0))tmr[i].active = false;
        }
    }
}

uint32_t app_now_ms(void)
{
    return (uint32_t)uptime_ms;
}

uint64_t app_now_us(void)
{
    return uptime_ms * 1000u;
}

void app_clock_advance(uint32_t ms)
{
    uptime_ms += ms;
}
