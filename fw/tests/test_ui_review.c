/* UI review contract, no hardware driver calls or workload optimization. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/disp/disp_pipe.h"
#include "services/frame_ring/frame_ring.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void run(unsigned ms) {for(unsigned i=0;i<ms;i+=10)sim_tick(10);}
static void post(ev_type_t type,int ch,int a,int b) {CHECK(evt_post4(type,ch,SRC_UI,a,b));app_tick(0);}
static void key(key_id_t k,gesture_t g) {post(EV_KEY,k,g,0);}
static void boot(void) {
 sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(520);
 env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);disp_init();
 CHECK(power_running());CHECK(power_get()->screen_on);CHECK(ui_review_navigate(PG_LIVE)==0);CHECK(ui_live_set_channel(0)==0); /* Legacy mixed-channel fixtures. */
}
static can_frame_t frame(uint32_t id,uint8_t ch,uint8_t flags,uint8_t length) {
 can_frame_t f={.id=id,.ch=ch,.flags=flags,.len=length,.ts_us=app_now_us()};
 for(unsigned i=0;i<length;i++)f.data[i]=(uint8_t)(i+0x20);
 return f;
}
static void push(can_frame_t f) {CHECK(ring_push(&f));app_capture_poll();}
static const char *json(void) {
 static char out[65536];jw_t j;jw_init(&j,out,sizeof out);jw_obj(&j);ui_json(&j);jw_end_obj(&j);
 CHECK(jw_finish(&j)>0 && !j.overflow);return out;
}
static void full_detail_and_stability(void) {
 boot();can_frame_t first=frame(0x18ff50e5,1,CANF_FD|CANF_BRS|CANF_EXT,64);push(first);
 uint32_t seq;CHECK(disp_row_seq(0,&seq));
 can_frame_t newer=frame(0x321,0,0,8);newer.ts_us++;push(newer);
 CHECK(ui_live_open_detail_seq(seq)==0);CHECK(ui_page()==PG_DETAIL && disp_paused());
 const char *s=json();CHECK(strstr(s,"\"page\":\"detail\""));CHECK(strstr(s,"\"len\":64"));
 CHECK(strstr(s,"\"available_len\":64")&&strstr(s,"\"truncated\":false"));
 CHECK(strstr(s,"\"data\":[32,33,34,35") && strstr(s,",92,93,94,95]"));
 CHECK(strstr(s,"\"source_index\":1") && strstr(s,"\"ch\":2"));
 char before[4096];strcpy(before,s);
 first.data[63]=0xab;first.ts_us+=1000;push(first);CHECK(!strcmp(before,json()));
 CHECK(ui_live_set_paused(false)==UI_REVIEW_PAGE);CHECK(ui_review_filter_open()==UI_REVIEW_PAGE);
 key(KEY_BACK,GEST_SHORT);CHECK(ui_page()==PG_LIVE&&!disp_paused()&&ui_cursor()==1);
 CHECK(ui_live_set_paused(true)==0);key(KEY_UP,GEST_SHORT);int cursor=ui_cursor();
 CHECK(ui_live_open_detail((unsigned)cursor)==0);key(KEY_BACK,GEST_SHORT);
 CHECK(ui_page()==PG_LIVE&&disp_paused()&&ui_cursor()==cursor);
 CHECK(ui_review_navigate(PG_LIVE)==0 && disp_paused()); /* same tab is a no-op */
 CHECK(ui_review_navigate(PG_DIAG)==0 && !disp_paused());
}
static void sequences_filters_and_drafts(void) {
 boot();can_frame_t f=frame(0x123,0,0,8);push(f);uint32_t oldest;CHECK(disp_row_seq(0,&oldest));
 for(unsigned i=0;i<DISP_ROWS;i++){f.ts_us++;push(f);}
 CHECK(ui_live_open_detail_seq(oldest)==UI_REVIEW_RANGE && ui_page()==PG_LIVE);
 CHECK(ui_live_set_view(1)==0);CHECK(ui_live_set_view(2)==UI_REVIEW_RANGE);
 CHECK(ui_live_set_paused(true)==0);int cursor=ui_cursor();
 CHECK(ui_review_filter_open()==0&&ui_page()==PG_FILTER);
 CHECK(ui_review_navigate(PG_HOME)==UI_REVIEW_DRAFT&&ui_page()==PG_FILTER);
 key(KEY_BACK,GEST_SHORT);CHECK(ui_page()==PG_LIVE&&disp_paused()&&ui_cursor()==cursor);
 CHECK(strstr(json(),"\"view\":\"byid\""));
 CHECK(ui_review_filter_open()==0);post(EV_TOUCH_ITEM,0,0,0); /* change channel draft */
 post(EV_TOUCH_ITEM,0,6,0);CHECK(ui_page()==PG_LIVE&&!disp_paused());
 disp_filter_t filter;disp_filter_get_ex(&filter);CHECK(filter.channel==DF_CH_1);
 CHECK(ui_review_navigate(PG_TX)==0);CHECK(ui_tx_edit_open(0,0));
 CHECK(ui_review_navigate(PG_HOME)==UI_REVIEW_DRAFT&&ui_page()==PG_TXEDIT);
 key(KEY_BACK,GEST_SHORT);CHECK(ui_page()==PG_TX);
 CHECK(ui_review_navigate(PG_HOME)==0);CHECK(ui_review_navigate(PG_ENG)==UI_REVIEW_RANGE);
}
static void id_freeze_and_formats(void) {
 boot();can_frame_t f=frame(0x123,0,CANF_FD,64);push(f);
 f.ts_us+=1000;f.data[63]++;push(f);
 CHECK(disp_id_count()==1&&disp_id_at(0)->count==2);
 CHECK(disp_id_byte_changed(disp_id_at(0),63));
 CHECK(ui_live_set_view(1)==0&&ui_live_set_paused(true)==0);
 uint64_t frozen_time=disp_view_time_us();run(500);CHECK(disp_view_time_us()==frozen_time);
 f.ts_us+=1000;f.data[63]++;push(f);CHECK(disp_id_at(0)->count==2);
 CHECK(ui_live_open_detail(0)==0);CHECK(strstr(json(),"\"count\":2"));
 CHECK(strstr(json(),"\"period_avg_us\":1000"));
 key(KEY_BACK,GEST_SHORT);CHECK(disp_paused()&&disp_id_at(0)->count==2);
 CHECK(ui_live_set_paused(false)==0&&disp_id_at(0)->count==3);
 can_frame_t classic=frame(0x123,0,0,8);push(classic);CHECK(disp_id_count()==2);
 CHECK(ui_live_open_detail(1)==0);CHECK(strstr(json(),"\"len\":8"));
 key(KEY_BACK,GEST_SHORT);CHECK(ui_live_set_view(0)==0);
 can_frame_t rtr=frame(0x123,0,CANF_RTR,8);push(rtr);CHECK(ui_live_open_detail(0)==0);
 CHECK(strstr(json(),"\"rtr\":true")&&strstr(json(),"\"available_len\":0")&&strstr(json(),"\"data\":[]"));
 key(KEY_BACK,GEST_SHORT);
 can_frame_t regular=frame(4,1,0,8);push(regular);
 can_frame_t error=frame(4,1,CANF_ERR,8);push(error);CHECK(ui_live_open_detail(0)==0);
 CHECK(strstr(json(),"\"err\":true")&&strstr(json(),"\"has_summary\":false"));
}
static void guard_and_no_side_effects(void) {
 sim_reset();CHECK(ui_review_navigate(PG_REC)==UI_REVIEW_NOT_READY);
 boot();uint32_t recframes=rec_get()->frames;rec_state_t state=rec_get()->state;
 for(page_t p=PG_HOME;p<=PG_SYSSET;p++)CHECK(ui_review_navigate(p)==0);
 CHECK(rec_get()->frames==recframes&&rec_get()->state==state&&!tx_any_active());
 post(EV_NTF_ASK,0,ASK_FORMAT,0);CHECK(ui_dialog_open());
 CHECK(ui_review_navigate(PG_HOME)==UI_REVIEW_DIALOG);
 CHECK(ui_live_set_view(1)==UI_REVIEW_DIALOG);CHECK(ui_live_set_paused(true)==UI_REVIEW_DIALOG);
 CHECK(ui_live_open_detail(0)==UI_REVIEW_DIALOG);CHECK(ui_dialog_ask()==ASK_FORMAT);
 key(KEY_BACK,GEST_SHORT);CHECK(!ui_dialog_open());
 post(EV_COMBO,0,COMBO_LOCK,0);CHECK(power_get()->locked);
 CHECK(ui_review_navigate(PG_HOME)==UI_REVIEW_LOCKED);
 CHECK(ui_live_set_paused(true)==UI_REVIEW_LOCKED);
}
int main(void) {
 CHECK(PG_FILTER==11&&PG_DETAIL==12);
 full_detail_and_stability();sequences_filters_and_drafts();id_freeze_and_formats();guard_and_no_side_effects();
 printf("UI review / full payload / frozen detail: %u checks passed\n",checks);return 0;
}
