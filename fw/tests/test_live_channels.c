/* Display identity and presentation: the same ID remains distinct by bus and direction. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/disp/disp_pipe.h"
#include "services/frame_ring/frame_ring.h"
#include "services/recorder/rec_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void run(unsigned ms){for(unsigned i=0;i<ms;i++)sim_tick(1);}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);disp_init();CHECK(ui_review_navigate(PG_LIVE)==0);}
static void push(unsigned ch,bool tx,uint64_t ts,uint8_t value){can_frame_t frame={.id=0x123,.flags=CANF_FD|(tx?CANF_TX:0),.len=64,.ch=(uint8_t)ch,.ts_us=ts};for(unsigned b=0;b<64;b++)frame.data[b]=(uint8_t)(value+b);CHECK(ring_push(&frame));app_capture_poll();}
static const char *json(void){static char data[65536];jw_t j;jw_init(&j,data,sizeof data);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return data;}
static unsigned occurrences(const char *text,const char *token){unsigned count=0;while((text=strstr(text,token))){count++;text+=strlen(token);}return count;}
static void key(key_id_t key,gesture_t gesture){CHECK(evt_post4(EV_KEY,(uint8_t)key,SRC_UI,gesture,0));app_tick(0);}
static void live_menu(void){sim_key(KEY_OK,1);run(1100);sim_key(KEY_OK,0);run(20);CHECK(ui_dialog_open()&&strstr(json(),"切换滚动 / 按 ID"));}
static void same_id_direction_and_channel_are_separate(void){
 boot();CHECK(ui_live_set_channel(0)==0);uint64_t base=app_now_us();uint8_t rec_mask=cfg_get()->rec_ch_mask;
 push(0,false,base,0x10);push(0,true,base+500,0x20);push(1,false,base+750,0x30);
 push(0,false,base+1000,0x11);push(0,true,base+2500,0x21);push(1,true,base+3000,0x40);
 CHECK(disp_id_count()==4&&disp_rows_available()==6);
 const disp_id_t *rx=disp_id_at(0),*tx=disp_id_at(1),*other_rx=disp_id_at(2),*other_tx=disp_id_at(3);
 CHECK(rx&&tx&&other_rx&&other_tx);
 CHECK(rx->ch==0&&!(rx->flags&CANF_TX)&&rx->count==2&&rx->per_avg_us==1000&&rx->data[63]==0x50);
 CHECK(tx->ch==0&&(tx->flags&CANF_TX)&&tx->count==2&&tx->per_avg_us==2000&&tx->data[63]==0x60);
 CHECK(other_rx->ch==1&&!(other_rx->flags&CANF_TX)&&other_rx->count==1&&other_rx->data[0]==0x30);
 CHECK(other_tx->ch==1&&(other_tx->flags&CANF_TX)&&other_tx->count==1&&other_tx->data[0]==0x40);
 disp_row_t frame;CHECK(disp_row(1,&frame)&&frame.ch==0&&(frame.flags&CANF_TX)&&frame.dt_same_us==2000);
 CHECK(disp_row(2,&frame)&&!(frame.flags&CANF_TX)&&frame.dt_same_us==1000);
 CHECK(occurrences(json(),"\"direction\":\"TX\"")==3&&occurrences(json(),"\"direction\":\"RX\"")==3);
 CHECK(ui_live_set_view(1)==0);CHECK(occurrences(json(),"\"direction\":\"TX\"")==2&&occurrences(json(),"\"tx\":true")==2);
 CHECK(ui_live_open_detail(1)==0);CHECK(strstr(json(),"\"tx\":true")&&strstr(json(),"\"count\":2")&&strstr(json(),"\"period_avg_us\":2000"));
 push(0,false,base+4000,0x12);push(0,true,base+4500,0x22);
 CHECK(strstr(json(),"\"count\":2")&&strstr(json(),"\"period_avg_us\":2000"));
 CHECK(evt_post4(EV_KEY,KEY_BACK,SRC_UI,GEST_SHORT,0));app_tick(0);
 CHECK(ui_page()==PG_LIVE&&disp_id_at(0)->count==3&&disp_id_at(1)->count==3);
 CHECK(cfg_get()->rec_ch_mask==rec_mask&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
}
static void channel_projection_keeps_history_context_and_recording(void){
 boot();CHECK(strstr(json(),"\"channel\":1"));uint64_t base=app_now_us();
 for(unsigned i=0;i<10;i++)push(i%2,(i%3)==0,base+i*1000u,(uint8_t)(0x10+i));
 disp_filter_t filter;disp_filter_get_ex(&filter);CHECK(filter.channel==DF_CH_ALL&&filter.direction==DF_DIR_ALL);
 CHECK(disp_rows_available()==10&&disp_id_count()==4);
 CHECK(strstr(json(),"\"total\":5")&&strstr(json(),"\"cached_total\":10")&&!strstr(json(),"\"ch\":2"));
 uint32_t other_seq,selected_seq;CHECK(disp_row_seq(0,&other_seq)&&disp_row_seq(5,&selected_seq));
 CHECK(ui_live_open_detail_seq(other_seq)==UI_REVIEW_RANGE&&ui_page()==PG_LIVE);
 key(KEY_UP,GEST_SHORT);key(KEY_UP,GEST_SHORT);CHECK(ui_cursor()==2&&disp_paused());
 CHECK(ui_live_set_channel(2)==0&&ui_cursor()==0&&disp_paused());key(KEY_UP,GEST_SHORT);CHECK(ui_cursor()==1);
 CHECK(!strstr(json(),"\"ch\":1")&&strstr(json(),"\"total\":5"));
 CHECK(ui_live_set_channel(1)==0&&ui_cursor()==2&&disp_paused());
 CHECK(ui_live_open_detail_seq(selected_seq)==0&&ui_page()==PG_DETAIL);
 CHECK(strstr(json(),"\"ch\":1")&&strstr(json(),"\"source_index\":2")&&strstr(json(),"\"source_channel\":1"));
 char wanted[64];snprintf(wanted,sizeof wanted,"\"seq\":%u",selected_seq);CHECK(strstr(json(),wanted));
 CHECK(ui_live_set_channel(2)==UI_REVIEW_PAGE);push(0,false,base+11000,0x51);push(1,true,base+12000,0x61);
 CHECK(disp_rows_available()==10);key(KEY_BACK,GEST_SHORT);CHECK(ui_page()==PG_LIVE&&ui_cursor()==2&&disp_paused());
 CHECK(strstr(json(),"\"channel\":1"));CHECK(ui_live_set_channel(2)==0&&ui_cursor()==1);
 CHECK(ui_live_set_view(1)==0&&strstr(json(),"\"total\":2"));key(KEY_DOWN,GEST_SHORT);CHECK(ui_cursor()==1);
 CHECK(occurrences(json(),"\"direction\":\"TX\"")==1&&occurrences(json(),"\"direction\":\"RX\"")==1);
 CHECK(ui_live_set_channel(1)==0&&ui_cursor()==0);CHECK(ui_live_set_channel(2)==0&&ui_cursor()==1);
 CHECK(ui_live_set_view(0)==0&&ui_cursor()==1);key(KEY_LEFT,GEST_SHORT);CHECK(ui_cursor()==2&&strstr(json(),"\"channel\":1"));
 key(KEY_RIGHT,GEST_SHORT);CHECK(ui_cursor()==1&&strstr(json(),"\"channel\":2"));
 live_menu();
 key(KEY_OK,GEST_SHORT);CHECK(!ui_dialog_open()&&strstr(json(),"\"view\":\"byid\"")&&ui_cursor()==1);
 live_menu();
 key(KEY_OK,GEST_SHORT);CHECK(strstr(json(),"\"view\":\"scroll\"")&&ui_cursor()==1);
 live_menu();key(KEY_DOWN,GEST_SHORT);key(KEY_OK,GEST_SHORT);CHECK(ui_page()==PG_FILTER);
 CHECK(ui_live_set_channel(1)==UI_REVIEW_PAGE);key(KEY_BACK,GEST_SHORT);
 CHECK(ui_page()==PG_LIVE&&ui_cursor()==1&&disp_paused()&&strstr(json(),"\"channel\":2"));
 live_menu();key(KEY_DOWN,GEST_SHORT);key(KEY_DOWN,GEST_SHORT);key(KEY_OK,GEST_SHORT);CHECK(ui_page()==PG_REC);
 key(KEY_BACK,GEST_SHORT);CHECK(ui_page()==PG_LIVE&&ui_cursor()==1&&disp_paused()&&strstr(json(),"\"channel\":2"));
 disp_filter_t after;disp_filter_get_ex(&after);CHECK(after.channel==filter.channel&&after.type==filter.type&&after.direction==filter.direction&&after.mask==filter.mask);
 CHECK(disp_rows_available()==10&&cfg_get()->rec_ch_mask==3&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
 CHECK(sim_rec_action(0)==0);run(200);uint32_t recorded=rec_get()->frames;
 push(0,false,base+13000,0x71);push(1,false,base+14000,0x72);CHECK(rec_get()->frames==recorded+2&&cfg_get()->rec_ch_mask==3);
 CHECK(sim_rec_action(1)==0&&ui_live_set_channel(1)==UI_REVIEW_DRAFT);run(200);CHECK(ui_live_set_channel(1)==UI_REVIEW_DRAFT);key(KEY_BACK,GEST_SHORT);
 CHECK(ui_live_set_channel(3)==UI_REVIEW_RANGE);CHECK(evt_post4(EV_COMBO,0,SRC_UI,COMBO_LOCK,0));app_tick(0);CHECK(ui_live_set_channel(1)==UI_REVIEW_LOCKED);
}
int main(void){same_id_direction_and_channel_are_separate();channel_projection_keeps_history_context_and_recording();printf("Display channel / RX-TX identity and summary presentation: %u checks passed\n",checks);return 0;}
