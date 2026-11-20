/* Target preprocessor path on simulated ports: no MCU or real SD claim. */
#ifndef CANFD_REVD_TARGET
#error This regression must compile the device UI branch.
#endif
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/device/file_port.h"
#include "services/frame_ring/frame_ring.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "sim/env_sim.h"
#include "sim/local_files_ui.h"
#include "sim/send_plan.h"
#include "sim/sim_api.h"
#include "sim/tx_scope.h"
#include "sim/tx_sequence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void run(unsigned ms){for(unsigned i=0;i<ms;i++){app_tick(1);env_tick(1);}}
static void post(ev_type_t type,int ch,int a,int b){CHECK(evt_post4(type,(uint8_t)ch,SRC_UI,a,b));app_tick(0);}
static const char *json(void){static char out[32768];jw_t j;jw_init(&j,out,sizeof out);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return out;}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(power_running());}
static void normal(int ch){post(EV_REQ_CH_MODE,ch,CAN_MODE_NORMAL,2);CHECK(chan_can_tx(ch));}
static void timing(int ch){txseq_entry_config_t entries[TX_ENTRIES];memcpy(entries,txseq_get(ch)->entries,sizeof entries);for(int i=0;i<TX_ENTRIES;i++)entries[i].enabled=i==0;entries[0].repeat_count=2;entries[0].frame_gap_ms=5;entries[0].cycle_ms=40;CHECK(txseq_configure(ch,entries,3,txseq_get(ch)->revision)==0);}
static sp_file_t install(void){static sp_file_t file;memset(&file,0,sizeof file);strcpy(file.name,"device.canplan.json");CHECK(sp_capture(3,&file.plan)==0);CHECK(sp_catalog_replace(&file,1)==0&&sp_catalog_get(0,&file));return file;}
static void target_home_keyboard_and_database_boundary(void){
 boot();CHECK(strstr(json(),"\"home_columns\":3"));CHECK(ui_review_navigate(PG_TX)==0);
 CHECK(strstr(json(),"\"tx_load_file_index\":15")&&strstr(json(),"\"tx_channels\":["));
 CHECK(!strstr(json(),"从本地 SD 数据库选帧")&&local_db_count()==0);
 local_db_frame_t row={.id=0x123,.length=1,.name="Demo"};CHECK(local_db_store("demo.dbc",&row,1)==LOCAL_FILES_ERR_UNSUPPORTED);
 CHECK(ui_local_open(2,0,0)==LOCAL_FILES_ERR_UNSUPPORTED&&!ui_draft_open());
 CHECK(ui_tx_edit_open(0,0));post(EV_TOUCH_ITEM,0,0,0);CHECK(strstr(json(),"\"page\":\"local_input\"")&&strstr(json(),"\"mode\":\"hex\""));
 CHECK(local_files_keyboard_replace(local_files_keyboard_token(),"456")==0);/* hex keyboard: 16 characters + backspace, clear, confirm. */
 local_files_touch(18);CHECK(!local_files_active()&&strstr(json(),"\"dirty\":true"));
 CHECK(tx_get()->e[0][0].f.id==0x123&&tx_get()->pending[0]==0);post(EV_TOUCH_ITEM,0,10,0);
 CHECK(tx_get()->e[0][0].f.id==0x456&&tx_get()->pending[0]==0&&ui_page()==PG_TX);
}
static void app_owns_scheduling_and_independent_switches(void){
 boot();CHECK(ui_review_navigate(PG_TX)==0);normal(0);normal(1);timing(0);timing(1);
 CHECK(ui_tx_channel_send(0,true)==0&&ui_tx_channel_send(1,true)==0);run(12);
 CHECK(txseq_get(0)->sent==2&&txseq_get(1)->sent==2);uint32_t sent=txseq_get(0)->sent;
 app_tick(0);app_tick(0);CHECK(txseq_get(0)->sent==sent);CHECK(tx_any_active());
 CHECK(tx_entry_update(0,0,0x321,0,1,(uint8_t[]){1},0)==TX_ERR_ACTIVE);
 CHECK(ui_tx_channel_send(0,false)==0);app_tick(0);uint32_t other=txseq_get(1)->sent;run(90);
 CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(0)->sent==sent&&txseq_get(1)->sent>other);
 CHECK(ui_tx_single(1,0)==TXSEQ_ERR_ACTIVE);CHECK(ui_tx_channel_send(1,false)==0);run(3);
 CHECK(ui_tx_target_select(3)==0);uint32_t echo0=tx_get()->echo_count[0],echo1=tx_get()->echo_count[1];
 CHECK(ui_tx_single(1,0)==0);run(5);CHECK(tx_get()->echo_count[0]==echo0&&tx_get()->echo_count[1]==echo1+1);
 CHECK(strstr(json(),"\"tx_scope_mask\":3"));
}
static void plan_load_rounds_and_soft_boot_lifecycle(void){
 boot();normal(0);normal(1);timing(0);timing(1);sp_file_t file=install();CHECK(sp_load(0,file.token)==0);
 run(10);CHECK(!tx_any_active()&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
 CHECK(sp_start_once()==0);run(90);CHECK(txseq_get(0)->sent==2&&txseq_get(1)->sent==2&&txseq_get(0)->rounds==1&&txseq_get(1)->rounds==1);
 CHECK(!tx_any_active()&&sp_state()->state==SP_LOADED);uint32_t token=file.token;
 post(EV_REQ_SHUTDOWN,0,0,0);run(400);CHECK(power_get()->state==PWR_OFF);
 sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);CHECK(power_running());
 CHECK(sp_catalog_count()==1&&sp_catalog_get(0,&file)&&file.token==token);CHECK(sp_state()->state==SP_IDLE&&sp_state()->loaded_mask==0);
 CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(1)->state==TXSEQ_IDLE&&scope_state()->state==SCOPE_IDLE);
 CHECK(txseq_get(0)->entries[0].repeat_count==1);CHECK(sp_load(0,file.token)==0&&!tx_any_active());
}
static void stopped_recording_uses_device_file_port(void){
 boot();post(EV_KEY,KEY_REC,GEST_SHORT,0);run(200);CHECK(rec_get()->state==REC_RUNNING);
 uint32_t session=rec_get()->session;can_frame_t frame={.id=0x321,.len=1,.data={0xAA}};CHECK(ring_push(&frame));app_capture_poll();
 post(EV_KEY,KEY_REC,GEST_SHORT,0);run(200);CHECK(rec_get()->state==REC_IDLE&&rec_get()->markers==0);
 CHECK(strstr(json(),"\"page\":\"local_record_save\"")&&file_port_record_token(session)!=0);
 uint32_t token=file_port_record_token(session);post(EV_KEY,KEY_BACK,GEST_SHORT,0);CHECK(!ui_draft_open()&&file_port_record_token(session)==token);
 CHECK(rec_get()->frames==1);CHECK(ui_review_navigate(PG_TX)==0);
}
/* Model the native driver's foreground completion bridge without dispatching
 * the rest of the application queue while an SD call owns the foreground. */
