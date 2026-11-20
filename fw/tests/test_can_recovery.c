/* Failed controller transitions must not become successful UI/send states. */
#include "app/app.h"
#include "services/can_core/chan_sm.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include <stdio.h>

static unsigned checks,failures;
#define CHECK(condition) do { checks++; if(!(condition)) { failures++; \
    printf("CAN recovery line %d: %s\n",__LINE__,#condition); } } while(0)
static void post(ev_type_t type,int ch,int a,int b)
{
    app_evt_t event={.type=type,.ch=(uint8_t)ch,.src=SRC_UI,.a=a,.b=b};
    if(type==EV_CAN_STATUS||type==EV_CAN_PROBE_RESULT)event.generation=port_can_generation(ch);
    CHECK(evt_post(&event));app_tick(0);
}
static void boot(void)
{
    sim_key(KEY_POWER,1);
    for(unsigned i=0;i<150;i++)sim_tick(10);
    sim_key(KEY_POWER,0);
}
static void startup_failure(void)
{
    sim_reset();CHECK(env_cmd("can_start_fail",0,1)==0);boot();
    CHECK(chan_get(0)->state==CH_STANDBY&&!chan_can_tx(0));
    CHECK(chan_get(1)->state==CH_LISTEN);
    CHECK(env_cmd("can_start_fail",0,0)==0);
    post(EV_REQ_CH_RATE,0,500000,2000000);
    CHECK(chan_get(0)->state==CH_LISTEN);
    post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,1);
    CHECK(chan_can_tx(0)&&chan_get(0)->first_tx_pending);
}
static void mode_failure(void)
{
    sim_reset();boot();post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,2);
    CHECK(tx_entry_submit(0,0,TXSRC_MANUAL));
    CHECK(env_cmd("can_start_fail",0,1)==0);
    post(EV_REQ_CH_RATE,0,250000,2000000);
    CHECK(chan_get(0)->state==CH_STANDBY&&!chan_can_tx(0));
    CHECK(tx_get()->pending[0]==0&&!tmr_active(TMR_TX_DWELL0));
    CHECK(!tx_entry_submit(0,0,TXSRC_MANUAL));
    CHECK(env_cmd("can_start_fail",0,0)==0);
    post(EV_REQ_CH_RATE,0,500000,2000000);
    CHECK(chan_get(0)->state==CH_LISTEN); /* Recovery never silently enables TX. */
}
static void recovery_failure_and_credits(void)
{
    for(int fail=0;fail<2;fail++) {
        sim_reset();boot();post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,2);
        CHECK(tx_entry_submit(0,0,TXSRC_MANUAL));
        post(EV_CAN_STATUS,0,256,0x10000);
        CHECK(chan_get(0)->state==CH_BUSOFF);
        uint32_t before=chan_get(0)->restart_seq;
        CHECK(env_cmd("can_start_fail",0,fail)==0);
        post(EV_REQ_CH_RECOVER,0,0,0);
        CHECK(chan_get(0)->state==(fail?CH_BUSOFF:CH_NORMAL));
        CHECK(chan_get(0)->restart_seq!=before);
        CHECK(tx_get()->pending[0]==0&&!tmr_active(TMR_TX_DWELL0));
        if(fail) {
            CHECK(!tx_entry_submit(0,0,TXSRC_MANUAL));
            CHECK(env_cmd("can_start_fail",0,0)==0);
            post(EV_REQ_CH_RECOVER,0,0,0);
            CHECK(chan_can_tx(0));
        }
        CHECK(tx_entry_submit(0,0,TXSRC_MANUAL));
        CHECK(tx_get()->pending[0]==1);
    }
}
static void probe_start_failure(void)
{
    sim_reset();boot();CHECK(env_cmd("can_start_fail",0,1)==0);
    post(EV_REQ_AUTOBAUD,0,CAN_MODE_LISTEN,0);
    CHECK(chan_get(0)->state==CH_STANDBY&&!chan_can_tx(0));
}
static void stale_hardware_events(void)
{
    sim_reset();boot();post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,2);
    app_evt_t old={.type=EV_CAN_STATUS,.ch=0,.src=SRC_SYS,.a=255,.b=0x10000,
        .generation=port_can_generation(0)};
    post(EV_REQ_CH_RATE,0,250000,2000000);
    CHECK(old.generation!=port_can_generation(0));
    CHECK(evt_post(&old));app_tick(0);
    CHECK(chan_can_tx(0)&&chan_get(0)->busoff_count==0);
    old.generation=port_can_generation(0);CHECK(evt_post(&old));app_tick(0);
    CHECK(chan_get(0)->state==CH_BUSOFF&&chan_get(0)->busoff_count==1);
    post(EV_REQ_CH_RECOVER,0,0,0);
    post(EV_REQ_AUTOBAUD,0,CAN_MODE_LISTEN,0);
    old=(app_evt_t){.type=EV_CAN_PROBE_RESULT,.ch=0,.src=SRC_SYS,.a=10,
        .generation=port_can_generation(0)};
    post(EV_REQ_CH_MODE,0,CAN_MODE_LISTEN,0);
    post(EV_REQ_AUTOBAUD,0,CAN_MODE_LISTEN,0);
    CHECK(old.generation!=port_can_generation(0));
    CHECK(evt_post(&old));app_tick(0);
    CHECK(chan_get(0)->state==CH_AUTOBAUD&&chan_get(0)->ab_idx==0);
    old.generation=port_can_generation(0);old.a=0;old.b=1;
    CHECK(evt_post(&old));CHECK(evt_post(&old));app_tick(0);
    CHECK(chan_get(0)->state==CH_AUTOBAUD&&chan_get(0)->ab_idx==1);
}
int main(void)
{
    startup_failure();mode_failure();recovery_failure_and_credits();probe_start_failure();
    stale_hardware_events();
    printf("CAN recovery: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
