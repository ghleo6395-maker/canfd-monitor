/* Independent explicit CAN switches and two-press REC lifecycle. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/frame_ring/frame_ring.h"
#include "services/recorder/rec_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/env_sim.h"
#include "sim/local_files_ui.h"
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
static void rec_press(void){post(EV_KEY,KEY_REC,GEST_SHORT,0);}
static void back(void){post(EV_KEY,KEY_BACK,GEST_SHORT,0);}
static void touch(int index){post(EV_TOUCH_ITEM,0,index,0);}
static const char *json(void){static char out[65536];jw_t j;jw_init(&j,out,sizeof out);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return out;}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(ui_review_navigate(PG_TX)==0);}
static void normal(int ch){post(EV_REQ_CH_MODE,ch,CAN_MODE_NORMAL,2);}
static void configure(int ch){txseq_entry_config_t entries[TX_ENTRIES];memcpy(entries,txseq_get(ch)->entries,sizeof entries);for(int i=0;i<TX_ENTRIES;i++)entries[i].enabled=i==0;entries[0].repeat_count=2;entries[0].frame_gap_ms=5;entries[0].cycle_ms=40;CHECK(txseq_configure(ch,entries,3,txseq_get(ch)->revision)==0);}
static void independent_switches_are_idempotent(void){
 boot();normal(0);normal(1);configure(0);configure(1);can_frame_t original[CH_NUM][TX_ENTRIES];for(int ch=0;ch<CH_NUM;ch++)for(int i=0;i<TX_ENTRIES;i++)original[ch][i]=tx_get()->e[ch][i].f;
 CHECK(ui_tx_channel_send(0,true)==0);txseq_channel_t first=*txseq_get(0);CHECK(first.state==TXSEQ_RUNNING&&ui_tx_channel_send(0,true)==0&&!memcmp(&first,txseq_get(0),sizeof first));
 CHECK(ui_tx_channel_send(1,true)==0&&!memcmp(&first,txseq_get(0),sizeof first));run(12);CHECK(txseq_get(0)->sent==2&&txseq_get(1)->sent==2);
 txseq_channel_t second=*txseq_get(1);CHECK(ui_tx_channel_send(0,false)==0);app_tick(0);CHECK(txseq_get(0)->state==TXSEQ_IDLE&&!memcmp(&second,txseq_get(1),sizeof second));
 first=*txseq_get(0);CHECK(ui_tx_channel_send(0,false)==0&&!memcmp(&first,txseq_get(0),sizeof first));uint32_t sent0=txseq_get(0)->sent,sent1=txseq_get(1)->sent;
 run(90);CHECK(txseq_get(0)->sent==sent0&&txseq_get(1)->sent>sent1);second=*txseq_get(1);CHECK(ui_tx_channel_send(1,true)==0&&!memcmp(&second,txseq_get(1),sizeof second));
 for(int ch=0;ch<CH_NUM;ch++)for(int i=0;i<TX_ENTRIES;i++)CHECK(!memcmp(&original[ch][i],&tx_get()->e[ch][i].f,sizeof(can_frame_t)));
 CHECK(strstr(json(),"\"tx_scope_mask\":1")&&strstr(json(),"\"tx_channel\":0"));CHECK(ui_tx_channel_send(1,false)==0);app_tick(0);
 boot();normal(0);post(EV_REQ_TX_CYC,0,1,3);CHECK(tx_get()->e[0][1].active&&txseq_get(0)->state==TXSEQ_IDLE);
 uint32_t next=tx_get()->e[0][1].next_due;CHECK(ui_tx_channel_send(0,true)==0&&tx_get()->e[0][1].active&&tx_get()->e[0][1].next_due==next&&txseq_get(0)->state==TXSEQ_IDLE);
 CHECK(ui_tx_channel_send(0,false)==0);app_tick(0);CHECK(!tx_get()->e[0][1].active);
}
static void pending_and_guarded_switches(void){
 boot();CHECK(ui_tx_channel_send(0,true)==TXSEQ_WAIT_CONFIRM);app_tick(0);uint32_t request=scope_state()->request;
 CHECK(ui_tx_channel_send(0,true)==UI_REVIEW_DIALOG&&ui_tx_channel_send(0,false)==UI_REVIEW_DIALOG&&scope_state()->request==request);
 post(EV_TOUCH_DLG,0,0,0);CHECK(scope_state()->state==SCOPE_ARMING);
 CHECK(ui_tx_channel_send(0,true)==TXSEQ_WAIT_CONFIRM&&scope_state()->request==request);
 CHECK(ui_tx_channel_send(1,false)==0&&scope_state()->state==SCOPE_ARMING);CHECK(ui_tx_channel_send(2,true)==UI_REVIEW_RANGE&&scope_state()->state==SCOPE_ARMING);
 CHECK(ui_tx_channel_send(0,false)==0);run(20);CHECK(scope_state()->state==SCOPE_IDLE&&!txseq_any_active()&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
 CHECK(ui_tx_edit_open(0,0)&&ui_tx_channel_send(1,true)==UI_REVIEW_DRAFT);back();
 CHECK(ui_tx_channel_send(-1,true)==UI_REVIEW_RANGE);post(EV_COMBO,0,COMBO_LOCK,0);CHECK(ui_tx_channel_send(0,true)==UI_REVIEW_LOCKED);post(EV_COMBO,0,COMBO_LOCK,0);
 CHECK(ui_review_navigate(PG_LIVE)==0&&ui_tx_channel_send(0,true)==UI_REVIEW_PAGE);sim_reset();CHECK(ui_tx_channel_send(0,true)==UI_REVIEW_NOT_READY);
}
static void rec_short_starts_then_stops_without_marking(void){
 boot();rec_press();CHECK(rec_get()->state==REC_PREPARING);uint32_t session=rec_get()->session;post(EV_KEY,KEY_REC,GEST_REPEAT,0);CHECK(rec_get()->state==REC_PREPARING);run(200);
 for(int ch=0;ch<CH_NUM;ch++){can_frame_t frame={.id=(uint32_t)(0x100+ch),.ch=(uint8_t)ch,.len=1,.data={0x55}};CHECK(ring_push(&frame));app_capture_poll();}
 CHECK(rec_get()->frames==2);post(EV_KEY,KEY_REC,GEST_REPEAT,0);CHECK(rec_get()->state==REC_RUNNING&&rec_get()->markers==0);
 rec_press();CHECK(rec_get()->state==REC_STOPPING&&rec_get()->session==session);rec_press();post(EV_KEY,KEY_REC,GEST_REPEAT,0);run(200);
 CHECK(rec_get()->state==REC_IDLE&&rec_get()->session==session&&rec_get()->markers==0&&strstr(json(),"local_record_save"));
 uint32_t token=env_record_file_token(session);file_info_t file;int index=-1;for(int i=0;i<port_sd_file_count();i++)if(env_file_token(i)==token)index=i;
 CHECK(index>=0&&port_sd_file_info(index,&file)&&!file.writing&&file.frames==2);back();CHECK(!ui_draft_open());
 rec_press();uint32_t next=rec_get()->session;CHECK(rec_get()->state==REC_PREPARING&&next!=session);rec_press();run(200);CHECK(rec_get()->state==REC_IDLE&&rec_get()->markers==0&&strstr(json(),"local_record_save"));back();
 rec_press();run(200);CHECK(ui_review_navigate(PG_TX)==0);CHECK(evt_post4(EV_REQ_REC_STOP,0,SRC_HOST,0,1));app_tick(0);CHECK(rec_get()->state==REC_STOPPING);
 rec_press();CHECK(ui_page()==PG_TX);run(200);CHECK(!ui_draft_open()&&rec_get()->markers==0); /* STOPPING ignores REC, including navigation. */
}
static void rec_stop_preserves_typed_and_outer_drafts(void){
 boot();rec_press();run(200);uint32_t session=rec_get()->session;CHECK(ui_review_navigate(PG_TX)==0&&ui_tx_edit_open(0,0));touch(0);uint32_t token=local_files_keyboard_token();CHECK(local_files_keyboard_replace(token,"456")==0);
 rec_press();run(200);CHECK(rec_get()->state==REC_IDLE&&rec_get()->markers==0&&local_files_keyboard_token()==token&&strstr(json(),"\"value\":\"456\"")&&!strstr(json(),"local_record_save"));
 rec_press();CHECK(rec_get()->session==session&&rec_get()->state==REC_IDLE);local_files_touch(local_files_item_count()-2);CHECK(ui_page()==PG_TXEDIT&&ui_draft_open());touch(10);
 CHECK(tx_get()->e[0][0].f.id==0x456&&strstr(json(),"local_record_save")&&rec_get()->session==session);back();
 rec_press();run(200);CHECK(ui_review_navigate(PG_TX)==0&&ui_local_open(0,1,0)==0);touch(2);touch(2);token=local_files_keyboard_token();CHECK(local_files_keyboard_replace(token,"2")==0);
 rec_press();run(200);CHECK(rec_get()->state==REC_IDLE&&local_files_keyboard_token()==token&&strstr(json(),"local_input"));local_files_touch(local_files_item_count()-2);touch(6);
 CHECK(txseq_get(1)->entries[0].repeat_count==2&&strstr(json(),"local_record_save")&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);back();
 CHECK(ui_tx_edit_open(0,0));touch(0);token=local_files_keyboard_token();rec_press();CHECK(rec_get()->state==REC_IDLE&&local_files_keyboard_token()==token);back();back();
}
int main(void){independent_switches_are_idempotent();pending_and_guarded_switches();rec_short_starts_then_stops_without_marking();rec_stop_preserves_typed_and_outer_drafts();printf("Independent CAN switches / idempotence / REC two-press / preserved drafts: %u checks passed\n",checks);return 0;}
