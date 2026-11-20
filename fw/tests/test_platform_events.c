/* Error completions from a physical storage driver must never become success. */
#include "app/app.h"
#include "sim/sim_api.h"
#include "services/core/app_evt.h"
#include "services/storage/storage_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/frame_ring/frame_ring.h"
#include "services/stats/stats.h"
#include "sim/env_sim.h"
#include <assert.h>
#include <stdio.h>
static int checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)
static void send(ev_type_t t,int a,int b){evt_post4(t,0,SRC_UI,a,b);app_tick(0);}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);for(int i=0;i<150;i++)sim_tick(10);sim_key(KEY_POWER,0);}
int main(void)
{
 boot(); CHECK(storage_get()->state==ST_READY);
 send(EV_REQ_FORMAT,0,1);CHECK(storage_get()->state==ST_FORMATTING);
 send(EV_SD_FORMAT_DONE,SD_ERR_IO,0);CHECK(storage_get()->state==ST_ERROR);
 CHECK(storage_get()->last_err==SD_ERR_IO);
 send(EV_REQ_FORMAT,0,1);send(EV_SD_FORMAT_DONE,SD_OK,0);
 CHECK(storage_get()->state==ST_READY);
 boot();send(EV_REQ_REC_START,0,0);
 for(int i=0;i<100;i++)sim_tick(10);
 CHECK(rec_get()->state==REC_RUNNING);
 send(EV_REQ_REC_STOP,0,1);CHECK(rec_get()->state==REC_STOPPING);
 send(EV_SD_CLOSE_DONE,SD_ERR_IO,0);
 CHECK(rec_get()->state==REC_IDLE && rec_get()->last_result==MSG_REC_WRITE_FAIL);
 boot();send(EV_REQ_REC_START,0,0);for(int i=0;i<100;i++)sim_tick(10);
 app_capture_poll();
 uint32_t accepted=rec_get()->frames, lost=rec_get()->drops;
 CHECK(env_cmd("sd_remove",0,0)==0);
 can_frame_t frame={.id=0x123,.len=8}; CHECK(ring_push(&frame));
 rec_tick(); CHECK(rec_get()->frames==accepted && rec_get()->drops==lost+1);
 boot();send(EV_REQ_REC_START,0,0);for(int i=0;i<100;i++)sim_tick(10);
 app_capture_poll(); accepted=rec_get()->frames; CHECK(ring_push(&frame));
 send(EV_REQ_REC_STOP,0,1); CHECK(rec_get()->frames==accepted+1);
 boot();send(EV_REQ_REC_START,0,0);for(int i=0;i<100;i++)sim_tick(10);
 send(EV_SD_WRITE_ERR,2,0);
 CHECK(rec_get()->state==REC_STOPPING && rec_get()->last_result==MSG_REC_WRITE_FAIL);
 uint32_t old_hw=stats_get(0)->hw_drops;
 send(EV_CAN_DROPS,3,2);send(EV_CAN_DROPS,4,3);send(EV_CAN_DROPS,5,4);
 CHECK(stats_get(0)->hw_drops==old_hw && stats_get(0)->ingress_drops==3 &&
       stats_get(0)->ring_drops==4 && stats_get(0)->tx_echo_drops==5);
 printf("platform completion errors: %d checks passed\n", checks);
}
