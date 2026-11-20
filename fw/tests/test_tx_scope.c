/* Both-channel manual operations share the real TX FIFO and ACK path. */
#include "sim/tx_scope.h"
#include "sim/tx_sequence.h"
#include "sim/send_plan.h"
#include "sim/sim_api.h"
#include "sim/env_sim.h"
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/power/power_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/frame_ring/frame_ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static ring_reader_t reader;
static can_frame_t frames[256];static unsigned count;
static void observe(void){const can_frame_t *f;while((f=ring_peek(&reader))){if(f->flags&CANF_TX){CHECK(count<256);frames[count++]=*f;}ring_advance(&reader);}}
static void run(unsigned ms){for(unsigned i=0;i<ms;i++){sim_tick(1);app_tick(0);observe();}}
static void post(ev_type_t type,int ch,int a,int b){CHECK(evt_post4(type,(uint8_t)ch,SRC_UI,a,b));app_tick(0);}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);for(int i=0;i<170;i++)sim_tick(10);sim_key(KEY_POWER,0);for(int i=0;i<5;i++)sim_tick(10);CHECK(power_running());env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);scope_init();count=0;ring_reader_attach(&reader,false);}
static void normal(int ch){post(EV_REQ_CH_MODE,ch,CAN_MODE_NORMAL,2);CHECK(chan_can_tx(ch));}
static void configure(int ch,unsigned repeat,unsigned gap,unsigned cycle){txseq_entry_config_t rows[4];memcpy(rows,txseq_get(ch)->entries,sizeof rows);for(int i=0;i<4;i++)rows[i].enabled=i==0;rows[0]=(txseq_entry_config_t){true,repeat,gap,cycle};CHECK(txseq_configure(ch,rows,7,txseq_get(ch)->revision)==0);}
static void own_lists_and_mask_stop(void){
 boot();normal(0);normal(1);configure(0,3,10,50);configure(1,2,7,30);uint32_t revision[2]={tx_get()->e[0][0].revision,tx_get()->e[1][0].revision};CHECK(scope_start(3)==0);CHECK(txseq_get(0)->state==TXSEQ_RUNNING&&txseq_get(1)->state==TXSEQ_RUNNING);run(22);CHECK(txseq_get(0)->sent==3&&txseq_get(1)->sent==2);CHECK(count==5);CHECK(frames[0].ch==0&&frames[0].id==0x123);CHECK(frames[1].ch==1&&frames[1].id==0x200);CHECK(frames[0].ts_us==frames[1].ts_us);CHECK(frames[0].data[0]==0x11&&frames[0].data[2]==0x33&&frames[1].data[2]==0x10);CHECK(frames[2].ch==1&&frames[2].ts_us-frames[1].ts_us==7000);CHECK(frames[3].ch==0&&frames[3].ts_us-frames[0].ts_us==10000);CHECK(tx_get()->e[0][0].revision==revision[0]&&tx_get()->e[1][0].revision==revision[1]);uint32_t sent1=txseq_get(1)->sent;scope_stop(1);app_tick(0);CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(1)->state==TXSEQ_RUNNING);run(45);CHECK(txseq_get(0)->sent==3&&txseq_get(1)->sent>sent1);scope_stop(2);
}
static void single_own_slot_and_atomic_invalid(void){
 boot();normal(0);normal(1);CHECK(scope_single(3,3)==0);CHECK(scope_single(3,0)==TXSEQ_ERR_ACTIVE);run(4);CHECK(count==2&&frames[0].id==0x7DF&&frames[1].id==0x100);CHECK(frames[0].len==8&&!(frames[0].flags&CANF_RTR));CHECK(frames[1].len==8&&(frames[1].flags&CANF_RTR));CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(1)->state==TXSEQ_IDLE);unsigned before=count;post(EV_REQ_CH_FD,1,0,0);CHECK(scope_single(3,2)==TXSEQ_ERR_FRAME);run(4);CHECK(count==before&&tx_get()->pending[0]==0&&tx_get()->pending[1]==0);CHECK(scope_single(0,0)==TXSEQ_ERR_ARGUMENT&&scope_single(3,4)==TXSEQ_ERR_ARGUMENT);
}
static void busy_second_never_starts_first(void){
 boot();normal(0);normal(1);configure(0,2,10,40);configure(1,2,10,40);CHECK(txseq_start(1,txseq_get(1)->revision)==0);CHECK(scope_start(3)==TXSEQ_ERR_ACTIVE);CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(1)->state==TXSEQ_RUNNING);run(2);CHECK(tx_get()->e[0][0].sent==0);scope_stop(2);run(2);post(EV_REQ_CH_FD,1,0,0);txseq_entry_config_t rows[4];memcpy(rows,txseq_get(1)->entries,sizeof rows);rows[2].enabled=true;CHECK(txseq_configure(1,rows,0,txseq_get(1)->revision)==0);CHECK(scope_start(3)==TXSEQ_ERR_FRAME);CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(1)->state==TXSEQ_IDLE);CHECK(tx_get()->e[0][0].sent==0);
}
static void aggregate_cancel_and_accept(void){
 boot();configure(0,2,10,40);configure(1,2,7,30);CHECK(scope_start(3)==TXSEQ_WAIT_CONFIRM);uint32_t request=scope_state()->request;CHECK(tx_get()->pending[0]==0&&tx_get()->pending[1]==0);app_tick(0);CHECK(ui_dialog_open());run(3);CHECK(count==0&&!txseq_any_active());sim_touch(2,1);CHECK(!ui_dialog_open()&&scope_state()->state==SCOPE_IDLE);CHECK(scope_answer(request,true)==TXSEQ_ERR_REQUEST);CHECK(scope_start(3)==TXSEQ_WAIT_CONFIRM);app_tick(0);CHECK(ui_dialog_open());sim_touch(2,0);CHECK(scope_state()->state==SCOPE_ARMING);CHECK(tx_get()->pending[0]==0&&tx_get()->pending[1]==0);run(3);CHECK(chan_can_tx(0)&&chan_can_tx(1));CHECK(txseq_get(0)->state==TXSEQ_RUNNING&&txseq_get(1)->state==TXSEQ_RUNNING);CHECK(count==2&&frames[0].ts_us==frames[1].ts_us);scope_stop(3);
}
static void stale_scope_and_shutdown_cancel(void){
 boot();CHECK(scope_single(3,0)==TXSEQ_WAIT_CONFIRM);uint32_t request=scope_state()->request;uint8_t byte=0xAA;CHECK(tx_entry_update(1,0,0x201,0,1,&byte,100)==TX_OK);CHECK(scope_answer(request,true)==TXSEQ_ERR_CHANGED);CHECK(tx_get()->pending[0]==0&&tx_get()->pending[1]==0);app_tick(0);sim_touch(2,1);CHECK(scope_start(3)==TXSEQ_WAIT_CONFIRM);request=scope_state()->request;scope_cancel();CHECK(scope_state()->state==SCOPE_IDLE&&scope_answer(request,true)==TXSEQ_ERR_REQUEST);app_tick(0);sim_touch(2,1);CHECK(scope_start(3)==TXSEQ_WAIT_CONFIRM);post(EV_REQ_SHUTDOWN,0,0,0);scope_tick();CHECK(scope_state()->state==SCOPE_IDLE&&!power_running());CHECK(tx_get()->pending[0]==0&&tx_get()->pending[1]==0&&count==0);
}
static void stop_all_during_arming_cannot_restart(void){
 boot();CHECK(scope_start(3)==TXSEQ_WAIT_CONFIRM);app_tick(0);sim_touch(2,0);CHECK(scope_state()->state==SCOPE_ARMING);CHECK(tx_get()->pending[0]==0&&tx_get()->pending[1]==0);post(EV_REQ_TX_STOP_ALL,0,0,0);CHECK(scope_state()->state==SCOPE_IDLE);run(30);CHECK(count==0&&!txseq_any_active());
 boot();CHECK(scope_single(3,0)==TXSEQ_WAIT_CONFIRM);app_tick(0);sim_touch(2,0);CHECK(scope_state()->state==SCOPE_ARMING);post(EV_REQ_TX_STOP_ALL,0,0,0);run(5);CHECK(count==0&&tx_get()->pending[0]==0&&tx_get()->pending[1]==0);
}
static void keeps_plan_owner(void){
 boot();normal(0);normal(1);send_plan_t p;CHECK(sp_capture(1,&p)==0);sp_file_t file;memset(&file,0,sizeof file);strcpy(file.name,"saved.canplan.json");strcpy(file.project,"方案项目");file.plan=p;CHECK(sp_catalog_replace(&file,1)==0&&sp_catalog_get(0,&file));CHECK(sp_load(0,file.token)==0);send_plan_t loaded=*sp_loaded_plan();CHECK(sp_start()==0);run(3);CHECK(scope_start(2)==0);run(3);CHECK(sp_state()->loaded_mask==1&&!memcmp(&loaded,sp_loaded_plan(),sizeof loaded));sp_stop();app_tick(0);CHECK(txseq_get(1)->state==TXSEQ_RUNNING);scope_stop(2);
}
static void active_plan_keeps_stopped_channel_ownership(void){
 boot();normal(0);normal(1);send_plan_t p;CHECK(sp_capture(3,&p)==0);sp_file_t file;memset(&file,0,sizeof file);strcpy(file.name,"owned.canplan.json");file.plan=p;CHECK(sp_catalog_replace(&file,1)==0&&sp_catalog_get(0,&file));CHECK(sp_load(0,file.token)==0&&sp_start()==0);run(3);scope_stop(1);app_tick(0);CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(1)->state==TXSEQ_RUNNING&&sp_state()->state==SP_RUNNING);CHECK(scope_start(1)==TXSEQ_ERR_CONFLICT);CHECK(txseq_get(0)->state==TXSEQ_IDLE);sp_stop();app_tick(0);run(3);CHECK(scope_start(1)==0);scope_stop(1);
}
int main(void){own_lists_and_mask_stop();single_own_slot_and_atomic_invalid();busy_second_never_starts_first();aggregate_cancel_and_accept();stale_scope_and_shutdown_cancel();stop_all_during_arming_cannot_restart();keeps_plan_owner();active_plan_keeps_stopped_channel_ownership();printf("Manual scope atomic validation / own-list TX: %u checks passed\n",checks);return 0;}
