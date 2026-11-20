/* SPDX-License-Identifier: MIT
 * Ownership boundary. Core alone touches app services/ring/UI view model;
 * SD owns its filesystem; UI owns LVGL; USB owns TinyUSB. No global app mutex.
 */
#include "bridges.h"
#include "runtime.h"
#include "storage_owner.h"
#include "board.h"
#include "usb_device.h"
#include "queue.h"
#include "semphr.h"
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/core/jsonw.h"
#include "services/core/port.h"
#include "services/input/keys.h"
#include "services/player/play_sm.h"
#include <string.h>

#define INPUT_LEN 32u
#define USB_CALLBACK_LEN 16u
#define USB_RX_LEN 1024u
#define USB_TX_LEN 2048u

typedef struct { uint32_t at_us, generation; int32_t a; uint16_t type; uint8_t key; bool down, is_key; } ui_input_t;
typedef struct { revd_usb_callback_kind_t type; bool value; } usb_callback_t;
static StaticQueue_t input_q_state, callback_q_state;
static uint8_t input_q_bytes[INPUT_LEN * sizeof(ui_input_t)];
static uint8_t callback_q_bytes[USB_CALLBACK_LEN * sizeof(usb_callback_t)];
static QueueHandle_t input_q, callback_q;
static StaticSemaphore_t snapshot_sem_state, ui_cmd_sem_state;
static SemaphoreHandle_t snapshot_sem, ui_cmd_sem;
static struct {
    char *buffer; size_t cap, length; revd_ui_view_t view;
    bool pending, ok;
} snapshot;
static enum { UI_CMD_NONE, UI_CMD_INIT, UI_CMD_STOP } ui_cmd;
static bool ui_cmd_result, core_ready;
static uint8_t requested_backlight;
static bool pending_backlight, pending_redraw;
static power_t published_power, ui_power;
static display_diag_t published_lcd;
static revd_bridge_metrics_t metrics;
/* Task-only FIFOs: all indexes/copies protected against task switches. CAN IRQ4
 * remains enabled and only touches its own SPSC queue, never these FIFOs. */
static uint8_t rx_bytes[USB_RX_LEN], tx_bytes[USB_TX_LEN];
static uint32_t rx_in, rx_out, tx_in, tx_out;
static bool link_mounted, link_suspended, link_connected;
static uint32_t core_last_ms;
static revd_ui_view_t core_view, last_ui_view;
static uint32_t virtual_key_generation[KEY_NUM];

