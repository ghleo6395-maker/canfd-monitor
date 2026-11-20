/* Real C services, file identity and device-local catalogue/keyboard. */
#include "sim/local_files_ui.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/tx_sched/tx_sm.h"
#include <stdio.h>
#include <string.h>
static int checks,failures;
#define CHECK(c) do{checks++;if(!(c)){failures++;printf("FAIL %d: %s\n",__LINE__,#c);}}while(0)
static char json[32768];
static const char *snap(void){jw_t j;jw_init(&j,json,sizeof json);jw_obj(&j);local_files_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return json;}
static void run(unsigned ms){while(ms){unsigned n=ms>10?10:ms;sim_tick((int)n);ms-=n;}}
static void boot(void){sim_reset();local_files_init();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(520);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(power_running()&&power_get()->screen_on&&storage_get()->state==ST_READY);}
static void request(ev_type_t type){evt_post4(type,0,SRC_UI,0,0);app_tick(0);}
static int key_index(const char *label){const char *keys=strstr(snap(),"\"keys\":[");if(!keys)return -1;char needle[128];snprintf(needle,sizeof needle,"\"label\":\"%s\"",label);for(const char *at=keys;(at=strstr(at,"{\"index\":"));at++){int index=-1;const char *end=strchr(at,'}'),*found=strstr(at,needle);if(found&&end&&found<end&&sscanf(at,"{\"index\":%d",&index)==1)return index;}return -1;}
static void tap(const char *label){int i=key_index(label);CHECK(i>=0);if(i>=0)local_files_touch(i);}
static void save_keys(void){int target=key_index("保存");CHECK(target>=0);for(int i=0;i<10&&local_files_cursor()>=10;i++)local_files_key(KEY_UP,GEST_SHORT);for(int i=0;i<10&&local_files_cursor();i++)local_files_key(KEY_LEFT,GEST_SHORT);CHECK(local_files_cursor()==0);for(int i=0;i<target/10;i++)local_files_key(KEY_DOWN,GEST_SHORT);for(int i=0;i<target%10;i++)local_files_key(KEY_RIGHT,GEST_SHORT);CHECK(local_files_cursor()==target);local_files_key(KEY_OK,GEST_SHORT);}
static void install(void){local_db_frame_t f[3]={{.id=0x123,.length=8,.cycle_ms=100,.name="VehicleStatus"},{.id=0x18FF50E5,.flags=CANF_EXT|CANF_FD|CANF_BRS,.length=64,.cycle_ms=20,.name="动力帧"},{.id=0x200,.length=4,.cycle_ms=500,.name="OtherStatus"}};CHECK(local_db_store("review.dbc",f,3)==0);CHECK(local_db_store("second.dbf",f,1)==1);}
static void catalogue(void){
 sim_reset();local_files_init();local_db_frame_t f={.id=0x123,.length=8,.cycle_ms=100,.name="Frame"};
 CHECK(local_db_store("preload.dbc",&f,1)==0);CHECK(local_files_open(2,0,0,0)==-1);
 boot();install();CHECK(local_db_count()==2);CHECK(ui_review_navigate(PG_TX)==0);CHECK(local_files_open(2,1,2,0)==0);
 CHECK(strstr(snap(),"\"page\":\"local_db\"")&&strstr(json,"CAN2"));CHECK(local_files_item_count()==4);
 local_files_key(KEY_DOWN,GEST_SHORT);local_files_key(KEY_LEFT,GEST_SHORT);CHECK(strstr(snap(),"第2条"));local_files_key(KEY_RIGHT,GEST_SHORT);
 local_files_touch(2);CHECK(strstr(snap(),"local_frames"));local_files_touch(1);CHECK(strstr(snap(),"local_frame")&&strstr(json,"动力帧")&&strstr(json,"64 字节"));
 const tx_entry_t old=tx_get()->e[1][2];uint32_t echo=tx_get()->echo_count[1];local_files_key(KEY_OK,GEST_SHORT);const tx_entry_t *e=&tx_get()->e[1][2];
 CHECK(e->f.id==0x18FF50E5&&e->f.len==64&&e->f.flags==(CANF_EXT|CANF_FD|CANF_BRS));CHECK(e->revision==old.revision+1&&e->period_ms==0&&!e->active);
 for(int i=0;i<64;i++)CHECK(e->f.data[i]==0);CHECK(e->sent==old.sent&&tx_get()->echo_count[1]==echo);CHECK(strstr(local_files_notice(),"未发送")&&!local_files_error());
 local_files_key(KEY_BACK,GEST_SHORT);CHECK(strstr(snap(),"local_frames"));local_files_key(KEY_BACK,GEST_SHORT);CHECK(strstr(snap(),"local_db"));local_files_key(KEY_BACK,GEST_SHORT);CHECK(!local_files_active());CHECK(ui_cursor()==TX_ENTRIES+2);
 CHECK(local_db_store("../bad.dbc",&f,1)==LOCAL_FILES_ERR_NAME);CHECK(local_db_store("table.csv",&f,1)==LOCAL_FILES_ERR_NAME);CHECK(local_db_store("big.dbc",&f,LOCAL_DB_MAX_FRAMES+1)==LOCAL_FILES_ERR_LIMIT);
 f.id=0x800;CHECK(local_db_store("bad.dbc",&f,1)==LOCAL_FILES_ERR_FRAME);f.id=0x123;f.flags=CANF_BRS;CHECK(local_db_store("bad.dbc",&f,1)==LOCAL_FILES_ERR_FRAME);f.flags=CANF_FD;f.length=9;CHECK(local_db_store("bad.dbc",&f,1)==LOCAL_FILES_ERR_FRAME);
 f.flags=0;f.length=8;memset(f.name,'a',sizeof f.name);CHECK(local_db_store("long.dbc",&f,1)==LOCAL_FILES_ERR_LIMIT);snprintf(f.name,sizeof f.name,"\xED\xA0\x80");CHECK(local_db_store("encoding.dbc",&f,1)==LOCAL_FILES_ERR_UTF8);CHECK(local_db_count()==2);
 snprintf(f.name,sizeof f.name,"Frame");CHECK(local_db_store("third.dbc",&f,1)==2);CHECK(local_db_store("fourth.dbc",&f,1)==3);CHECK(local_db_store("fifth.dbc",&f,1)==LOCAL_FILES_ERR_FULL);CHECK(local_db_count()==4);
 env_cmd("sd_remove",0,0);app_tick(0);CHECK(local_files_open(2,0,0,0)==LOCAL_FILES_ERR_SD);CHECK(local_db_store("absent.dbc",&f,1)==LOCAL_FILES_ERR_SD);
}
static void stale(void){
 boot();install();CHECK(local_files_open(2,0,0,0)==0);local_files_touch(2);local_files_touch(0);tx_entry_t old=tx_get()->e[0][0];
 local_db_frame_t f={.id=0x456,.length=1,.name="Replacement"};CHECK(local_db_store("REVIEW.DBC",&f,1)==0&&local_db_count()==2);
 CHECK(strstr(snap(),"数据库已更新")&&strstr(json,"VehicleStatus"));local_files_touch(4);CHECK(local_files_error());CHECK(tx_get()->e[0][0].revision==old.revision&&tx_get()->e[0][0].f.id==old.f.id);
 local_files_close();CHECK(local_files_open(2,0,0,0)==0);local_files_touch(2);local_files_touch(0);uint8_t bytes[1]={0xA5};CHECK(tx_entry_update(0,0,0x222,0,1,bytes,0)==TX_OK);uint32_t revision=tx_get()->e[0][0].revision;
 local_files_touch(4);CHECK(local_files_error());CHECK(tx_get()->e[0][0].f.id==0x222&&tx_get()->e[0][0].revision==revision&&tx_get()->e[0][0].f.data[0]==0xA5);
 app_evt_t fmt={.type=EV_SD_FORMAT_DONE,.a=SD_OK};local_files_handle_event(&fmt);CHECK(local_db_count()==0&&strstr(snap(),"local_db")&&strstr(json,"暂无DBC/DBF数据库"));
}
static void naming(void){
 boot();CHECK(local_files_open(3,0,0,0)==0&&local_files_draft());tap("路试");tap("A");CHECK(strstr(snap(),"\"value\":\"路试A\""));save_keys();CHECK(!local_files_active()&&!strcmp(env_rec_name_get(),"路试A"));
 request(EV_REQ_REC_START);run(200);CHECK(rec_get()->state==REC_RUNNING);file_info_t first,second;CHECK(port_sd_file_info(5,&first)&&strstr(first.name,"路试A_"));CHECK(local_files_open(3,0,0,0)==LOCAL_FILES_ERR_SD);
 request(EV_REQ_REC_STOP);run(200);CHECK(rec_get()->state==REC_IDLE);local_files_key(KEY_BACK,GEST_SHORT);request(EV_REQ_REC_START);run(200);CHECK(rec_get()->state==REC_RUNNING&&rec_get()->session==9);CHECK(port_sd_file_info(6,&second)&&strcmp(first.name,second.name)&&strstr(second.name,"路试A_"));request(EV_REQ_REC_STOP);run(200);CHECK(rec_get()->state==REC_IDLE);local_files_key(KEY_BACK,GEST_SHORT);
 CHECK(local_files_open(3,0,0,0)==0);tap("清空");tap("台架");local_files_key(KEY_BACK,GEST_SHORT);CHECK(!strcmp(env_rec_name_get(),"路试A"));
 CHECK(local_files_open(3,0,0,0)==0);tap("清空");tap("路试");tap("退格");CHECK(strstr(snap(),"\"value\":\"路\""));save_keys();CHECK(!strcmp(env_rec_name_get(),"路"));
 CHECK(local_files_open(3,0,0,0)==0);tap("清空");tap("测试");tap("测试");tap("测试");tap("测试");CHECK(local_files_error()&&strstr(snap(),"\"value\":\"测试测试测试\""));save_keys();CHECK(strlen(env_rec_name_get())==18);
 CHECK(local_files_open(3,0,0,0)==0);tap("清空");save_keys();CHECK(!strcmp(env_rec_name_get(),""));
}
static void rename_files(void){
 boot();const char trace[]="(0.000000) can0 123#AA\n";env_log_result_t result;int index=env_log_import("sample.log",trace,sizeof trace-1,&result);CHECK(index==5);uint32_t token=env_file_token(index);
 CHECK(local_files_open(5,0,0,0)==0&&strstr(snap(),"sample.log"));local_files_key(KEY_OK,GEST_SHORT);CHECK(local_files_draft()&&strstr(snap(),"local_rename"));
 tap("清空");tap("路试");tap("A");tap(".log");save_keys();file_info_t file;CHECK(port_sd_file_info(index,&file)&&!strcmp(file.name,"路试A.log"));CHECK(env_file_token(index)==token);
 CHECK(local_files_open(4,0,index,token+1)==LOCAL_FILES_ERR_CHANGED);CHECK(local_files_open(4,0,index,token)==0);tap("清空");tap("A");save_keys();CHECK(local_files_draft()&&local_files_error());CHECK(port_sd_file_info(index,&file)&&!strcmp(file.name,"路试A.log"));local_files_key(KEY_BACK,GEST_SHORT);CHECK(!local_files_active());
 CHECK(local_files_open(5,0,0,0)==0);port_sd_delete_oldest();local_files_key(KEY_OK,GEST_SHORT);CHECK(local_files_active()&&!local_files_draft()&&local_files_error());CHECK(env_file_token(index-1)==token);CHECK(port_sd_file_info(index-1,&file)&&!strcmp(file.name,"路试A.log"));
 local_files_close();port_sd_protect_toggle(index-1);CHECK(local_files_open(5,0,0,0)==0);local_files_touch(0);CHECK(!local_files_draft()&&local_files_error());local_files_close();CHECK(local_files_open(4,0,index-1,token)==ENV_LOG_ERR_READONLY);
}
static void sd_lifecycle(void){
 sim_reset();local_files_init();local_db_frame_t frame={.id=0x123,.length=8,.cycle_ms=100,.name="PowerPersistent"};
 CHECK(local_db_store("persistent.dbc",&frame,1)==0);CHECK(local_db_count()==1);
 sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(520);
 CHECK(power_running()&&local_db_count()==1);CHECK(local_files_open(2,0,0,0)==0);CHECK(strstr(snap(),"persistent.dbc"));
 local_files_close();ui_init();CHECK(local_db_count()==1);
 sim_key(KEY_POWER,1);run(2200);sim_key(KEY_POWER,0);run(400);CHECK(power_get()->state==PWR_OFF);CHECK(local_db_count()==1);
 sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(520);CHECK(power_running()&&local_db_count()==1);
 CHECK(local_files_open(2,0,0,0)==0);local_files_touch(2);local_files_touch(0);CHECK(strstr(snap(),"PowerPersistent"));local_files_close();
 evt_post4(EV_REQ_FORMAT,0,SRC_UI,0,1);app_tick(0);CHECK(storage_get()->state==ST_FORMATTING);run(600);
 CHECK(storage_get()->state==ST_READY&&local_db_count()==0);
 CHECK(local_db_store("new.dbc",&frame,1)==0);sim_reset();CHECK(local_db_count()==0);
}
int main(void){catalogue();stale();naming();rename_files();sd_lifecycle();printf("Device-local SD databases / keyboard / names: %d checks, %d failures\n",checks,failures);return failures?1:0;}
