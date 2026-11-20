/* UTF-8 backspace repeat belongs to the keyboard backend, not browser text. */
#include "sim/local_files_ui.h"
#include "sim/sim_api.h"
#include "sim/env_sim.h"
#include "app/app.h"
#include "services/core/app_evt.h"
#include "services/core/jsonw.h"
#include "services/power/power_sm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,saves,cancels;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define BACKSPACE ((int)sizeof "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-. "-1+3)
#define SAVE (BACKSPACE+2)
static char json[30000],value_buf[256];
static const char *save(const char *value,void *context){(void)value;(void)context;saves++;return NULL;}
static void cancel(void *context){(void)context;cancels++;}
static const char *state(void){jw_t j;jw_init(&j,json,sizeof json);jw_obj(&j);local_files_json(&j);jw_end_obj(&j);jw_finish(&j);CHECK(!j.overflow);return json;}
static const char *value(void){const char *s=strstr(state(),"\"keyboard\":");CHECK(s!=NULL);s=strstr(s,"\"value\":\"");CHECK(s!=NULL);s+=9;const char *end=strchr(s,'"');CHECK(end!=NULL&&(size_t)(end-s)<sizeof value_buf);memcpy(value_buf,s,(size_t)(end-s));value_buf[end-s]=0;return value_buf;}
static void run(unsigned ms){for(unsigned i=0;i<ms;i++){sim_tick(1);app_tick(0);local_files_tick();}}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);for(int i=0;i<170;i++)sim_tick(10);sim_key(KEY_POWER,0);for(int i=0;i<5;i++)sim_tick(10);CHECK(power_running());env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);saves=cancels=0;}
static uint32_t open_name(const char *initial){CHECK(local_text_open("名称",initial,47,save,cancel,NULL)==0);uint32_t token=local_files_keyboard_token();CHECK(token!=0&&strstr(state(),"\"backspace_delay_ms\":400")&&strstr(state(),"\"backspace_period_ms\":90"));return token;}
static void precise_delay_utf8_single_and_release(void){
 boot();uint32_t token=open_name("ABCDE路试");local_files_touch(BACKSPACE);CHECK(!strcmp(value(),"ABCDE路"));CHECK(local_files_backspace_begin(token)==0);CHECK(!strcmp(value(),"ABCDE"));run(399);CHECK(!strcmp(value(),"ABCDE"));run(1);CHECK(!strcmp(value(),"ABCD"));run(89);CHECK(!strcmp(value(),"ABCD"));run(1);CHECK(!strcmp(value(),"ABC"));CHECK(local_files_backspace_end(token)==0);run(1000);CHECK(!strcmp(value(),"ABC"));CHECK(local_files_backspace_begin(token)==0);CHECK(local_files_backspace_begin(token)==0);CHECK(!strcmp(value(),"AB"));run(400);CHECK(!strcmp(value(),"A"));CHECK(local_files_backspace_end(token)==0);local_files_close();
}
static void stale_tokens_cannot_end_new_draft_hold(void){
 boot();uint32_t old=open_name("ABCDEFGHIJ");CHECK(local_files_backspace_begin(old)==0);local_files_close();uint32_t token=open_name("12345");CHECK(token!=old);CHECK(local_files_backspace_begin(token)==0);CHECK(!strcmp(value(),"1234"));CHECK(local_files_backspace_end(old)==LOCAL_FILES_ERR_CHANGED);CHECK(local_files_backspace_begin(old)==LOCAL_FILES_ERR_CHANGED);run(400);CHECK(!strcmp(value(),"123"));CHECK(local_files_backspace_end(token)==0);run(500);CHECK(!strcmp(value(),"123"));local_files_close();sim_reset();sim_key(KEY_POWER,1);for(int i=0;i<170;i++)sim_tick(10);sim_key(KEY_POWER,0);for(int i=0;i<5;i++)sim_tick(10);uint32_t fresh=open_name("New");CHECK(fresh!=old&&fresh!=token);CHECK(local_files_backspace_begin(token)==LOCAL_FILES_ERR_CHANGED);local_files_close();
}
static void other_keys_save_cancel_and_empty_stop(void){
 boot();uint32_t token=open_name("ABCDEFG");CHECK(local_files_backspace_begin(token)==0);local_files_touch(0);CHECK(!strcmp(value(),"ABCDEFA"));run(1000);CHECK(!strcmp(value(),"ABCDEFA"));CHECK(local_files_backspace_begin(token)==0);local_files_touch(SAVE);CHECK(!local_files_active()&&saves==1);run(1000);CHECK(saves==1);token=open_name("Discard");CHECK(local_files_backspace_begin(token)==0);local_files_key(KEY_BACK,GEST_SHORT);CHECK(!local_files_active()&&cancels==1);run(1000);CHECK(saves==1&&cancels==1);token=open_name("");CHECK(local_files_backspace_begin(token)==0);run(1000);CHECK(!strcmp(value(),""));CHECK(local_files_backspace_end(token)==0);app_evt_t event={.type=EV_SYS_STOP_ALL};local_files_handle_event(&event);CHECK(!local_files_active()&&local_files_keyboard_token()==0);CHECK(local_files_backspace_end(token)==LOCAL_FILES_ERR_CHANGED);
}
static void physical_long_ok_stops_on_release_and_other_key(void){
 boot();(void)open_name("ABCDEFGHIJKLMNOP");for(int i=0;i<BACKSPACE;i++)local_files_key(KEY_RIGHT,GEST_SHORT);CHECK(local_files_cursor()==BACKSPACE);sim_key(KEY_OK,1);run(999);CHECK(strlen(value())==16);run(1);CHECK(strlen(value())==15);run(89);CHECK(strlen(value())==15);run(1);CHECK(strlen(value())==14);sim_key(KEY_OK,0);run(200);CHECK(strlen(value())==14);sim_key(KEY_OK,1);run(1000);CHECK(strlen(value())==13);sim_key(KEY_RIGHT,1);run(500);CHECK(strlen(value())==13);sim_key(KEY_RIGHT,0);sim_key(KEY_OK,0);run(200);CHECK(strlen(value())==13);local_files_close();
 (void)open_name("DoNotSaveOnHold");for(int i=0;i<SAVE;i++)local_files_key(KEY_RIGHT,GEST_SHORT);sim_key(KEY_OK,1);run(1200);sim_key(KEY_OK,0);run(1000);CHECK(local_files_active()&&saves==0);sim_key(KEY_OK,1);run(10);sim_key(KEY_OK,0);CHECK(!local_files_active()&&saves==1);
}
static void sd_loss_stops_repeat_without_saving(void){
 boot();uint32_t token=open_name("ABCDEFGH");CHECK(local_files_backspace_begin(token)==0);env_cmd("sd_remove",0,0);app_tick(0);run(1000);CHECK(local_files_active()&&!strcmp(value(),"ABCDEFG"));CHECK(saves==0);CHECK(local_files_backspace_begin(token)==LOCAL_FILES_ERR_SD);local_files_close();
}
static void browser_lease_expires_without_release_and_renew_keeps_timing(void){
 boot();uint32_t token=open_name("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstu");CHECK(strstr(state(),"\"backspace_lease_ms\":1500")!=NULL);CHECK(local_files_backspace_begin(token)==0);CHECK(strlen(value())==46);run(200);CHECK(local_files_backspace_begin(token)==0);run(199);CHECK(strlen(value())==46);run(1);CHECK(strlen(value())==45);run(90);CHECK(strlen(value())==44);run(1209);CHECK(strstr(state(),"\"backspace_holding\":true")!=NULL);run(1);CHECK(strstr(state(),"\"backspace_holding\":false")!=NULL);size_t stopped=strlen(value());run(1000);CHECK(strlen(value())==stopped);local_files_close();
 token=open_name("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstu");CHECK(local_files_backspace_begin(token)==0);for(int i=0;i<10;i++){run(250);CHECK(local_files_backspace_begin(token)==0);}CHECK(strstr(state(),"\"backspace_holding\":true")!=NULL);CHECK(strlen(value())<33&&strlen(value())>10);CHECK(local_files_backspace_end(token)==0);stopped=strlen(value());run(1600);CHECK(strlen(value())==stopped);local_files_close();
 uint32_t old=token;token=open_name("FreshDraft");CHECK(local_files_backspace_begin(token)==0);CHECK(local_files_backspace_end(old)==LOCAL_FILES_ERR_CHANGED);CHECK(local_files_backspace_begin(old)==LOCAL_FILES_ERR_CHANGED);run(1500);CHECK(strstr(state(),"\"backspace_holding\":false")!=NULL);CHECK(local_files_backspace_end(old)==LOCAL_FILES_ERR_CHANGED);CHECK(local_files_keyboard_token()==token);local_files_close();
}
static void physical_hold_has_no_browser_lease(void){
 boot();(void)open_name("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstu");for(int i=0;i<BACKSPACE;i++)local_files_key(KEY_RIGHT,GEST_SHORT);sim_key(KEY_OK,1);run(1000);size_t before=strlen(value());run(1800);CHECK(strstr(state(),"\"backspace_holding\":true")!=NULL);CHECK(strlen(value())<before-15);sim_key(KEY_OK,0);run(100);CHECK(strstr(state(),"\"backspace_holding\":false")!=NULL);local_files_close();
}
int main(void){precise_delay_utf8_single_and_release();stale_tokens_cannot_end_new_draft_hold();other_keys_save_cancel_and_empty_stop();physical_long_ok_stops_on_release_and_other_key();sd_loss_stops_repeat_without_saving();browser_lease_expires_without_release_and_renew_keeps_timing();physical_hold_has_no_browser_lease();printf("Keyboard token / UTF-8 backspace hold: %u checks passed\n",checks);return 0;}
