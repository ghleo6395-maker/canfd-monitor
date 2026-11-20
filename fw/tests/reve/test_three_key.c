/* Production recognizer + power state machine, without physical I/O. */
#include "services/core/board_profile.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/input/keys.h"
#include "services/power/power_sm.h"
#include "app/ui/core/ui_sm.h"
#include "sim/sim_api.h"
#include <assert.h>
#include <stdio.h>

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while(0)
static void tick(unsigned ms) {app_clock_advance(ms);keys_tick();}
static void reset(void) {tmr_init();evt_init();keys_reset();}
static unsigned events(ev_type_t type)
{
    app_evt_t e; unsigned n=0;
    while(evt_pop(&e)) {CHECK(e.type==type);++n;}
    return n;
}
static void release_chord(void)
{
    keys_raw(KEY_BACK,false,false);keys_raw(KEY_REC,false,false);
    CHECK(events(EV_KEY)==0);
}
static void run(unsigned ms) {for(unsigned n=0;n<ms;n+=10)sim_tick(10);}
int main(void)
{
    for(unsigned k=0;k<KEY_NUM;k++)
        CHECK(board_has_physical_key((key_id_t)k)==(k==KEY_BACK||k==KEY_REC||k==KEY_POWER));
    CHECK(!board_has_physical_key(KEY_NUM));CHECK(!board_has_physical_key((key_id_t)-1));
    reset();keys_raw(KEY_BACK,true,false);tick(100);keys_raw(KEY_REC,true,false);
    tick(1999);CHECK(events(EV_KEY)==0);tick(1);CHECK(events(EV_COMBO)==1);
    tick(5000);CHECK(events(EV_COMBO)==0);release_chord();
    keys_raw(KEY_BACK,true,false);keys_raw(KEY_REC,true,false);tick(2000);
    CHECK(events(EV_COMBO)==1);release_chord();
    reset();keys_raw(KEY_REC,true,false);keys_raw(KEY_BACK,true,false);tick(1999);
    release_chord();tick(5000);CHECK(events(EV_KEY)==0);
    reset();keys_raw(KEY_BACK,true,true);keys_raw(KEY_REC,true,false);tick(3000);
    CHECK(events(EV_COMBO)==0);release_chord();
    reset();keys_raw(KEY_BACK,true,false);keys_raw(KEY_REC,true,false);tick(1000);
    keys_cancel(KEY_BACK);tick(2000);keys_raw(KEY_REC,false,false);CHECK(events(EV_KEY)==0);
    /* A chord that straddles the 32-bit millisecond rollover must wait for
     * BOTH keys; comparing absolute timestamps fires the combo too early. */
    for (unsigned reversed=0; reversed<2; ++reversed) {
        reset();app_clock_advance(UINT32_MAX-100u);
        keys_raw(reversed?KEY_REC:KEY_BACK,true,false);tick(150);
        keys_raw(reversed?KEY_BACK:KEY_REC,true,false);
        tick(1999);CHECK(events(EV_COMBO)==0);
        tick(1);CHECK(events(EV_COMBO)==1);release_chord();
    }
    /* Touch-generated logical arrows/OK remain usable despite removed switches. */
    reset();keys_raw(KEY_OK,true,false);tick(100);keys_raw(KEY_OK,false,false);
    CHECK(events(EV_KEY)==1);
    sim_reset();sim_key(KEY_BACK,1);sim_key(KEY_POWER,1);run(1300);
    sim_key(KEY_POWER,0);sim_key(KEY_BACK,0);run(500);
    CHECK(power_get()->state==PWR_RUNNING);CHECK(ui_page()==PG_ENG);
    sim_key(KEY_BACK,1);sim_key(KEY_REC,1);run(2100);
    sim_key(KEY_BACK,0);sim_key(KEY_REC,0);run(50);CHECK(power_get()->locked);
    sim_key(KEY_BACK,1);sim_key(KEY_REC,1);run(2100);
    sim_key(KEY_BACK,0);sim_key(KEY_REC,0);run(50);CHECK(!power_get()->locked);
    sim_reset();sim_key(KEY_BACK,1);sim_key(KEY_REC,1);sim_key(KEY_POWER,1);run(3500);
    CHECK(power_get()->state==PWR_RUNNING);CHECK(ui_page()==PG_ENG);
    /* External power is not evidence of net charging. Low cell voltage must
     * still stop recording and shut down, while a single short dip is ignored. */
    app_evt_t batt={.type=EV_BATT,.a=3250,.b=1};
    power_handle(&batt);CHECK(power_get()->state==PWR_RUNNING);
    batt.a=3500;power_handle(&batt);CHECK(power_get()->state==PWR_RUNNING);
    batt.a=3250;power_handle(&batt);power_handle(&batt);
    CHECK(power_get()->state==PWR_RUNNING);
    power_handle(&batt);CHECK(power_get()->state==PWR_SHUTDOWN);
    printf("Rev E physical mask, chord and maintenance: %u checks passed\n",checks);
    return 0;
}
