/* A send file is a snapshot of the manual screen, not a timestamped log. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/power/power_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/local_files_ui.h"
#include "sim/send_plan.h"
#include "sim/sim_api.h"
#include "sim/tx_sequence.h"
#include "sim/env_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void run(unsigned ms){for(unsigned i=0;i<ms;i++)sim_tick(1);}
static void post(ev_type_t type,int ch,int a,int b){CHECK(evt_post4(type,(uint8_t)ch,SRC_UI,a,b));app_tick(0);}
static void key(key_id_t k){post(EV_KEY,k,GEST_SHORT,0);}
static void touch(int row){post(EV_TOUCH_ITEM,0,row,0);}
static const char *json(void){static char output[65536];jw_t j;jw_init(&j,output,sizeof output);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return output;}
static void boot(void){sim_reset();sp_init();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(power_running()&&power_get()->screen_on);CHECK(ui_review_navigate(PG_TX)==0);}
static int text_key(const char *label){
 const char *document=json(),*at=document;
 char wanted[160];snprintf(wanted,sizeof wanted,"\"label\":\"%s\"",label);
 const char *found=strstr(document,wanted);CHECK(found!=NULL);
 const char *begin=document;
 for(const char *p=document;p<found;p++)if(*p=='{')begin=p;
 int index=-1;CHECK(sscanf(begin,"{\"index\":%d",&index)==1&&index>=0);
 (void)at;return index;
}
static void write_name(const char *ascii,bool extension){
 touch(text_key("清空"));
 for(const char *p=ascii;*p;p++){char label[2]={*p,0};touch(text_key(label));}
 if(extension)touch(text_key("方案后缀"));
}
static void configure_screen(void){
 uint8_t data[64];for(unsigned i=0;i<64;i++)data[i]=(uint8_t)i;
 CHECK(tx_entry_update(1,0,0x18ff50e5,CANF_EXT|CANF_FD|CANF_BRS,64,data,0)==TX_OK);
 txseq_entry_config_t entries[TX_ENTRIES];memcpy(entries,txseq_get(1)->entries,sizeof entries);
 for(unsigned i=0;i<TX_ENTRIES;i++)entries[i].enabled=i==0;
 entries[0].repeat_count=3;entries[0].frame_gap_ms=7;entries[0].cycle_ms=40;
 CHECK(txseq_configure(1,entries,13,txseq_get(1)->revision)==0);
 CHECK(ui_tx_select(1,0)==0);
}
static sp_io_request_t save_screen(bool both){
 CHECK(ui_plan_open(both?2:1,1)==0);
 CHECK(ui_draft_open()&&strstr(json(),"\"page\":\"local_plan_name\""));
 CHECK(ui_review_navigate(PG_HOME)==UI_REVIEW_DRAFT);
 write_name(both?"Both":"Road",true);
 CHECK(strstr(json(),".canplan.json"));
 touch(text_key("保存"));
 CHECK(!ui_draft_open()&&ui_page()==PG_PROJECTFILES);
 sp_io_request_t request;CHECK(sp_io_peek(&request)&&request.kind==SP_IO_SAVE);
 CHECK(request.plan.channel_mask==(both?3u:2u));
 return request;
}
static void complete_save(const sp_io_request_t *request){
 sp_file_t file;memset(&file,0,sizeof file);snprintf(file.name,sizeof file.name,"%s",request->name);file.plan=request->plan;
 CHECK(sp_catalog_replace(&file,1)==0);
 CHECK(sp_io_complete(request->id,0,"发送方案已保存到SD文件")==0);
 CHECK(!sp_state()->io_pending&&sp_catalog_count()==1);
}
static void screen_save_load_and_preview(void){
 boot();configure_screen();uint32_t echo0=tx_get()->echo_count[0],echo1=tx_get()->echo_count[1];
 sp_io_request_t request=save_screen(false);
 CHECK(!strcmp(request.name,"Road.canplan.json"));
 CHECK(request.plan.channels[1].id_gap_ms==13);
 const send_plan_entry_t *entry=&request.plan.channels[1].entries[0];
 CHECK(entry->id==0x18ff50e5&&entry->length==64&&entry->flags==7);
 CHECK(entry->repeat_count==3&&entry->frame_gap_ms==7&&entry->cycle_ms==40&&entry->enabled);
 CHECK(!request.plan.channels[1].entries[1].enabled);
 for(unsigned i=0;i<64;i++)CHECK(entry->data[i]==i);
 CHECK(tx_get()->echo_count[0]==echo0&&tx_get()->echo_count[1]==echo1&&!tx_any_active());
 complete_save(&request);
 uint8_t changed[2]={0xaa,0xbb};CHECK(tx_entry_update(1,0,0x222,0,2,changed,99)==TX_OK);
 can_frame_t other=tx_get()->e[0][0].f;
 touch(0);CHECK(ui_page()==PG_TX&&strstr(json(),"\"tx_scope_mask\":2"));
 CHECK(tx_get()->e[1][0].f.id==0x18ff50e5&&tx_get()->e[1][0].f.len==64);
 CHECK(txseq_get(1)->entries[0].repeat_count==3&&txseq_get(1)->id_gap_ms==13);
 CHECK(!memcmp(&other,&tx_get()->e[0][0].f,sizeof other)&&!tx_any_active());
 CHECK(tx_get()->echo_count[0]==echo0&&tx_get()->echo_count[1]==echo1);
 /* File management keeps the read-only preview and explicit exact-round tools. */
 CHECK(ui_review_navigate(PG_FILES)==0);touch(port_sd_file_count());CHECK(ui_page()==PG_PLANDETAIL&&strstr(json(),"Road.canplan.json"));
 CHECK(strstr(json(),"仅加载到手动发送列表")&&strstr(json(),"加载并执行一轮"));
 touch(9);CHECK(ui_page()==PG_PLANENTRY);
 CHECK(strstr(json(),"DATA 38")&&strstr(json(),"38 39 3A 3B 3C 3D 3E 3F"));
 key(KEY_BACK);CHECK(ui_page()==PG_PLANDETAIL);
 touch(5);CHECK(ui_dialog_open()&&ui_dialog_ask()==ASK_PLAN_START);
 CHECK(strstr(json(),"执行一轮后自动停止")&&!strstr(json(),"开始循环。"));
 CHECK(tx_get()->echo_count[1]==echo1&&!tx_any_active());
 key(KEY_BACK);CHECK(!ui_dialog_open()&&tx_get()->echo_count[1]==echo1);
 touch(6);CHECK(ui_dialog_open()&&ui_dialog_ask()==ASK_PLAN_START);
 CHECK(strstr(json(),"开始循环。")&&!strstr(json(),"执行一轮后自动停止"));
 key(KEY_BACK);CHECK(!ui_dialog_open()&&tx_get()->echo_count[1]==echo1&&!tx_any_active());
 touch(5);CHECK(ui_dialog_open());post(EV_TOUCH_DLG,0,0,0);run(300);
 CHECK(!ui_dialog_open()&&txseq_get(1)->state==TXSEQ_IDLE&&txseq_get(1)->rounds==1);
 CHECK(tx_get()->echo_count[1]==echo1+3&&tx_get()->echo_count[0]==echo0);
 touch(6);if(ui_dialog_open())post(EV_TOUCH_DLG,0,0,0);run(180);
 CHECK(sp_state()->state==SP_RUNNING&&tx_get()->echo_count[1]>echo1+3);
 touch(7);run(2);CHECK(!tx_any_active());
 key(KEY_BACK);CHECK(ui_page()==PG_FILES);CHECK(ui_plan_open(0,0)==0&&strstr(json(),"\"page\":\"send_files\""));
 CHECK(!strstr(json(),"采集日志回放")&&ui_plan_open(3,0)==UI_REVIEW_RANGE);
}
static void snapshot_is_fixed_and_invalid_name_keeps_draft(void){
 boot();configure_screen();CHECK(ui_plan_open(1,1)==0);
 uint8_t changed[1]={0xaa};CHECK(tx_entry_update(1,0,0x111,0,1,changed,0)==TX_OK);
 write_name("bad",false);touch(text_key("保存"));
 CHECK(ui_draft_open()&&strstr(json(),"\"local_error\":true"));sp_io_request_t request;CHECK(!sp_io_peek(&request));
 touch(text_key("方案后缀"));touch(text_key("保存"));CHECK(sp_io_peek(&request));
 CHECK(request.plan.channels[1].entries[0].id==0x18ff50e5&&request.plan.channels[1].entries[0].length==64);
 complete_save(&request);
 CHECK(ui_review_navigate(PG_TX)==0);CHECK(ui_plan_open(1,1)==0);key(KEY_BACK);
 CHECK(ui_page()==PG_TX&&!ui_draft_open()&&!sp_io_peek(&request));
}
static void physical_save_entry_and_both_channels(void){
 boot();configure_screen();
 CHECK(ui_review_navigate(PG_PLAY)==0&&strstr(json(),"\"save_channel\":1"));
 CHECK(ui_review_navigate(PG_TX)==0);
 CHECK(ui_tx_select(1,3)==0);
 for(int i=0;i<4;i++)key(KEY_DOWN);
 CHECK(ui_cursor()==CH_NUM*TX_ENTRIES+3);key(KEY_OK);
 CHECK(ui_draft_open()&&strstr(json(),"保存当前屏发送方案"));key(KEY_BACK);
 CHECK(ui_page()==PG_TX);
 sp_io_request_t request=save_screen(true);CHECK(request.plan.channels[0].entries[0].id==0x123);
 CHECK(request.plan.channels[1].entries[0].id==0x18ff50e5);complete_save(&request);
 CHECK(ui_plan_open(0,0)==0);touch(1);CHECK(sp_io_peek(&request)&&request.kind==SP_IO_REFRESH);
 CHECK(sp_io_complete(request.id,SP_ERR_IO,"磁盘文件读取失败，旧目录保留")==SP_ERR_IO);
 CHECK(strstr(json(),"磁盘文件读取失败")&&sp_catalog_count()==1);
 CHECK(ui_plan_open(3,0)==UI_REVIEW_RANGE&&ui_page()==PG_PROJECTFILES);
 CHECK(ui_plan_open(-1,0)==UI_REVIEW_RANGE&&ui_plan_open(0,2)==UI_REVIEW_RANGE);
 post(EV_COMBO,0,COMBO_LOCK,0);CHECK(ui_plan_open(0,0)==UI_REVIEW_LOCKED);
}
int main(void){screen_save_load_and_preview();snapshot_is_fixed_and_invalid_name_keeps_draft();physical_save_entry_and_both_channels();printf("Send-plan local save / preview / restore / once / loop UI: %u checks passed\n",checks);return 0;}
