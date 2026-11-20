/* User stop -> asynchronous close -> bound local naming, using device controls. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/disp/disp_pipe.h"
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
static void run(unsigned ms){for(unsigned i=0;i<ms;i++)sim_tick(1);}
static void post_src(ev_type_t type,int ch,int a,int b,src_t src){CHECK(evt_post4(type,(uint8_t)ch,src,a,b));app_tick(0);}
static void key(key_id_t k){post_src(EV_KEY,k,GEST_SHORT,0,SRC_UI);}
static void long_rec(void){post_src(EV_KEY,KEY_REC,GEST_LONG,0,SRC_UI);}
static const char *json(void){static char data[65536];jw_t j;jw_init(&j,data,sizeof data);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return data;}
static int row(const char *label){
 const char *at=strstr(json(),"\"items\":[");CHECK(at!=NULL);int index=0;char expected[160];snprintf(expected,sizeof expected,"\"l\":\"%s\"",label);
 while((at=strstr(at,"\"l\":"))){if(!strncmp(at,expected,strlen(expected)))return index;index++;at++;}
 CHECK(false);return -1;
}
static void tap(const char *label){post_src(EV_TOUCH_ITEM,0,row(label),0,SRC_UI);}
static void word(const char *ascii){for(const char *p=ascii;*p;p++){char label[2]={*p,0};tap(label);}}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(ui_review_navigate(PG_LIVE)==0);}
static file_info_t file_for(uint32_t token){file_info_t file;for(int index=0;index<port_sd_file_count();index++)if(env_file_token(index)==token&&port_sd_file_info(index,&file))return file;CHECK(false);return (file_info_t){0};}
static uint32_t start(void){CHECK(sim_rec_action(0)==0);run(200);CHECK(rec_get()->state==REC_RUNNING);uint32_t token=env_record_file_token(rec_get()->session);CHECK(token!=0);return token;}
static void api_stop_and_rename(void){
 boot();CHECK(ui_live_set_paused(true)==0);uint32_t token=start();file_info_t original=file_for(token);
 CHECK(sim_rec_action(1)==0&&rec_get()->state==REC_STOPPING&&ui_draft_open());
 CHECK(ui_review_navigate(PG_HOME)==UI_REVIEW_DRAFT&&sim_rec_action(0)==UI_REVIEW_DRAFT);
 key(KEY_BACK);CHECK(ui_page()==PG_LIVE&&ui_draft_open());run(300);
 CHECK(rec_get()->state==REC_IDLE&&strstr(json(),"\"page\":\"local_record_save\"")&&strstr(json(),original.name));
 CHECK(disp_paused()&&sim_rec_action(0)==UI_REVIEW_DRAFT);
 tap("清空");word("CON");tap("保存");CHECK(ui_draft_open()&&strstr(json(),"\"local_error\":true"));
 file_info_t unchanged=file_for(token);CHECK(!strcmp(unchanged.name,original.name));
 tap("清空");tap("路试");word("A");tap("保存");
 file_info_t renamed=file_for(token);CHECK(!strcmp(renamed.name,"路试A.log")&&!renamed.writing);
 CHECK(renamed.frames==original.frames&&ui_page()==PG_LIVE&&disp_paused()&&!ui_draft_open());
 uint32_t next=start();CHECK(next!=token);long_rec();run(300);file_info_t before=file_for(next);
 tap("保留默认名");file_info_t after=file_for(next);CHECK(!strcmp(before.name,after.name)&&!ui_draft_open());
}
static void settings_stop_and_preparing_stop(void){
 boot();CHECK(ui_live_set_paused(true)==0);uint32_t token=start();CHECK(ui_review_navigate(PG_REC)==0);
 post_src(EV_TOUCH_ITEM,0,0,0,SRC_UI);run(300);
 CHECK(ui_page()==PG_LIVE&&disp_paused()&&strstr(json(),"local_record_save"));file_info_t original=file_for(token);
 key(KEY_BACK);file_info_t kept=file_for(token);CHECK(!strcmp(original.name,kept.name)&&!ui_draft_open());
 CHECK(sim_rec_action(0)==0&&rec_get()->state==REC_PREPARING);uint32_t empty_token=env_record_file_token(rec_get()->session);
 long_rec();CHECK(ui_draft_open());run(300);CHECK(rec_get()->state==REC_IDLE&&strstr(json(),"local_record_save"));
 kept=file_for(empty_token);CHECK(!kept.writing&&kept.frames==0);key(KEY_BACK);CHECK(!ui_draft_open());
}
static void preexisting_edit_and_dialog_are_preserved(void){
 boot();uint32_t token=start();CHECK(ui_review_navigate(PG_TX)==0&&ui_tx_edit_open(1,0));uint32_t id=tx_get()->e[1][0].f.id;
 long_rec();run(300);CHECK(rec_get()->state==REC_IDLE&&ui_page()==PG_TXEDIT&&ui_draft_open());
 CHECK(strstr(json(),"\"page\":\"txedit\"")&&!strstr(json(),"local_record_save"));
 key(KEY_BACK);CHECK(ui_page()==PG_TX&&strstr(json(),"local_record_save")&&tx_get()->e[1][0].f.id==id);
 tap("保留默认名");CHECK(!ui_draft_open());file_info_t file=file_for(token);CHECK(!file.writing);
 CHECK(ui_review_navigate(PG_LIVE)==0);start();long_rec();post_src(EV_NTF_ASK,0,ASK_CH_NORMAL,0,SRC_UI);run(300);
 CHECK(ui_dialog_open()&&!strstr(json(),"local_record_save"));key(KEY_BACK);
 CHECK(!ui_dialog_open()&&strstr(json(),"local_record_save"));key(KEY_BACK);CHECK(!ui_draft_open());
}
static void non_user_and_failed_stops_do_not_prompt(void){
 boot();start();post_src(EV_REQ_REC_STOP,0,0,1,SRC_HOST);run(300);CHECK(rec_get()->state==REC_IDLE&&!ui_draft_open());
 start();post_src(EV_SD_WRITE_ERR,0,2,0,SRC_SYS);run(300);CHECK(rec_get()->state==REC_IDLE&&rec_get()->last_result==MSG_REC_WRITE_FAIL&&!ui_draft_open());
 start();CHECK(sim_rec_action(1)==0);post_src(EV_SD_CLOSE_DONE,0,SD_ERR_IO,0,SRC_SYS);run(300);CHECK(!ui_draft_open()&&!strstr(json(),"local_record_save"));
 start();sim_key(KEY_POWER,1);run(2200);sim_key(KEY_POWER,0);run(400);CHECK(power_get()->state==PWR_OFF&&!ui_draft_open());
}
int main(void){api_stop_and_rename();settings_stop_and_preparing_stop();preexisting_edit_and_dialog_are_preserved();non_user_and_failed_stops_do_not_prompt();printf("Recording close / bound local save / default retention UI: %u checks passed\n",checks);return 0;}
