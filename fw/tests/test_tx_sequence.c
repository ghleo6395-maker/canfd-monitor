/* Functional tests of the host UI's ID-group send model against real app/TX,
 * timers, FIFO and simulated ACKs. These are not hardware timing measurements. */
#include "sim/tx_sequence.h"
#include "sim/sim_api.h"
#include "sim/env_sim.h"
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/frame_ring/frame_ring.h"
#include "services/power/power_sm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)

typedef struct { uint64_t at; int ch, entry; } sent_t;
static sent_t sent[256];
static unsigned sent_count;
static uint32_t accepted[CH_NUM][TX_ENTRIES];
static uint64_t model_clock;

static void observe(void)
{
    for (int ch = 0; ch < CH_NUM; ch++) for (int i = 0; i < TX_ENTRIES; i++) {
        uint32_t n = tx_get()->e[ch][i].sent;
        while (accepted[ch][i] < n) {
            CHECK(sent_count < sizeof sent / sizeof sent[0]);
            sent[sent_count++] = (sent_t){model_clock,ch,i};
            accepted[ch][i]++;
        }
    }
}

static void service(void) { txseq_tick(); observe(); }
static void step(uint32_t ms)
{
    model_clock += ms;
    app_tick(ms);
    txseq_tick();
    observe();
    env_tick(ms);
    app_tick(0); /* Drain real TX_DONE events from the simulator ACK path. */
}
static void run(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) step(1); }
static void post(ev_type_t type, int ch, int a, int b)
{
    CHECK(evt_post4(type,(uint8_t)ch,SRC_UI,a,b)); app_tick(0);
}
static void boot(void)
{
    sim_reset();
    sim_key(KEY_POWER,1);
    for (int i = 0; i < 170; i++) sim_tick(10);
    sim_key(KEY_POWER,0);
    for (int i = 0; i < 5; i++) sim_tick(10);
    CHECK(power_running());
    env_cmd("bus_fps",0,0); env_cmd("bus_fps",1,0);
    txseq_init();
    model_clock = 0; sent_count = 0;
    memset(accepted,0,sizeof accepted);
    for (int ch = 0; ch < CH_NUM; ch++) {
        for (int i = 0; i < TX_ENTRIES; i++) {
            uint8_t data[1] = {(uint8_t)i};
            CHECK(tx_entry_update(ch,i,0x100u + (uint32_t)i,0,1,data,100)==TX_OK);
        }
    }
}
static void normal(int ch) { post(EV_REQ_CH_MODE,ch,CAN_MODE_NORMAL,2); CHECK(chan_can_tx(ch)); }
static void config(int ch, int first_count, uint32_t first_gap, uint32_t first_cycle,
                   bool second, int second_count, uint32_t second_gap,
                   uint32_t second_cycle, uint32_t id_gap)
{
    txseq_entry_config_t cfg[TX_ENTRIES];
    memcpy(cfg,txseq_get(ch)->entries,sizeof cfg);
    for (int i = 0; i < TX_ENTRIES; i++) cfg[i].enabled = false;
    cfg[0] = (txseq_entry_config_t){true,(uint32_t)first_count,first_gap,first_cycle};
    cfg[1] = (txseq_entry_config_t){second,(uint32_t)second_count,second_gap,second_cycle};
    CHECK(txseq_configure(ch,cfg,id_gap,txseq_get(ch)->revision)==TXSEQ_OK);
}
static void start(int ch)
{
    CHECK(txseq_start(ch,txseq_get(ch)->revision)==TXSEQ_OK);
    service();
}
static void expected(unsigned i,uint64_t at,int ch,int entry)
{
    CHECK(i < sent_count); CHECK(sent[i].at==at); CHECK(sent[i].ch==ch); CHECK(sent[i].entry==entry);
}

