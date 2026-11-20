/* Choosing a send file directly replaces the included lists without sending. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/env_sim.h"
#include "sim/send_plan.h"
#include "sim/sim_api.h"
#include "sim/tx_scope.h"
#include "sim/tx_sequence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void run(unsigned ms){for(unsigned i=0;i<ms;i++)sim_tick(1);}
static void post(ev_type_t type,int ch,int a,int b){CHECK(evt_post4(type,(uint8_t)ch,SRC_UI,a,b));app_tick(0);}
static void key(key_id_t k){post(EV_KEY,k,GEST_SHORT,0);}
static void touch(int index){post(EV_TOUCH_ITEM,0,index,0);}
static const char *json(void){static char output[65536];jw_t j;jw_init(&j,output,sizeof output);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return output;}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(ui_review_navigate(PG_TX)==0);}
static sp_file_t make_file(const char *name,uint32_t mask,uint32_t id_base){
 sp_file_t file={0};snprintf(file.name,sizeof file.name,"%s",name);CHECK(sp_capture(mask,&file.plan)==0);
 for(unsigned ch=0;ch<CH_NUM;ch++)if(mask&(1u<<ch)){
  file.plan.channels[ch].id_gap_ms=7u+ch;
  for(unsigned index=0;index<TX_ENTRIES;index++){
   send_plan_entry_t *entry=&file.plan.channels[ch].entries[index];entry->id=id_base+ch*0x100u+index;
   entry->flags=index?0:CANF_FD|CANF_BRS;entry->length=index?8:64;entry->period_ms=index?100u+index:0;
   memset(entry->data,0,sizeof entry->data);for(unsigned byte=0;byte<entry->length;byte++)entry->data[byte]=(uint8_t)(0x20u+ch*0x40u+index+byte);
   memset(entry->dynamic,0,sizeof entry->dynamic);if(!index){entry->dynamic[1]=8;entry->dynamic[2]=255;entry->dynamic[3]=9;}
   entry->enabled=index==0;entry->repeat_count=index+1;entry->frame_gap_ms=3;entry->cycle_ms=50u+index*10u;
  }
 }
 return file;
}
static void install_files(void){sp_file_t files[]={make_file("CAN2.canplan.json",2,0x100),make_file("Both.canplan.json",3,0x300),make_file("CAN1.canplan.json",1,0x500)};CHECK(sp_catalog_replace(files,3)==0);}
static void direct_load_scope_and_return_paths(void){
 boot();install_files();CHECK(ui_tx_scope_select(3)==0);tx_entry_t original[TX_ENTRIES];memcpy(original,tx_get()->e[0],sizeof original);
 txseq_channel_t timing=*txseq_get(0);uint32_t nbr=cfg_get()->nbr[1],dbr=cfg_get()->dbr[1];bool fd=cfg_get()->fd[1];
 CHECK(ui_plan_open(0,0)==0&&strstr(json(),"\"send_files_return\":\"tx\""));
 CHECK(strstr(json(),"加载并替换 CAN2 列表；不发送"));key(KEY_OK);
 CHECK(ui_page()==PG_TX&&ui_cursor()==TX_ENTRIES&&!ui_dialog_open()&&!tx_any_active());
 CHECK(strstr(json(),"\"tx_channel\":1")&&strstr(json(),"\"tx_scope_mask\":2")&&strstr(json(),"\"tx_load_file_index\":16"));
 CHECK(!memcmp(original,tx_get()->e[0],sizeof original)&&!memcmp(&timing,txseq_get(0),sizeof timing));
 CHECK(chan_get(1)->state==CH_LISTEN&&cfg_get()->nbr[1]==nbr&&cfg_get()->dbr[1]==dbr&&cfg_get()->fd[1]==fd);
 CHECK(tx_get()->e[1][0].f.id==0x200&&tx_get()->e[1][0].f.len==64&&tx_get()->e[1][0].dyn.cnt_val==9);
 for(unsigned byte=0;byte<64;byte++)CHECK(tx_get()->e[1][0].f.data[byte]==(uint8_t)(0x60u+byte));
 CHECK(txseq_get(1)->id_gap_ms==8&&txseq_get(1)->entries[3].repeat_count==4&&txseq_get(1)->entries[3].frame_gap_ms==3&&txseq_get(1)->entries[3].cycle_ms==80&&!txseq_get(1)->entries[3].enabled);
 CHECK(tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
 CHECK(ui_tx_select(1,3)==0&&ui_tx_scope_select(1)==0);CHECK(ui_plan_open(0,0)==0);key(KEY_BACK);
 CHECK(ui_page()==PG_TX&&ui_cursor()==7&&strstr(json(),"\"tx_channel\":1")&&strstr(json(),"\"tx_scope_mask\":1"));
 for(unsigned index=7;index<16;index++)key(KEY_DOWN);CHECK(ui_cursor()==16);key(KEY_OK);CHECK(ui_page()==PG_PROJECTFILES);key(KEY_BACK);CHECK(ui_page()==PG_TX&&ui_cursor()==16);
 CHECK(ui_review_navigate(PG_SENDMODE)==0&&ui_page()==PG_TX);touch(16);CHECK(strstr(json(),"\"send_files_return\":\"tx\""));key(KEY_BACK);CHECK(ui_page()==PG_TX);
 touch(16);touch(1);CHECK(ui_page()==PG_TX&&ui_cursor()==4&&strstr(json(),"\"tx_scope_mask\":3")&&strstr(json(),"\"tx_channel\":1"));
 CHECK(tx_get()->e[0][0].f.id==0x300&&tx_get()->e[1][0].f.id==0x400&&!tx_any_active()&&!ui_dialog_open());
 CHECK(ui_plan_open(0,1)==0);touch(2);CHECK(ui_page()==PG_TX&&ui_cursor()==0&&strstr(json(),"\"tx_scope_mask\":1")&&strstr(json(),"\"tx_channel\":0"));
 CHECK(tx_get()->e[0][0].f.id==0x500&&tx_get()->e[1][0].f.id==0x400&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
 CHECK(ui_tx_edit_open(0,0));CHECK(ui_plan_open(0,0)==UI_REVIEW_DRAFT&&ui_page()==PG_TXEDIT);key(KEY_BACK);
}
static void failed_or_pending_load_keeps_the_old_lists(void){
 boot();install_files();post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,2);post(EV_REQ_TX_CYC,0,1,3);
 CHECK(tx_get()->e[0][1].active);CHECK(ui_tx_scope_select(2)==0);CHECK(ui_plan_open(0,0)==0);
 tx_entry_t before[CH_NUM][TX_ENTRIES];memcpy(before,tx_get()->e,sizeof before);touch(1);
 CHECK(ui_page()==PG_PROJECTFILES&&ui_cursor()==1&&strstr(json(),"\"tx_scope_mask\":2")&&strstr(json(),"\"tx_channel\":0"));
 CHECK(!memcmp(before,tx_get()->e,sizeof before)&&sp_state()->loaded_mask==0&&!ui_dialog_open());
 post(EV_REQ_TX_CYC,0,1,0);run(5);CHECK(sp_request_refresh()==0);memcpy(before,tx_get()->e,sizeof before);touch(0);
 CHECK(ui_page()==PG_PROJECTFILES&&ui_cursor()==0&&!memcmp(before,tx_get()->e,sizeof before));
 sp_io_request_t request;CHECK(sp_io_peek(&request)&&sp_io_complete(request.id,0,"目录已刷新")==0);key(KEY_BACK);CHECK(ui_page()==PG_TX);
 boot();install_files();CHECK(scope_start(3)==TXSEQ_WAIT_CONFIRM);app_tick(0);post(EV_TOUCH_DLG,0,0,0);CHECK(scope_state()->state==SCOPE_ARMING);
 CHECK(ui_plan_open(0,0)==0);memcpy(before,tx_get()->e,sizeof before);touch(1);
 CHECK(ui_page()==PG_PROJECTFILES&&ui_cursor()==1&&!memcmp(before,tx_get()->e,sizeof before)&&scope_state()->state==SCOPE_ARMING&&sp_state()->loaded_mask==0);
 scope_cancel();
}
static void unrelated_manual_channel_keeps_running(void){
 boot();install_files();post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,2);CHECK(txseq_start(0,txseq_get(0)->revision)==0);run(2);
 CHECK(txseq_get(0)->state==TXSEQ_RUNNING);can_frame_t frame=tx_get()->e[0][0].f;uint32_t revision=txseq_get(0)->revision,sent=txseq_get(0)->sent;
 CHECK(ui_plan_open(0,0)==0);touch(0);CHECK(ui_page()==PG_TX&&txseq_get(0)->state==TXSEQ_RUNNING);
 CHECK(txseq_get(0)->revision==revision&&!memcmp(&frame,&tx_get()->e[0][0].f,sizeof frame)&&strstr(json(),"\"tx_scope_mask\":2"));
 run(150);CHECK(txseq_get(0)->sent>sent&&txseq_get(1)->state==TXSEQ_IDLE);scope_stop(1);
}
int main(void){direct_load_scope_and_return_paths();failed_or_pending_load_keeps_the_old_lists();unrelated_manual_channel_keeps_running();printf("Direct send-file load / scope / return / atomic rejection UI: %u checks passed\n",checks);return 0;}
