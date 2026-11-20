/* Device-local timing navigation: actions use only keys/touch, no web editor. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/power/power_sm.h"
#include "services/disp/disp_pipe.h"
#include "services/frame_ring/frame_ring.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include "sim/tx_sequence.h"
#include "sim/local_files_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void run(unsigned ms){for(unsigned i=0;i<ms;i+=10)sim_tick(10);}
static void post(ev_type_t type,int ch,int a,int b){CHECK(evt_post4(type,(uint8_t)ch,SRC_UI,a,b));app_tick(0);}
static void key(key_id_t k){post(EV_KEY,k,GEST_SHORT,0);}
static void touch(int row){post(EV_TOUCH_ITEM,0,row,0);}
static const char *json(void){static char out[65536];jw_t j;jw_init(&j,out,sizeof out);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return out;}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(power_running()&&power_get()->screen_on);CHECK(ui_review_navigate(PG_TX)==0);}
static void open_timing(void){CHECK(ui_local_open(0,1,0)==0);CHECK(ui_page()==PG_TXSET);touch(2);CHECK(ui_page()==PG_TXTIMING);}
static void keyboard_action(const char *label){
 const char *document=json(),*keys=strstr(document,"\"keys\":[");CHECK(keys!=NULL);char wanted[64];snprintf(wanted,sizeof wanted,"\"label\":\"%s\"",label);const char *found=strstr(keys,wanted);CHECK(found!=NULL);const char *begin=found;while(begin>document&&*begin!='{')begin--;int index=-1;CHECK(sscanf(begin,"{\"index\":%d",&index)==1&&index>=0);touch(index);
}
static void input_text(const char *value){CHECK(strstr(json(),"\"page\":\"local_input\"")&&strstr(json(),"\"mode\":\"decimal\""));CHECK(local_files_keyboard_replace(local_files_keyboard_token(),value)==0);}
static void set_number(int row,const char *value){touch(row);input_text(value);keyboard_action("确定");CHECK(!local_files_draft());}
static void set_ten_repeats(void){set_number(2,"10");}
static void local_save_cancel_and_data(void){
 boot();can_frame_t old=tx_get()->e[1][0].f;open_timing();set_ten_repeats();
 CHECK(txseq_get(1)->entries[0].repeat_count==1);CHECK(strstr(json(),"\"repeat_count\":10"));
 set_number(3,"5"); /* Same-ID interval: 10 -> 5ms. */
 set_number(4,"200");
 CHECK(strstr(json(),"\"numeric\":false")&&strstr(json(),"\"dirty\":true"));
 CHECK(ui_review_navigate(PG_HOME)==UI_REVIEW_DRAFT);CHECK(!ui_tx_edit_open(1,1));CHECK(ui_tx_select(0,0)==UI_REVIEW_DRAFT);
 touch(6);CHECK(ui_page()==PG_TX&&!ui_draft_open());
 const txseq_channel_t *q=txseq_get(1);CHECK(q->entries[0].repeat_count==10&&q->entries[0].frame_gap_ms==5&&q->entries[0].cycle_ms==200);
 CHECK(!memcmp(&old,&tx_get()->e[1][0].f,sizeof old));CHECK(!tx_any_active());
 CHECK(ui_local_open(0,1,0)==0);set_number(1,"17");
 CHECK(strstr(json(),"\"id_gap_ms\":17"));touch(11);CHECK(ui_page()==PG_TX&&txseq_get(1)->id_gap_ms==10);
 CHECK(ui_local_open(0,1,0)==0);touch(7);CHECK(strstr(json(),"\"dirty\":true"));CHECK(txseq_get(1)->entries[0].cycle_ms==200);touch(6);
 CHECK(txseq_get(1)->entries[0].repeat_count==10&&txseq_get(1)->entries[0].frame_gap_ms==10&&txseq_get(1)->entries[0].cycle_ms==100);
 CHECK(!memcmp(&old,&tx_get()->e[1][0].f,sizeof old));
}
static void slot_draft_and_conflicts(void){
 boot();open_timing();set_number(2,"2");
 touch(5);CHECK(ui_page()==PG_TXSET&&strstr(json(),"\"repeat_count\":2"));touch(3);CHECK(ui_page()==PG_TXTIMING);
 touch(1);CHECK(strstr(json(),"\"enabled\":false"));touch(7);CHECK(ui_page()==PG_TXSET);CHECK(strstr(json(),"\"repeat_count\":2"));
 touch(2);set_number(2,"3");
 txseq_entry_config_t changed[TX_ENTRIES];memcpy(changed,txseq_get(1)->entries,sizeof changed);changed[0].cycle_ms=150;
 CHECK(txseq_configure(1,changed,10,txseq_get(1)->revision)==0);
 touch(6);CHECK(ui_page()==PG_TXTIMING&&ui_draft_open());CHECK(strstr(json(),"\"stale\":true")&&strstr(json(),"\"repeat_count\":3"));
 CHECK(txseq_get(1)->entries[0].repeat_count==1&&txseq_get(1)->entries[0].cycle_ms==150);
 key(KEY_BACK);key(KEY_BACK);CHECK(ui_page()==PG_TX&&!ui_draft_open());
 open_timing();touch(3);input_text("11");key(KEY_BACK); /* Cancel only the keyboard draft. */
 CHECK(!strstr(json(),"\"dirty\":true"));touch(7);key(KEY_BACK);CHECK(ui_page()==PG_TX);
}
static void defaults_confirm_and_physical_entry(void){
 boot();CHECK(ui_tx_select(1,3)==0);key(KEY_DOWN);key(KEY_DOWN);CHECK(ui_cursor()==CH_NUM*TX_ENTRIES+1);key(KEY_OK);
 CHECK(ui_page()==PG_TXSET&&strstr(json(),"\"ch\":1"));
 touch(0);CHECK(strstr(json(),"\"ch\":0"));touch(0);CHECK(strstr(json(),"\"ch\":1"));
 txseq_entry_config_t other[TX_ENTRIES];memcpy(other,txseq_get(0)->entries,sizeof other);other[0].repeat_count=4;other[0].frame_gap_ms=25;other[0].cycle_ms=300;
 CHECK(txseq_configure(0,other,90,txseq_get(0)->revision)==0);
 can_frame_t before=tx_get()->e[0][0].f;touch(8);CHECK(txseq_get(0)->id_gap_ms==10&&txseq_get(0)->entries[0].repeat_count==4&&txseq_get(0)->entries[0].frame_gap_ms==10&&txseq_get(0)->entries[0].cycle_ms==100);
 CHECK(!memcmp(&before,&tx_get()->e[0][0].f,sizeof before));
 touch(9);CHECK(ui_dialog_open()&&txseq_get(1)->state==TXSEQ_CONFIRMING&&txseq_get(1)->sent==0);
 key(KEY_BACK);CHECK(!ui_dialog_open()&&txseq_get(1)->state==TXSEQ_IDLE&&txseq_get(1)->sent==0);
 touch(11);CHECK(ui_page()==PG_TX&&!ui_draft_open());
 CHECK(ui_local_open(-1,0,0)==UI_REVIEW_RANGE);CHECK(ui_local_open(0,2,0)==UI_REVIEW_RANGE);
 post(EV_COMBO,0,COMBO_LOCK,0);CHECK(ui_local_open(0,0,0)==UI_REVIEW_LOCKED);
}
static void invalid_times_do_not_commit(void){
 boot();open_timing();touch(2);input_text("0");keyboard_action("确定");CHECK(local_files_draft()&&local_files_error());CHECK(strstr(json(),"\"value\":\"0\""));CHECK(txseq_get(1)->entries[0].repeat_count==1);key(KEY_BACK);CHECK(ui_page()==PG_TXTIMING&&!local_files_draft());CHECK(strstr(json(),"\"repeat_count\":1"));touch(7);key(KEY_BACK);CHECK(ui_page()==PG_TX);
 open_timing();touch(1);set_number(2,"2");touch(3);input_text("0");keyboard_action("确定");CHECK(local_files_draft()&&local_files_error());CHECK(txseq_get(1)->entries[0].enabled&&txseq_get(1)->entries[0].repeat_count==1);key(KEY_BACK);CHECK(strstr(json(),"\"frame_gap_ms\":10"));touch(7);key(KEY_BACK);CHECK(ui_page()==PG_TX);
 open_timing();set_ten_repeats();touch(4);input_text("90");keyboard_action("确定");CHECK(local_files_draft()&&local_files_error());CHECK(txseq_get(1)->entries[0].repeat_count==1&&txseq_get(1)->entries[0].cycle_ms==100);key(KEY_BACK);CHECK(strstr(json(),"\"repeat_count\":10")&&strstr(json(),"\"cycle_ms\":100"));touch(7);key(KEY_BACK);CHECK(ui_page()==PG_TX&&!tx_any_active());
}
static void typed_keyboard_retains_field_ownership(void){
 boot();open_timing();touch(2);input_text("2");touch(3); /* Keyboard digit 3; not the underlying frame-gap row. */
 CHECK(local_files_draft()&&strstr(json(),"\"value\":\"23\""));CHECK(txseq_get(1)->entries[0].repeat_count==1&&txseq_get(1)->entries[0].frame_gap_ms==10);CHECK(ui_review_navigate(PG_HOME)==UI_REVIEW_DRAFT);CHECK(!ui_tx_edit_open(0,0));
 input_text("2");keyboard_action("确定");set_number(3,"11");CHECK(strstr(json(),"\"repeat_count\":2")&&strstr(json(),"\"frame_gap_ms\":11"));CHECK(txseq_get(1)->entries[0].repeat_count==1&&txseq_get(1)->entries[0].frame_gap_ms==10);
 touch(7);key(KEY_BACK);CHECK(ui_page()==PG_TX&&!ui_draft_open()&&!tx_any_active());
}
static void auxiliary_navigation_does_not_hide_editor(void){
 boot();CHECK(ui_local_open(2,0,0)==0);CHECK(strstr(json(),"\"page\":\"local_db\""));
 CHECK(ui_tx_edit_open(0,0));CHECK(ui_page()==PG_TXEDIT&&strstr(json(),"\"page\":\"txedit\""));
 key(KEY_BACK);CHECK(ui_page()==PG_TX&&!ui_draft_open());
 CHECK(ui_local_open(0,0,0)==0);post(EV_SYS_STOP_ALL,0,0,0);CHECK(ui_page()==PG_TX&&!ui_draft_open());
}
static void detail_to_local_draft(void){
 boot();CHECK(ui_review_navigate(PG_LIVE)==0);disp_init();
 can_frame_t f={.id=0x18ff50e5,.ch=0,.flags=CANF_EXT|CANF_FD|CANF_BRS|CANF_TX,.len=64,.ts_us=1000};
 for(unsigned i=0;i<64;i++)f.data[i]=(uint8_t)(0xa0+i);
 CHECK(ring_push(&f));app_capture_poll();CHECK(ui_live_open_detail(0)==0);CHECK(ui_page()==PG_DETAIL);
 can_frame_t before=tx_get()->e[1][2].f;uint32_t revision=tx_get()->e[1][2].revision;
 CHECK(ui_detail_to_tx(1,2)==0&&ui_page()==PG_TXEDIT&&ui_draft_open());
 CHECK(!memcmp(&before,&tx_get()->e[1][2].f,sizeof before)&&tx_get()->e[1][2].revision==revision);
 CHECK(strstr(json(),"\"ch\":1")&&strstr(json(),"\"entry\":2")&&strstr(json(),"\"len\":64")&&strstr(json(),"\"period_ms\":0"));
 CHECK(strstr(json(),"\"flags\":7")&&strstr(json(),"160,161,162,163"));
 CHECK(ui_detail_to_tx(0,0)==UI_REVIEW_DRAFT);touch(10);CHECK(ui_page()==PG_TX);
 CHECK(tx_get()->e[1][2].f.id==f.id&&tx_get()->e[1][2].f.ch==1&&tx_get()->e[1][2].f.flags==7);
 CHECK(!memcmp(tx_get()->e[1][2].f.data,f.data,64)&&!tx_any_active());
 CHECK(ui_review_navigate(PG_LIVE)==0);f.flags=CANF_ERR;CHECK(ring_push(&f));app_capture_poll();CHECK(ui_live_open_detail(0)==0);
 CHECK(ui_detail_to_tx(1,0)==UI_REVIEW_RANGE&&ui_page()==PG_DETAIL);
 key(KEY_BACK);CHECK(ui_page()==PG_LIVE);
}
int main(void){CHECK(PG_DETAIL==12&&PG_TXSET==13&&PG_TXTIMING==14);local_save_cancel_and_data();slot_draft_and_conflicts();defaults_confirm_and_physical_entry();invalid_times_do_not_commit();detail_to_local_draft();auxiliary_navigation_does_not_hide_editor();typed_keyboard_retains_field_ownership();printf("Device local grouped timing UI: %u checks passed\n",checks);return 0;}