static void sequential_groups_and_channel_isolation(void)
{
    boot(); normal(0); normal(1);
    config(0,3,10,50,true,2,5,20,7);
    start(0); run(150);
    CHECK(sent_count==10); CHECK(txseq_get(1)->sent==0);
    static const uint32_t times[] = {0,10,20,57,62,84,94,104,141,146};
    static const int entries[] = {0,0,0,1,1,0,0,0,1,1};
    for (unsigned i=0;i<10;i++) expected(i,times[i],0,entries[i]);
    CHECK(txseq_get(0)->missed==0 && txseq_get(0)->rounds==2);
    CHECK(tx_get()->e[0][0].sent==6 && tx_get()->e[0][1].sent==4);
    CHECK(tx_any_active());
    txseq_stop(0); uint32_t before=txseq_get(0)->sent; run(100);
    CHECK(txseq_get(0)->sent==before && !tx_any_active());
    start(0); CHECK(txseq_get(0)->sent==1 && txseq_get(0)->rounds==0);
    config(1,1,10,30,false,1,10,100,99);
    start(1); run(35);
    CHECK(txseq_get(1)->sent==2 && txseq_get(0)->state==TXSEQ_RUNNING);
    txseq_stop(1); CHECK(txseq_get(0)->state==TXSEQ_RUNNING);
}

static void one_id_cycle_and_no_zero_gap(void)
{
    boot(); normal(0);
    config(0,3,10,50,false,1,10,100,123);
    start(0); run(75);
    CHECK(sent_count==6);
    for (unsigned i=0;i<6;i++) expected(i,(i<3?0:50)+(i%3)*10,0,0);
    txseq_stop(0);
    config(0,1,0,1,false,1,10,100,0);
    start(0); uint32_t before=txseq_get(0)->sent;
    service(); service(); CHECK(txseq_get(0)->sent==before);
    step(1); CHECK(txseq_get(0)->sent==before+1);
}

static void late_ticks_skip_expired_positions(void)
{
    boot(); normal(0);
    config(0,5,10,60,false,1,10,100,7);
    start(0); step(35);
    CHECK(txseq_get(0)->sent==2 && txseq_get(0)->missed==2);
    CHECK(txseq_get(0)->occurrence==4);
    expected(0,0,0,0); expected(1,35,0,0);
    run(10); CHECK(txseq_get(0)->sent==3);
    expected(2,45,0,0);
    run(15); CHECK(txseq_get(0)->sent==4);
    expected(3,60,0,0);
    /* A very late final submission cannot cause a same-timestamp new round. */
    step(500); uint32_t before=txseq_get(0)->sent;
    CHECK(before==5); service(); CHECK(txseq_get(0)->sent==before);
    step(1); CHECK(txseq_get(0)->sent==before+1);
}

static void permission_has_no_probe_and_cancel_is_final(void)
{
    boot();
    config(0,2,10,30,false,1,10,100,7);
    CHECK(txseq_start(0,txseq_get(0)->revision)==TXSEQ_WAIT_CONFIRM);
    app_tick(0); CHECK(ui_dialog_open() && ui_dialog_ask()==ASK_SEQ_NORMAL);
    run(25); CHECK(txseq_get(0)->sent==0 && tx_get()->e[0][0].sent==0);
    CHECK(chan_get(0)->state==CH_LISTEN && tx_get()->pending[0]==0);
    sim_touch(2,1); CHECK(!ui_dialog_open());
    CHECK(txseq_get(0)->state==TXSEQ_IDLE && txseq_get(0)->stop_reason==TXSEQ_STOP_CANCELLED);
    run(20); CHECK(txseq_get(0)->sent==0);
    CHECK(txseq_start(0,txseq_get(0)->revision)==TXSEQ_WAIT_CONFIRM);
    app_tick(0); sim_touch(2,0);
    CHECK(chan_can_tx(0) && txseq_get(0)->state==TXSEQ_ARMING);
    CHECK(tx_get()->pending[0]==0 && tx_get()->e[0][0].sent==0);
    service(); CHECK(txseq_get(0)->sent==1);
    CHECK(!chan_get(0)->first_tx_pending);
    txseq_stop(0);
    post(EV_REQ_CH_MODE,0,CAN_MODE_LISTEN,0);
    post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,1);
    CHECK(chan_get(0)->first_tx_pending);
    CHECK(txseq_start(0,txseq_get(0)->revision)==TXSEQ_WAIT_CONFIRM);
    app_tick(0); CHECK(ui_dialog_ask()==ASK_SEQ_FIRST);
    uint32_t total=tx_get()->e[0][0].sent;
    run(15); CHECK(tx_get()->e[0][0].sent==total);
    sim_touch(2,0); service(); CHECK(tx_get()->e[0][0].sent==total+1);
    txseq_stop(0);
    uint32_t old=txseq_get(0)->request;
    CHECK(txseq_answer(0,old,true)==TXSEQ_ERR_REQUEST);
}

