/* SPDX-License-Identifier: MIT
 * Pinned upstream FreeRTOS POSIX kernel + production bridges/USB/event queue.
 * The panel/HAL/TinyUSB endpoint facade is RAM-only. This verifies ownership
 * and lifecycle, not ARM timing or physical throughput.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "platform/stm32h743_revd/rtos/runtime.h"
#include "platform/stm32h743_revd/rtos/bridges.h"
#include "services/core/jsonw.h"
#include "app/ui/core/ui_sm.h"
#include <stdatomic.h>
#define main legacy_usb_test_main
#define ui_dialog_ask legacy_ui_dialog_ask
#include "../usb/test_usb_device.c"
#undef ui_dialog_ask
#undef main

static TaskHandle_t tasks[4];
static StaticTask_t task_tcbs[4];
static StackType_t task_stacks[4][16384];
static bool owner_ready, panel_active, sd_waiting, stop_done, mock_done;
static unsigned core_work, work_during_sd_wait, touch_work, key_work, snapshots, panel_steps;
static unsigned sd_steps, endpoint_calls, transport_steps;
static bool saw_key_down, saw_key_up, injected_host;
static unsigned callback_reads;
static page_t fixture_page=PG_HOME;
static int fixture_ask=-1;
static bool fixture_dialog, virtual_held;
static bool force_bad_snapshot;
static unsigned virtual_cancels, host_stale_commands, host_detached, failed_snapshots;
static char oversized_snapshot[700];
static _Atomic unsigned ui_regression_phase, usb_regression_phase;
static const char old_tx_session[]="OLD-TX-SESSION-MUST-NOT-LEAK";
static const char old_tx_overflow[]="OLD-TX-OVERFLOW-MUST-NOT-LEAK";
static void require_owner(unsigned id) { CHECK(xTaskGetCurrentTaskHandle() == tasks[id]); }
void keys_cancel(key_id_t key) { require_owner(0); CHECK(key==KEY_REC);virtual_held=false;++virtual_cancels; }
TaskHandle_t revd_rtos_task_handle(revd_rtos_task_id_t id) { return tasks[id]; }
bool revd_rtos_task_metrics(revd_rtos_task_id_t id, revd_rtos_task_metrics_t *out) {
    *out = (revd_rtos_task_metrics_t){.runs = id == REVD_RTOS_CORE ? core_work : id == REVD_RTOS_UI ? panel_steps : sd_steps,
        .max_step_us = 100, .stack_min_free_bytes = 8192, .stack_size_bytes = sizeof task_stacks[0]}; return true;
}
bool revd_rtos_running(void) { return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING; }
static void notify(unsigned id) { if (tasks[id]) xTaskNotifyGive(tasks[id]); }
void revd_rtos_notify_core(void) { notify(0); }
void revd_rtos_notify_storage(void) { notify(1); }
void revd_rtos_notify_usb(void) { notify(2); }
void revd_rtos_notify_ui(void) { notify(3); }
uint64_t board_time_us(void) { return (uint64_t)xTaskGetTickCount() * 1000u; }
void board_fatal(uint32_t code) { (void)code; abort(); }
void board_can_task(void) { require_owner(0); ++core_work; if (sd_waiting) ++work_during_sd_wait; }
void board_io_task(void) { require_owner(0); }
bool port_key_down(key_id_t key) { (void)key; return true; }
void app_init(void) {
    require_owner(0); evt_init();
    power = (power_t){.state = PWR_RUNNING, .screen_on = true};
    rec = (rec_t){.state = REC_IDLE, .last_result = MSG_REC_STOPPED};
}
void app_tick(uint32_t dt) {
    (void)dt; require_owner(0); app_evt_t e;
    while (evt_pop(&e)) {
        if (e.type == EV_NTF_FILE_REMOVED) CHECK(e.a == 7);
        if (e.type == EV_REQ_REC_START) ++host_stale_commands;
        if (e.type == EV_HOST_LINK && !e.a) ++host_detached;
    }
}
void app_capture_poll(void) { require_owner(0); }
void app_key_raw(key_id_t key, bool down) {
    require_owner(0); CHECK(key == KEY_REC); ++key_work;
    virtual_held=down;
    if (down) saw_key_down = true; else saw_key_up = true;
}
void app_touch(ev_type_t type, int32_t a) { require_owner(0); CHECK(type == EV_TOUCH_ITEM && a == 3); ++touch_work; }
void ui_json(jw_t *j) {
    require_owner(0); ++snapshots; jw_key(j,"ui"); jw_obj(j);
    jw_kstr(j,"title","kernel ownership fixture");
    if(force_bad_snapshot)jw_kstr(j,"oversized",oversized_snapshot);
    jw_end_obj(j);
}
page_t ui_page(void) { require_owner(0); return fixture_page; }
int ui_cursor(void) { require_owner(0); return 0; }
bool ui_dialog_open(void) { require_owner(0); return fixture_dialog; }
int ui_dialog_ask(void) { require_owner(0); return fixture_ask; }
bool display_init(void) { require_owner(3); vTaskDelay(3); panel_active = true; return true; }
void display_poweroff(void) { require_owner(3); panel_active = false; }
void display_set_backlight(uint8_t percent) { require_owner(3); CHECK(percent <= 100); }
void display_request_redraw(void) { require_owner(3); }
void display_task(uint32_t now) {
    (void)now; require_owner(3); ++panel_steps;
    if (!panel_active) return;
    char text[512]; size_t len; revd_ui_view_t view;
    bool ok=revd_ui_snapshot(text,sizeof text,&len,&view);
    if(!ok) {
        CHECK(atomic_load(&ui_regression_phase)==11);++failed_snapshots;
        /* The old rendered dialog remains on the panel. Fetching a newer
         * view that could not be rendered must NOT authorize its old click. */
        vTaskSuspendAll();revd_ui_touch(EV_TOUCH_DLG,0);
        atomic_store(&ui_regression_phase,12);(void)xTaskResumeAll();return;
    }
    CHECK(len == strlen(text) && strstr(text,"kernel ownership fixture"));
    CHECK(view.page >= PG_HOME && view.page < PG_NUM && view.cursor == 0);
    revd_ui_commit_view(&view); /* Fixture's parse/render succeeded. */
    if (panel_steps == 2) { revd_ui_touch(EV_TOUCH_ITEM,3); revd_ui_key(KEY_REC,true); }
    if (panel_steps == 3) revd_ui_key(KEY_REC,false);
    unsigned phase=atomic_load(&ui_regression_phase);
    if(phase==1 && view.dialog && view.ask==ASK_REC_STOP) {
        /* The old dialog's click and press wait in the queue while core moves
         * to another confirmation of the SAME page/cursor. */
        vTaskSuspendAll();
        revd_ui_touch(EV_TOUCH_DLG,0);revd_ui_touch(EV_TOUCH_ITEM,3);revd_ui_key(KEY_REC,true);
        atomic_store(&ui_regression_phase,2);
        (void)xTaskResumeAll();
    } else if(phase==3 && view.dialog && view.ask==ASK_FORMAT) {
        vTaskSuspendAll();revd_ui_key(KEY_REC,true);atomic_store(&ui_regression_phase,4);(void)xTaskResumeAll();
    } else if(phase==6 && view.page==PG_SYSSET && !view.dialog) {
        vTaskSuspendAll();revd_ui_key(KEY_REC,false);atomic_store(&ui_regression_phase,7);(void)xTaskResumeAll();
    } else if(phase==9 && view.dialog && view.ask==ASK_REC_STOP) {
        atomic_store(&ui_regression_phase,10);
    } else if(phase==13 && view.dialog && view.ask==ASK_FORMAT) {
        atomic_store(&ui_regression_phase,14);
    }
}
void revd_storage_owner_bootstrap(void) { owner_ready = false; }
void revd_storage_owner_init(void) { require_owner(1); owner_ready = true; }
bool revd_storage_owner_ready(void) { return owner_ready; }
void revd_storage_publish_context(void) { require_owner(0); }
void revd_storage_owner_step(void) {
    require_owner(1); ++sd_steps;
    if (sd_steps == 5) { sd_waiting = true; vTaskDelay(25); sd_waiting = false; }
    if (sd_steps == 12) revd_storage_file_removed(7);
}
bool tud_cdc_n_connected(uint8_t itf) { require_owner(2); CHECK(itf == 0); return cdc_dtr; }
uint32_t tud_cdc_n_available(uint8_t itf) { require_owner(2); CHECK(itf == 0); return tud_cdc_available(); }
uint32_t tud_cdc_n_read(uint8_t itf,void *buffer,uint32_t count) {
    require_owner(2); CHECK(itf == 0); ++endpoint_calls;
    uint32_t n = tud_cdc_available(); if (n > count) n = count;
    for (uint32_t i = 0; i < n; ++i) ((char *)buffer)[i] = (char)tud_cdc_read_char(); return n;
}
uint32_t tud_cdc_n_write_available(uint8_t itf) { require_owner(2); CHECK(itf == 0); return tud_cdc_write_available(); }
uint32_t tud_cdc_n_write(uint8_t itf,const void *buffer,uint32_t count) { require_owner(2); CHECK(itf == 0); ++endpoint_calls; return tud_cdc_write(buffer,count); }
uint32_t tud_cdc_n_write_flush(uint8_t itf) { require_owner(2); CHECK(itf == 0); return 0; }
static void core_main(void *arg) {
    (void)arg; revd_core_boot();
    CHECK(revd_ui_init_sync());
    for (unsigned n = 0; n < 240; ++n) {
        now_ms = xTaskGetTickCount(); test_usb_regs.DSTS = (now_ms & 0x3fffu) << 8;
        if(n==60) {
            fixture_dialog=true;fixture_ask=ASK_REC_STOP;revd_ui_core_observe();
            atomic_store(&ui_regression_phase,1);
        }
        unsigned phase=atomic_load(&ui_regression_phase);
        if(phase==2) {
            fixture_ask=ASK_FORMAT;revd_ui_core_observe();atomic_store(&ui_regression_phase,3);
        } else if(phase==5) {
            fixture_page=PG_SYSSET;fixture_dialog=false;fixture_ask=-1;
            revd_ui_core_observe();atomic_store(&ui_regression_phase,6);
        } else if(phase==10) {
            fixture_ask=ASK_FORMAT;force_bad_snapshot=true;
            revd_ui_core_observe();atomic_store(&ui_regression_phase,11);
        } else if(phase==12) {
            force_bad_snapshot=false;
        } else if(phase==14) {
            fixture_page=PG_SYSSET;fixture_dialog=false;fixture_ask=-1;
            revd_ui_core_observe();atomic_store(&ui_regression_phase,15);
        }
        if(n==85) {
            CHECK(atomic_load(&ui_regression_phase)==8);
            fixture_page=PG_HOME;fixture_dialog=true;fixture_ask=ASK_REC_STOP;
            revd_ui_core_observe();atomic_store(&ui_regression_phase,9);
        }
        if(n==110) {
            CHECK(atomic_load(&ui_regression_phase)==15&&failed_snapshots==1);
            CHECK(revd_usb_write(old_tx_session,sizeof old_tx_session-1)==sizeof old_tx_session-1);
            atomic_store(&usb_regression_phase,1);
        }
        if(n==130) {
            CHECK(atomic_load(&usb_regression_phase)==2);
            CHECK(revd_usb_write(old_tx_overflow,sizeof old_tx_overflow-1)==sizeof old_tx_overflow-1);
            atomic_store(&usb_regression_phase,3);
        }
        revd_core_step();
        phase=atomic_load(&ui_regression_phase);
        if(phase==4 && virtual_held) atomic_store(&ui_regression_phase,5);
        if(phase==7) {
            CHECK(!virtual_held&&virtual_cancels==1&&key_work==3&&touch_work==1);
            revd_bridge_metrics_t metrics;revd_bridge_metrics(&metrics);CHECK(metrics.stale_touch_ignored>=3);
            atomic_store(&ui_regression_phase,8);
        } else if(phase==12) {
            CHECK(touch_work==1);
            revd_bridge_metrics_t metrics;revd_bridge_metrics(&metrics);CHECK(metrics.stale_touch_ignored>=4);
            atomic_store(&ui_regression_phase,13);
        }
        if(atomic_load(&usb_regression_phase)==4 && host_detached) {
            CHECK(host_stale_commands==0);
            CHECK(!strstr(tx,old_tx_session)&&!strstr(tx,old_tx_overflow));
            atomic_store(&usb_regression_phase,5);
        }
        if(atomic_load(&usb_regression_phase)==6 && revd_usb_connected()) atomic_store(&usb_regression_phase,7);
        if (n == 45) { revd_ui_stop_sync(); stop_done = true; CHECK(revd_ui_init_sync()); }
        vTaskDelay(1);
    }
    CHECK(core_work >= 120 && work_during_sd_wait >= 10 && sd_steps > 20);
    CHECK(touch_work == 1 && key_work == 3 && saw_key_down && saw_key_up&&!virtual_held&&virtual_cancels==1);
    CHECK(atomic_load(&ui_regression_phase)==15&&atomic_load(&usb_regression_phase)==7&&failed_snapshots==1);
    CHECK(host_stale_commands==0&&!strstr(tx,old_tx_session)&&!strstr(tx,old_tx_overflow));
    CHECK(snapshots > 20 && panel_steps > 20 && stop_done && transport_steps > 20);
    CHECK(strstr(tx,"revd-freertos-20261024") && strstr(tx,"\"scheduler\":\"freertos\""));
    CHECK(strstr(tx,"\"irq_read_errors\":0") && !strstr(tx,"STATUS_OVERFLOW"));
    revd_bridge_metrics_t m; revd_bridge_metrics(&m);
    CHECK(m.input_dropped == 0 && m.usb_callback_dropped == 1 && m.usb_rx_bytes >= 7 && m.usb_tx_bytes > 1000);
    mock_done = true; vTaskEndScheduler(); abort();
}
static void storage_main(void *arg) { (void)arg; revd_storage_owner_init(); for (;;) { revd_storage_owner_step(); vTaskDelay(1); } }
static void usb_main(void *arg) {
    (void)arg;
    while (!revd_usb_core_ready()) vTaskDelay(1);
    mounted = cdc_dtr = true; tud_mount_cb(); tud_cdc_line_state_cb(0,true,false);
    for (;;) {
        ++transport_steps;
        unsigned phase=atomic_load(&usb_regression_phase);
        if(phase==1) {
            vTaskSuspendAll();
            CHECK(revd_usb_transport_receive("REC START\n",10)==10);
            revd_usb_callback(REVD_USB_DTR,false);revd_usb_callback(REVD_USB_DTR,true);
            char scratch[64];CHECK(revd_usb_rx_available()==0);
            CHECK(revd_usb_transport_send_peek(scratch,sizeof scratch)==0);
            atomic_store(&usb_regression_phase,2);(void)xTaskResumeAll();
        } else if(phase==3) {
            vTaskSuspendAll();
            CHECK(revd_usb_transport_receive("REC START\n",10)==10);
            /* Callback queue capacity is 16. Overflow must retain one unmount,
             * clear both session FIFOs and publish a disconnected link. */
            for(unsigned n=0;n<17;n++)revd_usb_callback(REVD_USB_RESUME,true);
            char scratch[64];CHECK(revd_usb_rx_available()==0);
            CHECK(revd_usb_transport_send_peek(scratch,sizeof scratch)==0);
            CHECK(!revd_usb_connected()&&!revd_usb_mounted());
            atomic_store(&usb_regression_phase,4);(void)xTaskResumeAll();
        } else if(phase==5) {
            mounted=cdc_dtr=true;tud_mount_cb();tud_cdc_line_state_cb(0,true,false);
            atomic_store(&usb_regression_phase,6);
        }
        if (!injected_host && transport_steps == 3) { strcpy(rx,"STATUS\n"); rx_len=7; rx_pos=0; injected_host=true; }
        revd_usb_transport_step(); ++callback_reads; vTaskDelay(1);
    }
}
static void ui_main(void *arg) { (void)arg; for (;;) { revd_ui_step(); vTaskDelay(1); } }
int main(void) {
    memset(oversized_snapshot,'X',sizeof oversized_snapshot-1);
    revd_rtos_bridges_init();
    TaskFunction_t entries[] = {core_main,storage_main,usb_main,ui_main};
    for (unsigned i = 0; i < 4; ++i) { tasks[i]=xTaskCreateStatic(entries[i],"ownership",16384,0,5-i,task_stacks[i],&task_tcbs[i]); CHECK(tasks[i]); }
    vTaskStartScheduler();
    printf("{\"kernel\":\"FreeRTOS V11.3.2\",\"port\":\"upstream GCC/POSIX\",\"test\":\"production bridges+USB+events ownership and stale/session regressions\",\"checks\":%u,\"core_steps\":%u,\"core_steps_during_sd_wait\":%u,\"ui_snapshots\":%u,\"endpoint_calls\":%u,\"virtual_cancels\":%u,\"stale_host_commands\":%u,\"physical_hardware_tested\":false,\"pass\":%s}\n",checks,core_work,work_during_sd_wait,snapshots,endpoint_calls,virtual_cancels,host_stale_commands,mock_done?"true":"false");
    return mock_done?0:1;
}
