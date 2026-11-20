/* SPDX-License-Identifier: MIT */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/bench/bench.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/sim_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(EV_REQ_REC_START == 28, "existing simulator event number changed");
_Static_assert(EV_REQ_REC_MARK == 30, "existing recorder event number changed");
_Static_assert(EV_NTF_ASK == 49, "append new events without renumbering existing events");

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static bool ok(const char *r) { return strncmp(r,"OK ",3)==0; }
static void run(unsigned ms) { for (unsigned i=0;i<ms;i+=10) sim_tick(10); }
static void post(ev_type_t type, int ch, int a, int b)
{ CHECK(evt_post4(type,(uint8_t)ch,SRC_HOST,a,b)); run(20); }
static void boot(void)
{
    sim_reset(); sim_key(KEY_POWER,1); run(1200); sim_key(KEY_POWER,0); run(500);
    CHECK(power_running()); CHECK(storage_get()->state==ST_READY);
}
static void test_ranges_and_boot(void)
{
    sim_reset();
    CHECK(!ok(bench_execute(BENCH_ON,NULL)));
    uint32_t epoch=bench_get()->diag_epoch;
    boot(); CHECK(bench_get()->diag_epoch>epoch);
    CHECK(!bench_get()->enabled);
    uint32_t normal=cfg_rec_seg_kb();
    const bench_config_t bad[]={{63,0,0},{65537,0,0},{64,249,0},{64,3600001,0},{64,0,49},{64,0,60001}};
    CHECK(!ok(bench_execute(BENCH_CONFIG,NULL)));
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) CHECK(!ok(bench_execute(BENCH_CONFIG,&bad[i])));
    bench_config_t c={64,250,50};
    CHECK(ok(bench_execute(BENCH_CONFIG,&c)));
    CHECK(cfg_rec_seg_kb()==normal);
    CHECK(ok(bench_execute(BENCH_ON,NULL)));
    CHECK(cfg_rec_seg_kb()==64 && bench_get()->enabled);
    CHECK(rec_get()->state==REC_IDLE);
    CHECK(chan_get(0)->mode==CAN_MODE_LISTEN && chan_get(1)->mode==CAN_MODE_LISTEN);
    CHECK(!tx_any_active() && !play_any_active());
    run(110); CHECK(bench_get()->redraws_requested>=2);
    CHECK(bench_get()->rotations_requested==0);
    CHECK(!ok(bench_execute(BENCH_ROTATE,NULL)));
    CHECK(ok(bench_execute(BENCH_OFF,NULL)));
    CHECK(cfg_rec_seg_kb()==normal);
    uint32_t red=bench_get()->redraws_requested;run(200);CHECK(bench_get()->redraws_requested==red);
    CHECK(!ok(bench_execute((bench_action_t)99,NULL)));
    CHECK(ok(bench_execute(BENCH_ON,NULL)));
    epoch=bench_get()->diag_epoch;
    sim_reset();CHECK(!bench_get()->enabled && bench_get()->diag_epoch>epoch);
}
static void test_record_rotate_confirmations(void)
{
    boot(); bench_config_t c={64,250,50};
    CHECK(ok(bench_execute(BENCH_CONFIG,&c)));CHECK(ok(bench_execute(BENCH_ON,NULL)));
    post(EV_REQ_REC_START,0,0,0);run(500);
    CHECK(rec_get()->state==REC_RUNNING);
    CHECK(!ok(bench_execute(BENCH_CONFIG,&c)));
    CHECK(!ok(bench_execute(BENCH_OFF,NULL)));
    CHECK(!ok(bench_execute(BENCH_ON,NULL)));
    uint32_t seg=rec_get()->seg,requested=bench_get()->rotations_requested;
    run(500);CHECK(rec_get()->seg>=seg+2);
    CHECK(bench_get()->rotations_requested>=requested+2);
    CHECK(ok(bench_execute(BENCH_ROTATE,NULL)));run(10);
    CHECK(rec_get()->seg>=seg+3);
    while (evt_post4(EV_NONE,0,SRC_SYS,0,0)) {}
    uint32_t rejected=bench_get()->request_rejected;
    CHECK(!ok(bench_execute(BENCH_ROTATE,NULL)));
    CHECK(bench_get()->request_rejected==rejected+1);
    app_tick(0);
    /* Existing host stop and transmit approvals remain mandatory in bench. */
    post(EV_REQ_REC_STOP,0,0,0);
    CHECK(rec_get()->state==REC_RUNNING);
    CHECK(ui_dialog_ask()==ASK_REC_STOP);
    sim_touch(2,1);run(20);CHECK(rec_get()->state==REC_RUNNING);
    post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,0);
    CHECK(chan_get(0)->mode==CAN_MODE_LISTEN);
    CHECK(ui_dialog_ask()==ASK_CH_NORMAL);
    sim_touch(2,1);run(20);
    /* The approved UI stop uses the normal core path, then bench can stop. */
    CHECK(evt_post4(EV_REQ_REC_STOP,0,SRC_UI,0,1));run(500);
    CHECK(rec_get()->state==REC_IDLE);CHECK(ok(bench_execute(BENCH_OFF,NULL)));
    seg=rec_get()->seg;
    post(EV_REQ_REC_ROTATE,0,0,0);CHECK(rec_get()->seg==seg);
}
static void test_storage_and_tx_denials(void)
{
    boot();post(EV_SD_REMOVE,0,0,0);CHECK(!ok(bench_execute(BENCH_ON,NULL)));
    boot();
    /* Pending single TX is busy even when there is no cyclic entry. */
    tx_t *tx=(tx_t *)(void *)tx_get();tx->pending[0]=1;
    CHECK(!ok(bench_execute(BENCH_ON,NULL)));tx->pending[0]=0;
    tx->e[1][0].active=true;CHECK(!ok(bench_execute(BENCH_ON,NULL)));tx->e[1][0].active=false;
    player_t *pl=(player_t *)(void *)play_get(0);pl->state=PL_PAUSED;
    CHECK(!ok(bench_execute(BENCH_ON,NULL)));pl->state=PL_IDLE;
    CHECK(ok(bench_execute(BENCH_ON,NULL)));
    cfg_mut()->screen_off_s=1;run(3000);CHECK(power_get()->screen_on);
}
int main(void)
{
    test_ranges_and_boot();test_record_rotate_confirmations();test_storage_and_tx_denials();
    printf("Bench: %u checks passed\n",checks);return 0;
}
