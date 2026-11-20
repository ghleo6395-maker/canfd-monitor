/* Capture timestamps must remain monotonic while millisecond deadlines wrap. */
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    evt_init();
    tmr_init();
    uint64_t elapsed_ms = UINT32_MAX - 10u;
    app_clock_advance((uint32_t)elapsed_ms);
    uint64_t before = port_capture_time_us();
    tmr_start(TMR_REC_GUARD, 20);

    app_clock_advance(15);
    elapsed_ms += 15;
    assert(app_now_ms() == 4);
    assert(port_capture_time_us() == before + 15000);
    assert(app_now_us() == elapsed_ms * 1000);
    tmr_tick();
    app_evt_t event;
    assert(!evt_pop(&event));
    assert(tmr_active(TMR_REC_GUARD));

    app_clock_advance(5);
    elapsed_ms += 5;
    tmr_tick();
    assert(evt_pop(&event));
    assert(event.type == EV_TIMER && event.a == TMR_REC_GUARD);
    assert(!evt_pop(&event));
    assert(!tmr_active(TMR_REC_GUARD));

    /* More than one rollover and reset both preserve the clock contract. */
    app_clock_advance(UINT32_MAX);
    elapsed_ms += UINT32_MAX;
    assert(app_now_ms() == (uint32_t)elapsed_ms);
    assert(port_capture_time_us() == elapsed_ms * 1000);
    tmr_start(TMR_REC_GUARD, 20);
    app_clock_advance(19);
    assert(!tmr_take_expired(TMR_REC_GUARD));
    app_clock_advance(1);
    assert(tmr_take_expired(TMR_REC_GUARD));
    assert(!tmr_take_expired(TMR_REC_GUARD));
    tmr_init();
    assert(app_now_ms() == 0 && app_now_us() == 0);
    assert(!tmr_active(TMR_REC_GUARD));
    puts("application capture clock: rollover, repeated wrap, timers and reset passed");
    return 0;
}