void revd_rtos_bridges_init(void)
{
    input_q = xQueueCreateStatic(INPUT_LEN, sizeof(ui_input_t), input_q_bytes, &input_q_state);
    callback_q = xQueueCreateStatic(USB_CALLBACK_LEN, sizeof(usb_callback_t), callback_q_bytes, &callback_q_state);
    snapshot_sem = xSemaphoreCreateBinaryStatic(&snapshot_sem_state);
    ui_cmd_sem = xSemaphoreCreateBinaryStatic(&ui_cmd_sem_state);
    configASSERT(input_q && callback_q && snapshot_sem && ui_cmd_sem);
    revd_storage_owner_bootstrap();
}
static void publish_power(void)
{
    taskENTER_CRITICAL(); published_power = *power_get(); taskEXIT_CRITICAL();
}
void revd_bridge_metrics(revd_bridge_metrics_t *out)
{
    taskENTER_CRITICAL(); *out = metrics; taskEXIT_CRITICAL();
}
void revd_ui_core_observe(void)
{
    int page = ui_page(), cursor = ui_cursor(), ask = ui_dialog_ask();
    bool dialog = ui_dialog_open(), locked = power_get()->locked;
    if (!core_view.generation || page != core_view.page || cursor != core_view.cursor ||
        ask != core_view.ask || dialog != core_view.dialog || locked != core_view.locked) {
        uint32_t generation = core_view.generation + 1u;
        bool changed_context = page != core_view.page || ask != core_view.ask ||
            dialog != core_view.dialog || locked != core_view.locked;
        for (unsigned k = 0; k < KEY_NUM; ++k) {
            bool arrow = k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT;
            /* Arrow repeats legitimately change cursor within the same page. */
            if (virtual_key_generation[k] && (changed_context || !arrow)) {
                keys_cancel((key_id_t)k); virtual_key_generation[k] = 0;
            }
        }
        core_view = (revd_ui_view_t){.page=page,.cursor=cursor,.ask=ask,
            .dialog=dialog,.locked=locked,.generation=generation};
    }
}
static void serve_snapshot(void)
{
    /* Called only by core, including its bounded wait for display init/stop. */
    taskENTER_CRITICAL(); bool pending = snapshot.pending; taskEXIT_CRITICAL();
    if (!pending) return;
    revd_ui_core_observe();
    jw_t j; jw_init(&j, snapshot.buffer, snapshot.cap);
    jw_obj(&j); ui_json(&j); jw_end_obj(&j);
    snapshot.length = jw_finish(&j);
    snapshot.ok = !j.overflow;
    snapshot.view = core_view;
    taskENTER_CRITICAL(); snapshot.pending = false; taskEXIT_CRITICAL();
    (void)xSemaphoreGive(snapshot_sem);
}
bool revd_ui_snapshot(char *buffer, size_t cap, size_t *length, revd_ui_view_t *view)
{
    configASSERT(xTaskGetCurrentTaskHandle() == revd_rtos_task_handle(REVD_RTOS_UI));
    uint32_t start = (uint32_t)board_time_us();
    taskENTER_CRITICAL();
    snapshot.buffer = buffer; snapshot.cap = cap; snapshot.pending = true;
    taskEXIT_CRITICAL();
    revd_rtos_notify_core();
    /* The UI buffer stays alive and untouched by UI until core completes.
     * No timeout can return while core still holds the pointer. */
    (void)xSemaphoreTake(snapshot_sem, portMAX_DELAY);
    *length = snapshot.length; *view = snapshot.view;
    uint32_t elapsed = (uint32_t)board_time_us() - start;
    taskENTER_CRITICAL();
    if (elapsed > metrics.max_snapshot_wait_us) metrics.max_snapshot_wait_us = elapsed;
    taskEXIT_CRITICAL();
    return snapshot.ok;
}
void revd_ui_commit_view(const revd_ui_view_t *view)
{
    configASSERT(xTaskGetCurrentTaskHandle() == revd_rtos_task_handle(REVD_RTOS_UI));
    last_ui_view = *view;
}
static void post_input(ui_input_t item)
{
    item.at_us = (uint32_t)board_time_us();
    item.generation = last_ui_view.generation;
    if (xQueueSend(input_q, &item, 0) != pdPASS) {
        taskENTER_CRITICAL(); ++metrics.input_dropped; taskEXIT_CRITICAL();
    } else revd_rtos_notify_core();
}
void revd_ui_touch(ev_type_t type, int32_t a) { post_input((ui_input_t){.type = type, .a = a}); }
void revd_ui_key(key_id_t key, bool down) { post_input((ui_input_t){.is_key = true, .key = key, .down = down}); }
const power_t *revd_ui_power(void) { return &ui_power; }
static bool ui_command(bool init)
{
    configASSERT(xTaskGetCurrentTaskHandle() == revd_rtos_task_handle(REVD_RTOS_CORE));
    taskENTER_CRITICAL(); ui_cmd = init ? UI_CMD_INIT : UI_CMD_STOP; taskEXIT_CRITICAL();
    revd_rtos_notify_ui();
    /* A UI task may already be waiting for its next view model. Service that
     * request before waiting again, avoiding a core<->UI circular wait. */
    while (xSemaphoreTake(ui_cmd_sem, pdMS_TO_TICKS(1)) != pdTRUE) serve_snapshot();
    return ui_cmd_result;
}
bool revd_ui_init_sync(void) { return ui_command(true); }
void revd_ui_stop_sync(void) { (void)ui_command(false); }
void revd_ui_backlight(uint8_t percent)
{
    taskENTER_CRITICAL(); requested_backlight = percent; pending_backlight = true; taskEXIT_CRITICAL();
    revd_rtos_notify_ui();
}
void revd_ui_request_redraw(void)
{
    taskENTER_CRITICAL(); pending_redraw = true; taskEXIT_CRITICAL(); revd_rtos_notify_ui();
}
void revd_ui_diagnostics(display_diag_t *out)
{
    taskENTER_CRITICAL(); *out = published_lcd; taskEXIT_CRITICAL();
}
void revd_ui_step(void)
{
    taskENTER_CRITICAL();
    ui_power = published_power;
    int cmd = ui_cmd; ui_cmd = UI_CMD_NONE;
    bool backlight = pending_backlight; pending_backlight = false;
    uint8_t percent = requested_backlight;
    bool redraw = pending_redraw; pending_redraw = false;
    taskEXIT_CRITICAL();
    if (cmd) {
        ui_cmd_result = cmd == UI_CMD_INIT ? display_init() : (display_poweroff(), true);
        (void)xSemaphoreGive(ui_cmd_sem);
    }
    if (backlight) display_set_backlight(percent);
    if (redraw) display_request_redraw();
    display_task(HAL_GetTick());
    taskENTER_CRITICAL(); published_lcd = *display_diagnostics(); taskEXIT_CRITICAL();
}
void revd_storage_file_removed(int index)
{
    /* SD no longer mutates player indexes across a task boundary. */
    (void)evt_post4(EV_NTF_FILE_REMOVED, 0, SRC_SYS, index, 0);
}
void revd_core_boot(void)
{
    app_init();
    revd_ui_core_observe();
    revd_storage_publish_context(); publish_power();
    while (!revd_storage_owner_ready()) vTaskDelay(pdMS_TO_TICKS(1));
    if (!port_key_down(KEY_POWER)) { app_key_raw(KEY_POWER, true); app_tick(1000); }
    if (!usb_device_init()) board_fatal(4);
    core_last_ms = HAL_GetTick();
    taskENTER_CRITICAL(); core_ready = true; taskEXIT_CRITICAL();
    publish_power();
}
void revd_core_step(void)
{
    uint32_t now = HAL_GetTick(), dt = now - core_last_ms;
    board_can_task(); app_capture_poll();
    if (dt) { core_last_ms = now; app_tick(dt); }
    revd_ui_core_observe();
    ui_input_t item;
    for (unsigned i = 0; i < INPUT_LEN && xQueueReceive(input_q, &item, 0) == pdPASS; ++i) {
        uint32_t age = (uint32_t)board_time_us() - item.at_us;
        taskENTER_CRITICAL(); if (age > metrics.max_input_latency_us) metrics.max_input_latency_us = age; taskEXIT_CRITICAL();
        /* A delayed index must never act on a different page or confirmation.
         * Releases remain deliverable after navigation to avoid stuck keys. */
        bool neutral = !item.is_key && item.type == EV_NONE;
        bool release = item.is_key && !item.down;
        if (!neutral && !release && item.generation != core_view.generation) {
            taskENTER_CRITICAL(); ++metrics.stale_touch_ignored; taskEXIT_CRITICAL();
            continue;
        }
        if (item.is_key) {
            if (item.key >= KEY_NUM) continue;
            if (!item.down && !virtual_key_generation[item.key]) continue;
            app_key_raw((key_id_t)item.key, item.down);
            virtual_key_generation[item.key] = item.down ? core_view.generation : 0;
        } else app_touch((ev_type_t)item.type, item.a);
        revd_ui_core_observe();
    }
    board_io_task();
    revd_storage_publish_context();
    usb_device_task();
    serve_snapshot();
    app_tick(0);
    revd_storage_publish_context();
    publish_power();
    (void)board_time_us();
}
bool revd_usb_core_ready(void)
{
    taskENTER_CRITICAL(); bool ready = core_ready; taskEXIT_CRITICAL(); return ready;
}
uint32_t revd_usb_rx_available(void)
{
    taskENTER_CRITICAL(); uint32_t n = rx_in - rx_out; taskEXIT_CRITICAL(); return n;
}
int32_t revd_usb_rx_char(void)
{
    taskENTER_CRITICAL(); int32_t c = rx_in == rx_out ? -1 : rx_bytes[rx_out++ % USB_RX_LEN]; taskEXIT_CRITICAL(); return c;
}
uint32_t revd_usb_tx_available(void)
{
    taskENTER_CRITICAL(); uint32_t n = USB_TX_LEN - (tx_in - tx_out); taskEXIT_CRITICAL(); return n;
}
uint32_t revd_usb_write(const void *bytes, uint32_t count)
{
    taskENTER_CRITICAL();
    uint32_t n = USB_TX_LEN - (tx_in - tx_out); if (n > count) n = count;
    for (uint32_t i = 0; i < n; ++i) tx_bytes[tx_in++ % USB_TX_LEN] = ((const uint8_t *)bytes)[i];
    taskEXIT_CRITICAL(); revd_rtos_notify_usb(); return n;
}
uint32_t revd_usb_flush(void) { return 0; }
void revd_usb_publish_link(bool mounted, bool suspended, bool connected)
{
    taskENTER_CRITICAL(); link_mounted = mounted; link_suspended = suspended; link_connected = connected; taskEXIT_CRITICAL();
}
bool revd_usb_mounted(void) { taskENTER_CRITICAL(); bool v = link_mounted; taskEXIT_CRITICAL(); return v; }
bool revd_usb_suspended(void) { taskENTER_CRITICAL(); bool v = link_suspended; taskEXIT_CRITICAL(); return v; }
bool revd_usb_connected(void) { taskENTER_CRITICAL(); bool v = link_connected; taskEXIT_CRITICAL(); return v; }
uint32_t revd_usb_transport_read_space(void)
{
    taskENTER_CRITICAL(); uint32_t n = USB_RX_LEN - (rx_in - rx_out); taskEXIT_CRITICAL(); return n;
}
uint32_t revd_usb_transport_receive(const void *bytes, uint32_t count)
{
    taskENTER_CRITICAL();
    uint32_t n = USB_RX_LEN - (rx_in - rx_out); if (n > count) n = count;
    for (uint32_t i = 0; i < n; ++i) rx_bytes[rx_in++ % USB_RX_LEN] = ((const uint8_t *)bytes)[i];
    metrics.usb_rx_bytes += n;
    taskEXIT_CRITICAL(); if (n) revd_rtos_notify_core(); return n;
}
uint32_t revd_usb_transport_send_peek(void *bytes, uint32_t cap)
{
    taskENTER_CRITICAL(); uint32_t n = tx_in - tx_out; if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; ++i) ((uint8_t *)bytes)[i] = tx_bytes[(tx_out + i) % USB_TX_LEN];
    taskEXIT_CRITICAL(); return n;
}
void revd_usb_transport_send_advance(uint32_t count)
{
    taskENTER_CRITICAL(); configASSERT(count <= tx_in - tx_out); tx_out += count; metrics.usb_tx_bytes += count; taskEXIT_CRITICAL();
}
void revd_usb_callback(revd_usb_callback_kind_t type, bool value)
{
    usb_callback_t item = {type, value};
    /* One atomic publication: core cannot refill an old session's bytes
     * between the reset and its corresponding lifecycle callback. */
    taskENTER_CRITICAL();
    if (type != REVD_USB_RESUME) { rx_out = rx_in; tx_out = tx_in; }
    bool queued = xQueueSend(callback_q, &item, 0) == pdPASS;
    if (!queued) {
        ++metrics.usb_callback_dropped;
        /* Fail closed. A synthetic unmount is the only retained callback. */
        (void)xQueueReset(callback_q);
        usb_callback_t unmount = {REVD_USB_UNMOUNT, false};
        (void)xQueueSend(callback_q, &unmount, 0);
        link_connected = false; link_mounted = false;
        rx_out = rx_in; tx_out = tx_in;
    }
    taskEXIT_CRITICAL();
    revd_rtos_notify_core();
}
bool revd_usb_callback_pop(revd_usb_callback_kind_t *type, bool *value)
{
    usb_callback_t item;
    if (xQueueReceive(callback_q, &item, 0) != pdPASS) return false;
    *type = item.type; *value = item.value; return true;
}
