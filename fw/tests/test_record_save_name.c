/* Saving a name never controls persistence of an already closed recording. */
#include "sim/local_files_ui.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include "app/app.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/frame_ring/frame_ring.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static const char characters[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-. ";
#define CHARS ((int)(sizeof characters-1u))
#define CLEAR (CHARS+4)
#define SAVE (CHARS+5)
#define KEEP (CHARS+6)
#define BACKSPACE (CHARS+3)
static char original[ENV_LOG_MAX_TEXT],after[ENV_LOG_MAX_TEXT],json[30000];
static void run(unsigned ms){for(unsigned i=0;i<ms;i++){sim_tick(1);app_tick(0);}}
static void post(ev_type_t type,int ch,int a,int b){CHECK(evt_post4(type,(uint8_t)ch,SRC_UI,a,b));app_tick(0);}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);for(int i=0;i<170;i++)sim_tick(10);sim_key(KEY_POWER,0);for(int i=0;i<5;i++)sim_tick(10);CHECK(power_running());env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);}
static int file_index(uint32_t token){for(int i=0;i<port_sd_file_count();i++)if(env_file_token(i)==token)return i;return -1;}
static file_info_t info(uint32_t token){file_info_t out;CHECK(port_sd_file_info(file_index(token),&out));return out;}
static void set_text(const char *text){local_files_touch(CLEAR);for(const char *p=text;*p;p++){const char *at=strchr(characters,*p);CHECK(at!=NULL);local_files_touch((int)(at-characters));}}
static const char *snapshot(void){jw_t j;jw_init(&j,json,sizeof json);jw_obj(&j);local_files_json(&j);jw_end_obj(&j);jw_finish(&j);CHECK(!j.overflow);return json;}
static uint32_t recording(uint32_t *session){
 post(EV_REQ_REC_START,0,0,0);run(130);CHECK(rec_get()->state==REC_RUNNING);*session=rec_get()->session;uint32_t token=env_record_file_token(*session);CHECK(token!=0&&info(token).writing);CHECK(local_record_save_open(*session,token)==LOCAL_FILES_ERR_SD);
 can_frame_t f={.ts_us=(uint64_t)app_now_ms()*1000,.id=0x123,.len=2,.data={0xAA,0xBB}};CHECK(ring_push(&f));f.ch=1;f.ts_us+=100;f.id=0x456;f.data[0]=0xCC;CHECK(ring_push(&f));app_tick(0);
 CHECK(evt_post4(EV_REQ_REC_STOP,0,SRC_HOST,0,1));app_tick(0);CHECK(rec_get()->state==REC_STOPPING);CHECK(local_record_save_open(*session,token)==LOCAL_FILES_ERR_SD);run(170);CHECK(rec_get()->state==REC_IDLE&&rec_get()->last_result==MSG_REC_STOPPED);if(local_files_active())local_files_close();CHECK(!info(token).writing&&info(token).frames==2);return token;
}
static void default_name_and_cancel_preserve_saved_content(void){
 boot();uint32_t session,token=recording(&session);file_info_t before=info(token);size_t needed;int size=env_log_export(file_index(token),original,sizeof original,&needed);CHECK(size>0);CHECK(local_record_save_open(session,token)==0);const char *s=snapshot();CHECK(strstr(s,"\"page\":\"local_record_save\"")&&strstr(s,"保存录制文件")&&strstr(s,"保留默认名")&&strstr(s,"\"record_token\""));CHECK(strstr(s,".log")&&strstr(s,".txt")&&!strstr(s,"canplan"));CHECK(local_files_draft());local_files_touch(KEEP);CHECK(!local_files_active()&&!strcmp(info(token).name,before.name));CHECK(env_log_export(file_index(token),after,sizeof after,&needed)==size&&!memcmp(original,after,(size_t)size));
 CHECK(local_record_save_open(session,token)==0);set_text("Discarded");local_files_key(KEY_BACK,GEST_SHORT);CHECK(!local_files_active()&&!strcmp(info(token).name,before.name));CHECK(local_record_save_open(session,token)==0);local_files_touch(SAVE);CHECK(!local_files_active()&&!strcmp(info(token).name,before.name));
}
static void base_extension_validation_and_no_overwrite(void){
 boot();uint32_t session,token=recording(&session);file_info_t before=info(token);const char *line="(1.000000) can0 123#AA\n";env_log_result_t imported;int other=env_log_import("Taken.log",line,strlen(line),&imported);CHECK(other>=0);CHECK(env_record_file_token(0)==0);CHECK(local_record_save_open(session,env_file_token(other))==LOCAL_FILES_ERR_CHANGED);CHECK(local_record_save_open(session,token)==0);
 for(unsigned i=0;i<3;i++){set_text(i==0?"Taken":i==1?"CON":"Road.bin");local_files_touch(SAVE);CHECK(local_files_active()&&local_files_error()&&!strcmp(info(token).name,before.name));}
 set_text("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");local_files_touch(SAVE);CHECK(local_files_active()&&local_files_error());local_files_touch(BACKSPACE);local_files_touch(SAVE);CHECK(!local_files_active()&&strlen(info(token).name)==47&&strstr(info(token).name,".log"));
 CHECK(local_record_save_open(session,token)==0);set_text("Road");local_files_touch(SAVE);CHECK(!local_files_active()&&!strcmp(info(token).name,"Road.log"));CHECK(local_record_save_open(session,token)==0);set_text("Road.txt");local_files_touch(SAVE);CHECK(!local_files_active()&&!strcmp(info(token).name,"Road.txt"));file_info_t untouched;CHECK(port_sd_file_info(other,&untouched)&&!strcmp(untouched.name,"Taken.log"));
 CHECK(local_record_save_open(session,token)==0);set_text("");for(int i=0;i<7;i++)local_files_touch(CHARS);local_files_touch(0);local_files_touch(SAVE);CHECK(!local_files_active()&&strlen(info(token).name)==47&&strstr(info(token).name,"路试"));
}
static void session_token_pair_and_compaction_are_bound(void){
 boot();uint32_t old_session,current_session,old=recording(&old_session),current=recording(&current_session);CHECK(old_session!=current_session&&old!=current);CHECK(local_record_save_open(current_session,old)==LOCAL_FILES_ERR_CHANGED);CHECK(local_record_save_open(old_session,old)==LOCAL_FILES_ERR_CHANGED);CHECK(local_record_save_open(current_session,current)==0);int was=file_index(current);port_sd_delete_oldest();app_tick(0);CHECK(file_index(current)==was-1&&env_record_file_token(current_session)==current);set_text("Moved");local_files_touch(SAVE);CHECK(!local_files_active()&&!strcmp(info(current).name,"Moved.log"));CHECK(strcmp(info(old).name,"Moved.log")!=0);
 CHECK(local_record_save_open(current_session,current)==0);post(EV_SYS_STOP_ALL,0,0,0);CHECK(!local_files_active()&&!strcmp(info(current).name,"Moved.log"));port_sd_format();run(520);CHECK(env_record_file_token(current_session)==0&&env_record_file_token(old_session)==0);CHECK(local_record_save_open(current_session,current)==LOCAL_FILES_ERR_CHANGED);
}
int main(void){default_name_and_cancel_preserve_saved_content();base_extension_validation_and_no_overwrite();session_token_pair_and_compaction_are_bound();printf("Closed recording naming / token binding: %u checks passed\n",checks);return 0;}