static void validation_defaults_and_edit_ownership(void)
{
    boot(); normal(0);
    txseq_entry_config_t c[TX_ENTRIES];
    memcpy(c,txseq_get(0)->entries,sizeof c);
    uint32_t revision=txseq_get(0)->revision;
    c[0].repeat_count=3; c[0].frame_gap_ms=10; c[0].cycle_ms=20;
    CHECK(txseq_configure(0,c,5,revision)==TXSEQ_ERR_ARGUMENT);
    CHECK(txseq_get(0)->revision==revision && txseq_get(0)->entries[0].repeat_count==1);
    c[0].cycle_ms=21;
    CHECK(txseq_configure(0,c,5,revision)==TXSEQ_OK);
    CHECK(txseq_configure(0,c,5,revision)==TXSEQ_ERR_CHANGED);
    c[0].repeat_count=TXSEQ_MAX_COUNT+1;
    CHECK(txseq_configure(0,c,5,txseq_get(0)->revision)==TXSEQ_ERR_ARGUMENT);
    c[0].repeat_count=1; c[0].cycle_ms=0;
    CHECK(txseq_configure(0,c,5,txseq_get(0)->revision)==TXSEQ_ERR_ARGUMENT);
    config(0,20,4,500,false,1,10,100,321);
    uint32_t id=tx_get()->e[0][0].f.id, entry_revision=tx_get()->e[0][0].revision;
    CHECK(txseq_defaults(0,txseq_get(0)->revision)==TXSEQ_OK);
    CHECK(txseq_get(0)->entries[0].repeat_count==20 && !txseq_get(0)->entries[1].enabled);
    CHECK(txseq_get(0)->entries[0].frame_gap_ms==10 && txseq_get(0)->entries[0].cycle_ms==191);
    CHECK(txseq_get(0)->id_gap_ms==10);
    CHECK(tx_get()->e[0][0].f.id==id && tx_get()->e[0][0].revision==entry_revision);
    start(0);
    CHECK(txseq_configure(0,c,5,txseq_get(0)->revision)==TXSEQ_ERR_ACTIVE);
    uint8_t data=0xab;
    CHECK(tx_entry_update(0,0,0x123,0,1,&data,100)==TX_ERR_ACTIVE);
    CHECK(tx_entry_update(0,3,0x123,0,1,&data,100)==TX_OK);
    post(EV_REQ_TX_STOP_ALL,0,0,0); CHECK(txseq_get(0)->state==TXSEQ_IDLE);
    CHECK(tx_entry_update(0,0,0x123,0,1,&data,100)==TX_OK);
    memcpy(c,txseq_get(0)->entries,sizeof c);
    for(int i=0;i<TX_ENTRIES;i++) c[i].enabled=false;
    CHECK(txseq_configure(0,c,0,txseq_get(0)->revision)==TXSEQ_OK);
    CHECK(txseq_start(0,txseq_get(0)->revision)==TXSEQ_ERR_EMPTY);
}

