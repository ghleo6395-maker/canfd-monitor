/* One normal target selection synchronizes list and mask, without controlling TX. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/core/app_evt.h"
#include "services/power/power_sm.h"
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
static void back(void){post(EV_KEY,KEY_BACK,GEST_SHORT,0);}
static const char *json(void){static char out[65536];jw_t j;jw_init(&j,out,sizeof out);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return out;}
static void target(unsigned mask,unsigned channel,int cursor){char expected[64];snprintf(expected,sizeof expected,"\"tx_scope_mask\":%u",mask);CHECK(strstr(json(),expected));snprintf(expected,sizeof expected,"\"tx_channel\":%u",channel);CHECK(strstr(json(),expected));CHECK(ui_cursor()==cursor);}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(ui_review_navigate(PG_TX)==0);}
static void atomic_targets_preserve_both_lists(void){
 boot();tx_entry_t entries[CH_NUM][TX_ENTRIES];memcpy(entries,tx_get()->e,sizeof entries);txseq_channel_t timing[CH_NUM]={*txseq_get(0),*txseq_get(1)};
 CHECK(ui_tx_select(1,3)==0&&ui_tx_scope_select(1)==0);target(1,1,7);
 CHECK(sim_tx_target_select(1)==0);target(1,0,0);
 CHECK(sim_tx_target_select(2)==0);target(2,1,4);
 CHECK(ui_tx_select(0,2)==0);target(2,0,2);CHECK(ui_tx_target_select(3)==0);target(3,0,0);
 CHECK(ui_tx_select(1,3)==0&&ui_tx_target_select(3)==0);target(3,1,4);
 CHECK(!memcmp(entries,tx_get()->e,sizeof entries)&&!memcmp(&timing[0],txseq_get(0),sizeof timing[0])&&!memcmp(&timing[1],txseq_get(1),sizeof timing[1]));
 CHECK(tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0&&!txseq_any_active()&&!ui_dialog_open());
 CHECK(ui_tx_select(0,3)==0);target(3,0,3);CHECK(ui_tx_scope_select(2)==0);target(2,0,3); /* Legacy APIs remain independent. */
}
static void guards_do_not_partially_change_selection(void){
 boot();CHECK(ui_tx_select(1,3)==0&&ui_tx_scope_select(1)==0);
 CHECK(sim_tx_target_select(0)==UI_REVIEW_RANGE&&sim_tx_target_select(4)==UI_REVIEW_RANGE&&sim_tx_target_select(UINT32_MAX)==UI_REVIEW_RANGE);target(1,1,7);
 CHECK(ui_tx_edit_open(1,0));CHECK(ui_tx_target_select(2)==UI_REVIEW_DRAFT);post(EV_TOUCH_ITEM,0,0,0);uint32_t token=local_files_keyboard_token();
 CHECK(token&&ui_tx_target_select(2)==UI_REVIEW_DRAFT&&local_files_keyboard_token()==token);back();back();target(1,1,7);
 post(EV_COMBO,0,COMBO_LOCK,0);CHECK(ui_tx_target_select(2)==UI_REVIEW_LOCKED);post(EV_COMBO,0,COMBO_LOCK,0);target(1,1,7);
 post(EV_NTF_ASK,0,ASK_FORMAT,0);CHECK(ui_tx_target_select(2)==UI_REVIEW_DIALOG);back();target(1,1,7);
 CHECK(ui_review_navigate(PG_LIVE)==0&&ui_tx_target_select(2)==UI_REVIEW_PAGE);CHECK(ui_review_navigate(PG_TX)==0);target(1,1,7);
 sim_reset();CHECK(sim_tx_target_select(2)==UI_REVIEW_NOT_READY);
}
static void valid_selection_cancels_only_an_uncommitted_request(void){
 boot();CHECK(sim_tx_target_select(2)==0);CHECK(scope_start(3)==TXSEQ_WAIT_CONFIRM);app_tick(0);
 CHECK(ui_tx_target_select(1)==UI_REVIEW_DIALOG&&scope_state()->state==SCOPE_CONFIRMING);
 post(EV_TOUCH_DLG,0,0,0);CHECK(scope_state()->state==SCOPE_ARMING);
 CHECK(ui_tx_target_select(0)==UI_REVIEW_RANGE&&scope_state()->state==SCOPE_ARMING);target(2,1,4);
 CHECK(sim_tx_target_select(2)==0&&scope_state()->state==SCOPE_IDLE);target(2,1,4); /* Same mask still cancels the old pending request. */
 run(30);CHECK(!txseq_any_active()&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
 boot();post(EV_REQ_CH_MODE,0,CAN_MODE_NORMAL,2);post(EV_REQ_CH_MODE,1,CAN_MODE_NORMAL,2);CHECK(txseq_start(0,txseq_get(0)->revision)==0);run(2);
 CHECK(txseq_get(0)->state==TXSEQ_RUNNING&&txseq_get(1)->state==TXSEQ_IDLE);uint32_t sent=txseq_get(0)->sent;
 CHECK(sim_tx_target_select(2)==0);target(2,1,4);CHECK(txseq_get(0)->state==TXSEQ_RUNNING&&txseq_get(1)->state==TXSEQ_IDLE);
 run(500);CHECK(txseq_get(0)->sent>sent&&tx_get()->echo_count[1]==0);
 CHECK(sim_tx_target_select(3)==0);target(3,1,4);CHECK(txseq_get(0)->state==TXSEQ_RUNNING&&txseq_get(1)->state==TXSEQ_IDLE);scope_stop(1);
}
int main(void){atomic_targets_preserve_both_lists();guards_do_not_partially_change_selection();valid_selection_cancels_only_an_uncommitted_request();printf("Unified TX target / atomic selection / guards / pending cancellation: %u checks passed\n",checks);return 0;}