static void complete_transport_only(void){
 env_tick(1);app_evt_t kept[APP_EVT_QUEUE_LEN],event;unsigned n=0;
 while(evt_pop(&event)){if(event.type==EV_CAN_TX_DONE)app_tx_complete(event.ch);else{CHECK(n<APP_EVT_QUEUE_LEN);kept[n++]=event;}}
 for(unsigned i=0;i<n;i++)CHECK(evt_post(&kept[i]));
}
static void sd_wait_keeps_existing_tx_but_never_dispatches_commands(void){
 boot();CHECK(ui_review_navigate(PG_TX)==0);normal(0);
 txseq_entry_config_t entries[TX_ENTRIES];memcpy(entries,txseq_get(0)->entries,sizeof entries);for(int i=0;i<TX_ENTRIES;i++)entries[i].enabled=i==0;
 entries[0].repeat_count=20;entries[0].frame_gap_ms=5;entries[0].cycle_ms=120;CHECK(txseq_configure(0,entries,0,txseq_get(0)->revision)==0);
 CHECK(ui_tx_channel_send(0,true)==0);uint32_t started=app_now_ms();
 CHECK(evt_post4(EV_REQ_REC_START,0,SRC_UI,0,0));
 for(unsigned i=0;i<110;i++){complete_transport_only();app_realtime_poll(1);CHECK(rec_get()->state==REC_IDLE);}
 complete_transport_only();CHECK(app_now_ms()==started+110&&txseq_get(0)->sent==20&&txseq_get(0)->missed==0);
 CHECK(tx_get()->echo_count[0]==20&&tx_get()->pending[0]==0);CHECK(ui_page()==PG_TX&&!ui_dialog_open());
 app_tick(0);CHECK(rec_get()->state==REC_PREPARING);CHECK(ui_tx_channel_send(0,false)==0);
 boot();CHECK(scope_start(1)==TXSEQ_WAIT_CONFIRM);app_tick(0);sim_touch(2,0);CHECK(scope_state()->state==SCOPE_ARMING);
 uint32_t request=scope_state()->request;
 for(unsigned i=0;i<12;i++)app_realtime_poll(1);
 CHECK(scope_state()->state==SCOPE_ARMING&&scope_state()->request==request&&!txseq_any_active()&&tx_get()->pending[0]==0);
}
static void sd_wait_consumes_only_tx_dwell_timeout(void){
 boot();CHECK(ui_review_navigate(PG_TX)==0);normal(0);
 txseq_entry_config_t entries[TX_ENTRIES];memcpy(entries,txseq_get(0)->entries,sizeof entries);for(int i=0;i<TX_ENTRIES;i++)entries[i].enabled=i==0;
 entries[0].repeat_count=30;entries[0].frame_gap_ms=5;entries[0].cycle_ms=160;CHECK(txseq_configure(0,entries,0,txseq_get(0)->revision)==0);
 CHECK(ui_tx_channel_send(0,true)==0);tmr_start(TMR_REC_GUARD,5);
 /* No transport completion: the first accepted frame starts its 100ms dwell. */
 app_realtime_poll(1);CHECK(tx_get()->pending[0]==1);
 for(unsigned i=0;i<99;i++)app_realtime_poll(1);
 CHECK(tx_get()->noack_count[0]==0&&tx_get()->pending[0]==TX_FIFO_LEN&&tmr_active(TMR_REC_GUARD));
 app_realtime_poll(1);CHECK(tx_get()->noack_count[0]==1&&tx_get()->pending[0]==0);
 CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(0)->stop_reason==TXSEQ_STOP_NOACK);
 CHECK(tmr_active(TMR_REC_GUARD)&&!tmr_active(TMR_TX_DWELL0)&&rec_get()->state==REC_IDLE);
 CHECK(tx_get()->echo_count[0]==0&&!ui_dialog_open()&&ui_page()==PG_TX);
 uint32_t sent=txseq_get(0)->sent;app_realtime_poll(100);CHECK(tx_get()->noack_count[0]==1&&txseq_get(0)->sent==sent);
 CHECK(!tmr_take_expired(TMR_NUM));
}
static const char *accept_text(const char *value,void *context){(void)value;(void)context;return NULL;}
static void elapsed_consumed_in_sd_still_services_foreground_owners(void){
 boot();CHECK(ui_review_navigate(PG_TX)==0);CHECK(scope_start(1)==TXSEQ_WAIT_CONFIRM);app_tick(0);sim_touch(2,0);
 CHECK(scope_state()->state==SCOPE_ARMING);app_tick(0);CHECK(scope_state()->state==SCOPE_ARMING&&txseq_get(0)->state==TXSEQ_IDLE);
 app_realtime_poll(1);CHECK(scope_state()->state==SCOPE_ARMING);app_tick(0);
 CHECK(scope_state()->state==SCOPE_IDLE&&txseq_get(0)->state==TXSEQ_RUNNING&&txseq_get(0)->sent==1);scope_stop(1);app_tick(0);
 boot();normal(0);normal(1);timing(0);timing(1);sp_file_t file=install();CHECK(sp_load(0,file.token)==0&&sp_start_once()==0);
 for(unsigned i=0;i<12;i++){complete_transport_only();app_realtime_poll(1);app_tick(0);}
 CHECK(txseq_get(0)->rounds==1&&txseq_get(1)->rounds==1&&sp_state()->state==SP_LOADED);
 CHECK(local_text_open("Test hold","ABCD",47,accept_text,NULL,NULL)==0);CHECK(local_files_backspace_begin(local_files_keyboard_token())==0);
 CHECK(strstr(json(),"\"value\":\"ABC\""));app_realtime_poll(400);app_tick(0);CHECK(strstr(json(),"\"value\":\"AB\""));local_files_close();
}
static void queued_old_timeout_cannot_abort_new_frame(void){
 boot();normal(0);CHECK(tx_entry_submit(0,0,TXSRC_MANUAL));app_clock_advance(TX_DWELL_MS);tmr_tick();
 app_evt_t old;CHECK(evt_pop(&old)&&old.type==EV_TIMER&&old.a==TMR_TX_DWELL0);app_tx_complete(0);
 CHECK(tx_entry_submit(0,0,TXSRC_MANUAL));tx_handle(&old);CHECK(tx_get()->pending[0]==1&&tx_get()->noack_count[0]==0);
 app_realtime_poll(TX_DWELL_MS-1);CHECK(tx_get()->pending[0]==1&&tx_get()->noack_count[0]==0);
 app_realtime_poll(1);CHECK(tx_get()->pending[0]==0&&tx_get()->noack_count[0]==1);
}
static void storage_error_survives_software_boot(void){
 boot();CHECK(sp_request_refresh()==0);sp_io_request_t request;CHECK(sp_io_peek(&request));CHECK(sp_io_complete(request.id,SP_ERR_IO,"SD directory unavailable")==SP_ERR_IO);
 post(EV_REQ_SHUTDOWN,0,0,0);run(400);CHECK(power_get()->state==PWR_OFF);sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);
 CHECK(power_running()&&sp_state()->state==SP_IDLE&&sp_state()->last_error==SP_ERR_IO&&!strcmp(sp_state()->message,"SD directory unavailable"));
}
static void displayed_plan_identity_rejects_replaced_row(void){
 boot();CHECK(ui_review_navigate(PG_TX)==0);sp_file_t file=install();CHECK(ui_plan_open(0,0)==0);
 const uint32_t old=file.token;file.plan.channels[0].entries[0].data[0]^=1;CHECK(sp_catalog_replace(&file,1)==0&&sp_catalog_get(0,&file)&&file.token!=old);
 can_frame_t before=tx_get()->e[0][0].f;CHECK(ui_plan_file_select(0,old)==SP_ERR_CHANGED&&ui_page()==PG_PROJECTFILES&&!memcmp(&before,&tx_get()->e[0][0].f,sizeof before));
 CHECK(strstr(json(),"\"plan_token\":")&&strstr(json(),"\"items_start\":0")&&strstr(json(),"\"items_total\":2"));
 CHECK(ui_plan_file_select(0,file.token)==0&&ui_page()==PG_TX&&!tx_any_active());
}
static void timer_delivery_retries_a_full_event_queue_once(void){
 sim_reset();for(unsigned i=0;i<APP_EVT_QUEUE_LEN;i++)CHECK(evt_post4(EV_NONE,0,SRC_SYS,0,0));
 tmr_start(TMR_REC_GUARD,1);app_clock_advance(1);tmr_tick();CHECK(tmr_active(TMR_REC_GUARD));
 app_evt_t event;CHECK(evt_pop(&event)&&event.type==EV_NONE);tmr_tick();CHECK(!tmr_active(TMR_REC_GUARD));
 tmr_tick();unsigned delivered=0,other=0;while(evt_pop(&event)){if(event.type==EV_TIMER){CHECK(event.a==TMR_REC_GUARD);delivered++;}else other++;}
 CHECK(delivered==1&&other==APP_EVT_QUEUE_LEN-1);tmr_tick();CHECK(!evt_pop(&event));
}
static void cached_historic_log_can_be_named_without_payload_index(void){
 boot();file_info_t file;CHECK(port_sd_file_info(2,&file)&&!file.sample_available&&!file.writing&&!file.protected_);
 uint32_t token=file_port_token(2);CHECK(token!=0&&ui_review_navigate(PG_FILES)==0);post(EV_TOUCH_ITEM,0,2,0);
 CHECK(ui_page()==PG_FILEDETAIL&&strstr(json(),"帧数未索引")&&!strstr(json(),"内置只读样例"));
 post(EV_TOUCH_ITEM,0,5,0);CHECK(local_files_draft()&&strstr(json(),"local_rename"));local_files_close();
 CHECK(local_files_open(5,0,0,0)==0&&strstr(json(),"candump_1M.log"));local_files_close();
 CHECK(local_files_open(4,0,2,token+1)==LOCAL_FILES_ERR_CHANGED);port_sd_protect_toggle(2);
 CHECK(local_files_open(4,0,2,token)==FILE_PORT_ERR_READONLY);
}
int main(void){target_home_keyboard_and_database_boundary();app_owns_scheduling_and_independent_switches();plan_load_rounds_and_soft_boot_lifecycle();stopped_recording_uses_device_file_port();sd_wait_keeps_existing_tx_but_never_dispatches_commands();sd_wait_consumes_only_tx_dwell_timeout();elapsed_consumed_in_sd_still_services_foreground_owners();queued_old_timeout_cannot_abort_new_frame();storage_error_survives_software_boot();displayed_plan_identity_rejects_replaced_row();timer_delivery_retries_a_full_event_queue_once();cached_historic_log_can_be_named_without_payload_index();printf("Target device core on simulated ports: %u checks passed\n",checks);return 0;}
