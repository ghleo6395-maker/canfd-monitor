/* Typed local field drafts, full CAN FD payloads and signed live-list movement. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/disp/disp_pipe.h"
#include "services/frame_ring/frame_ring.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/env_sim.h"
#include "sim/local_files_ui.h"
#include "sim/sim_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void run(unsigned ms){for(unsigned i=0;i<ms;i++)sim_tick(1);}
static void key(key_id_t key){CHECK(evt_post4(EV_KEY,(uint8_t)key,SRC_UI,GEST_SHORT,0));app_tick(0);}
static void touch(int row){CHECK(evt_post4(EV_TOUCH_ITEM,0,SRC_UI,row,0));app_tick(0);}
static const char *json(void){static char text[65536];jw_t j;jw_init(&j,text,sizeof text);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return text;}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);}
static void action(const char *label){const char *text=json(),*keys=strstr(text,"\"keys\":[");CHECK(keys!=NULL);char expected[80];snprintf(expected,sizeof expected,"\"label\":\"%s\"",label);const char *found=strstr(keys,expected);CHECK(found!=NULL);const char *begin=found;while(begin>text&&*begin!='{')begin--;int index;CHECK(sscanf(begin,"{\"index\":%d",&index)==1);touch(index);}
static void replace(const char *value){CHECK(local_files_keyboard_replace(local_files_keyboard_token(),value)==0);}
static void direct_entry_and_full_payload_draft(void){
 boot();touch(1);CHECK(ui_page()==PG_TX&&!ui_dialog_open());key(KEY_BACK);CHECK(ui_page()==PG_HOME);CHECK(ui_review_navigate(PG_SENDMODE)==0&&ui_page()==PG_TX);
 CHECK(ui_tx_edit_open(0,2));can_frame_t original=tx_get()->e[0][2].f;uint32_t revision=tx_get()->e[0][2].revision;
 touch(12);CHECK(strstr(json(),"\"page\":\"local_input\"")&&strstr(json(),"\"mode\":\"payload\"")&&strstr(json(),"\"parent_page\":\"txedit\""));
 char spaced[192];for(unsigned byte=0;byte<64;byte++)snprintf(spaced+byte*3u,sizeof spaced-byte*3u,byte==63?"%02X":"%02X ",original.data[byte]);
 CHECK(strlen(spaced)==191&&strstr(json(),spaced));action("确定");CHECK(!local_files_active()&&ui_page()==PG_TXEDIT&&tx_get()->e[0][2].revision==revision);
 touch(12);replace("DE AD");action("确定");CHECK(local_files_active()&&strstr(json(),"\"local_error\":true"));
 CHECK(!memcmp(&original,&tx_get()->e[0][2].f,sizeof original));action("取消");CHECK(ui_page()==PG_TXEDIT);
 char compact[129];for(unsigned byte=0;byte<64;byte++)snprintf(compact+byte*2u,3,"%02X",byte^0xa5u);
 touch(12);replace(compact);action("确定");CHECK(tx_get()->e[0][2].revision==revision&&!memcmp(&original,&tx_get()->e[0][2].f,sizeof original));
 touch(10);CHECK(ui_page()==PG_TX&&!ui_draft_open()&&tx_get()->e[0][2].f.len==64);
 for(unsigned byte=0;byte<64;byte++)CHECK(tx_get()->e[0][2].f.data[byte]==(uint8_t)(byte^0xa5u));
 CHECK(tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
}
static void field_validation_tokens_and_no_sd_dependency(void){
 boot();CHECK(ui_review_navigate(PG_TX)==0&&ui_tx_edit_open(0,0));uint32_t original=tx_get()->e[0][0].f.id;
 touch(0);uint32_t token=local_files_keyboard_token();replace("1FFFFFFF");action("确定");CHECK(local_files_active()&&strstr(json(),"\"local_error\":true")&&tx_get()->e[0][0].f.id==original);
 CHECK(local_files_keyboard_replace(token,"12Z")==LOCAL_FILES_ERR_ARGUMENT);action("取消");
 touch(0);CHECK(local_files_keyboard_token()!=token&&local_files_keyboard_replace(token,"123")==LOCAL_FILES_ERR_CHANGED);
 replace("1234567");CHECK(local_files_backspace_begin(local_files_keyboard_token())==0);run(600);CHECK(local_files_backspace_end(local_files_keyboard_token())==0);
 CHECK(!strstr(json(),"\"value\":\"1234567\""));
 env_cmd("sd_remove",0,0);run(20);replace("7FF");action("确定");CHECK(ui_page()==PG_TXEDIT&&!local_files_active()&&tx_get()->e[0][0].f.id==original);
 key(KEY_BACK);CHECK(ui_page()==PG_TX&&tx_get()->e[0][0].f.id==original);
 CHECK(ui_tx_edit_open(0,2));touch(5);replace("10");action("确定");CHECK(local_files_active()&&strstr(json(),"合法DLC"));action("取消");
 touch(6);CHECK(strstr(json(),"\"mode\":\"decimal\""));replace("65536");action("确定");CHECK(local_files_active());replace("65535");action("确定");
 CHECK(tx_get()->e[0][2].period_ms!=65535);key(KEY_BACK);CHECK(!ui_draft_open());
 CHECK(ui_tx_edit_open(0,0));touch(0);uint8_t data[1]={0xee};CHECK(tx_entry_update(0,0,0x456,0,1,data,0)==TX_OK);replace("123");action("确定");
 CHECK(local_files_active()&&strstr(json(),"已变化")&&tx_get()->e[0][0].f.id==0x456);action("取消");key(KEY_BACK);
}
static void signed_scroll_is_view_only(void){
 boot();disp_init();CHECK(ui_review_navigate(PG_LIVE)==0&&ui_live_set_channel(2)==0);CHECK(ui_live_scroll(128)==0&&!disp_paused()&&ui_cursor()==0);
 for(unsigned i=0;i<4;i++){can_frame_t frame={.id=0x120+i,.ch=(uint8_t)(i==3),.len=1,.ts_us=1000+i};CHECK(ring_push(&frame));app_capture_poll();}
 disp_filter_t before;disp_filter_get_ex(&before);CHECK(ui_live_set_channel(1)==0&&ui_live_scroll(128)==0&&ui_cursor()==2&&disp_paused());
 CHECK(ui_live_scroll(-128)==0&&ui_cursor()==0&&disp_paused());CHECK(ui_live_scroll(129)==UI_REVIEW_RANGE&&ui_live_scroll(-129)==UI_REVIEW_RANGE);
 CHECK(ui_live_scroll(0)==0&&disp_paused());CHECK(ui_live_set_paused(false)==0&&ui_live_set_view(1)==0);CHECK(ui_live_scroll(1)==0&&ui_cursor()==1&&!disp_paused());
 CHECK(ui_live_set_channel(2)==0&&ui_cursor()==0&&ui_live_set_channel(1)==0&&ui_cursor()==1);
 disp_filter_t after;disp_filter_get_ex(&after);CHECK(before.channel==after.channel&&before.mask==after.mask&&before.direction==after.direction&&tx_get()->echo_count[0]==0);
 CHECK(ui_review_navigate(PG_TX)==0&&ui_live_scroll(1)==UI_REVIEW_PAGE);
}
int main(void){direct_entry_and_full_payload_draft();field_validation_tokens_and_no_sd_dependency();signed_scroll_is_view_only();printf("Direct send / typed draft keyboard / full payload / signed scroll: %u checks passed\n",checks);return 0;}