static void fifo_no_ack_restart_and_stop_all(void)
{
    boot(); normal(0);
    config(0,20,1,30,false,1,10,100,0);
    CHECK(env_cmd("bus_nodes",0,0)==0);
    start(0); run(20);
    CHECK(txseq_get(0)->sent==TX_FIFO_LEN);
    CHECK(txseq_get(0)->missed==12 && tx_get()->pending[0]==TX_FIFO_LEN);
    run(90);
    CHECK(txseq_get(0)->state==TXSEQ_IDLE && txseq_get(0)->stop_reason==TXSEQ_STOP_NOACK);
    CHECK(tx_get()->pending[0]==0);
    CHECK(env_cmd("bus_nodes",0,2)==0); uint32_t before=txseq_get(0)->sent;
    run(50); CHECK(txseq_get(0)->sent==before);
    start(0); run(2); CHECK(txseq_get(0)->sent==3);
    post(EV_REQ_CH_MODE,0,CAN_MODE_LISTEN,0);
    post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,2);
    service(); CHECK(txseq_get(0)->state==TXSEQ_IDLE && txseq_get(0)->stop_reason==TXSEQ_STOP_MODE);
    start(0); post(EV_SYS_STOP_ALL,0,0,0);
    CHECK(txseq_get(0)->state==TXSEQ_IDLE && txseq_get(0)->stop_reason==TXSEQ_STOP_POWER);
    before=txseq_get(0)->sent; run(20); CHECK(txseq_get(0)->sent==before);
}

static void wrap_and_load_limit(void)
{
    boot(); normal(0);
    config(0,2,10,30,false,1,10,100,9);
    app_clock_advance(UINT32_MAX-app_now_ms()-15u); power_activity();
    model_clock=0;
    start(0); run(65);
    CHECK(sent_count==5);
    static const uint32_t when[]={0,10,30,40,60};
    for(unsigned i=0;i<5;i++) expected(i,when[i],0,0);
    txseq_stop(0);
    post(EV_REQ_CH_MODE,0,CAN_MODE_LISTEN,0);
    post(EV_REQ_CH_RATE,0,50000,2000000);
    normal(0);
    uint8_t bytes[8]={0}; CHECK(tx_entry_update(0,0,0x123,0,8,bytes,1)==TX_OK);
    config(0,1,0,1,false,1,10,100,0);
    CHECK(txseq_load_permille(0)>1000);
    CHECK(txseq_start(0,txseq_get(0)->revision)==TXSEQ_ERR_LOAD);
    CHECK(txseq_get(0)->state==TXSEQ_IDLE);
}

static void selected_channel_is_not_an_action(void)
{
    boot(); CHECK(ui_tx_select(1,0)==UI_REVIEW_PAGE);
    CHECK(ui_review_navigate(PG_TX)==UI_REVIEW_OK);
    CHECK(ui_tx_select(1,2)==UI_REVIEW_OK && ui_cursor()==TX_ENTRIES+2);
    CHECK(!tx_any_active() && tx_get()->e[1][2].sent==0);
    CHECK(ui_tx_select(-1,0)==UI_REVIEW_RANGE && ui_cursor()==TX_ENTRIES+2);
    CHECK(ui_tx_select(0,TX_ENTRIES)==UI_REVIEW_RANGE && ui_cursor()==TX_ENTRIES+2);
    post(EV_NTF_ASK,0,ASK_FORMAT,0);
    CHECK(ui_tx_select(0,0)==UI_REVIEW_DIALOG);
    sim_touch(2,1); CHECK(!ui_dialog_open());
    post(EV_COMBO,0,COMBO_LOCK,0);
    CHECK(ui_tx_select(0,0)==UI_REVIEW_LOCKED);
}

int main(void)
{
    sequential_groups_and_channel_isolation();
    one_id_cycle_and_no_zero_gap();
    late_ticks_skip_expired_positions();
    permission_has_no_probe_and_cancel_is_final();
    validation_defaults_and_edit_ownership();
    fifo_no_ack_restart_and_stop_all();
    wrap_and_load_limit();
    selected_channel_is_not_an_action();
    printf("UI ID-group send model: %u checks passed (shared TX/FIFO/ACK, no board claim)\n",checks);
    return 0;
}
