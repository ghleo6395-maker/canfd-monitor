/* SPDX-License-Identifier: MIT
 * Production key recognizer and app/simulator integration. No hardware I/O. */
#include "services/input/keys.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/can_core/chan_sm.h"
#include "services/storage/storage_sm.h"
#include "app/ui/core/ui_sm.h"
#include "sim/sim_api.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void reset_keys(void) {tmr_init();evt_init();keys_reset();}
static void tick(unsigned ms) {app_clock_advance(ms);keys_tick();}
static void no_gestures(void)
{
    app_evt_t e;
    while(evt_pop(&e)) CHECK(e.type!=EV_KEY && e.type!=EV_COMBO);
}
static void test_cancel_all_keys(void)
{
    for(unsigned k=0;k<KEY_NUM;k++) {
        reset_keys();
        if(k==KEY_POWER) keys_raw(KEY_POWER,false,false);
        keys_raw((key_id_t)k,true,false);CHECK(keys_is_down((key_id_t)k));
        keys_cancel((key_id_t)k);CHECK(!keys_is_down((key_id_t)k));
        tick(5000);keys_raw((key_id_t)k,false,false);no_gestures();
        keys_cancel((key_id_t)k);no_gestures();
    }
    reset_keys();keys_raw(KEY_OK,true,false);tick(999);no_gestures();
    keys_cancel(KEY_OK);tick(5000);keys_raw(KEY_OK,false,false);no_gestures();
    reset_keys();keys_raw(KEY_REC,true,true);keys_cancel(KEY_REC);tick(5000);no_gestures();
    reset_keys();keys_raw(KEY_UP,true,false);tick(500);
    app_evt_t e;CHECK(evt_pop(&e)&&e.type==EV_KEY&&e.ch==KEY_UP&&e.a==GEST_REPEAT);
    keys_cancel(KEY_UP);tick(5000);keys_raw(KEY_UP,false,false);no_gestures();
}
static void test_chord_reuse_and_invalid_key(void)
{
    reset_keys();keys_raw(KEY_LEFT,true,false);keys_raw(KEY_RIGHT,true,false);tick(1999);
    keys_cancel(KEY_LEFT);tick(2);keys_raw(KEY_RIGHT,false,false);no_gestures();
    keys_raw(KEY_LEFT,true,false);keys_raw(KEY_RIGHT,true,false);tick(2000);
    app_evt_t e;CHECK(evt_pop(&e)&&e.type==EV_COMBO&&e.a==COMBO_LOCK);CHECK(!evt_pop(&e));
    keys_cancel(KEY_LEFT);keys_cancel(KEY_RIGHT);tick(3000);
    keys_raw(KEY_LEFT,false,false);keys_raw(KEY_RIGHT,false,false);no_gestures();
    keys_raw(KEY_LEFT,true,false);keys_raw(KEY_RIGHT,true,false);tick(2000);
    CHECK(evt_pop(&e)&&e.type==EV_COMBO);CHECK(!evt_pop(&e));
    reset_keys();keys_raw(KEY_OK,true,false);
    keys_cancel((key_id_t)-1);keys_cancel(KEY_NUM);CHECK(keys_is_down(KEY_OK));
    keys_cancel(KEY_OK);keys_raw(KEY_OK,false,false);no_gestures();
    reset_keys();keys_raw(KEY_POWER,true,false);keys_cancel(KEY_POWER);
    CHECK(!keys_power_armed());tick(3000);no_gestures();
    keys_raw(KEY_POWER,false,false);CHECK(keys_power_armed());no_gestures();
}
static void run(unsigned ms) {for(unsigned n=0;n<ms;n+=10)sim_tick(10);}
static void test_cancel_cannot_answer_new_dialog(void)
{
    sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(500);
    CHECK(storage_get()->state==ST_READY);
    CHECK(evt_post4(EV_REQ_CH_MODE,0,SRC_HOST,CAN_MODE_NORMAL,0));run(20);
    CHECK(ui_dialog_ask()==ASK_CH_NORMAL&&chan_get(0)->mode==CAN_MODE_LISTEN);
    sim_key(KEY_OK,1);run(100);CHECK(keys_is_down(KEY_OK));
    /* Another input cancels the old prompt and opens a different one while
     * virtual OK is still held. The stale release must not answer it. */
    sim_touch(2,1);run(20);CHECK(!ui_dialog_open());
    CHECK(evt_post4(EV_REQ_FORMAT,0,SRC_HOST,0,0));run(20);
    CHECK(ui_dialog_ask()==ASK_FORMAT&&storage_get()->state==ST_READY);
    keys_cancel(KEY_OK);CHECK(!keys_is_down(KEY_OK));
    sim_key(KEY_OK,0);run(500);
    CHECK(ui_dialog_ask()==ASK_FORMAT&&storage_get()->state==ST_READY);
    CHECK(chan_get(0)->mode==CAN_MODE_LISTEN);
    /* A new deliberate press still works: cancellation is not a permanent
     * input lock. Formatting here affects only the simulator's RAM model. */
    sim_key(KEY_OK,1);run(60);sim_key(KEY_OK,0);run(20);
    CHECK(ui_dialog_ask()!=ASK_FORMAT);
    run(500);CHECK(storage_get()->state==ST_READY);
}
int main(void)
{
    test_cancel_all_keys();test_chord_reuse_and_invalid_key();test_cancel_cannot_answer_new_dialog();
    printf("keys_cancel production + dialog integration: %u checks passed\n",checks);return 0;
}
