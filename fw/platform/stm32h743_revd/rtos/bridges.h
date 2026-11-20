/* SPDX-License-Identifier: MIT */
#ifndef REVD_RTOS_BRIDGES_H
#define REVD_RTOS_BRIDGES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "services/core/app_types.h"
#include "services/power/power_sm.h"
#include "display.h"

typedef struct { int page, cursor, ask; bool dialog, locked; uint32_t generation; } revd_ui_view_t;
typedef struct {
    uint32_t input_dropped, usb_callback_dropped, stale_touch_ignored;
    uint32_t max_input_latency_us, max_snapshot_wait_us;
    uint32_t usb_rx_bytes, usb_tx_bytes;
} revd_bridge_metrics_t;
/* UI task asks core to populate its buffer while it waits; parsing and all LVGL
 * remain UI-owned. Core never waits for UI while refusing snapshot requests. */
bool revd_ui_snapshot(char *buffer, size_t cap, size_t *length, revd_ui_view_t *view);
void revd_ui_commit_view(const revd_ui_view_t *view); /* Only after successful UI parse/render. */
void revd_ui_core_observe(void); /* Core only, after each UI service event. */
void revd_ui_touch(ev_type_t type, int32_t a);
void revd_ui_key(key_id_t key, bool down);
const power_t *revd_ui_power(void); /* UI's copy, refreshed at start of its step */
bool revd_ui_init_sync(void);
void revd_ui_stop_sync(void);
void revd_ui_backlight(uint8_t percent);
void revd_ui_request_redraw(void);
void revd_ui_diagnostics(display_diag_t *out);
void revd_storage_file_removed(int index);
void revd_bridge_metrics(revd_bridge_metrics_t *out);

/* CDC core/control <-> sole TinyUSB transport owner. Short critical copies only.
 * No TinyUSB operation occurs from a core control/stream callback. */
uint32_t revd_usb_rx_available(void);
int32_t revd_usb_rx_char(void);
uint32_t revd_usb_tx_available(void);
uint32_t revd_usb_write(const void *bytes, uint32_t count);
uint32_t revd_usb_flush(void);
bool revd_usb_mounted(void);
bool revd_usb_suspended(void);
bool revd_usb_connected(void);
void revd_usb_publish_link(bool mounted, bool suspended, bool connected);
uint32_t revd_usb_transport_read_space(void);
uint32_t revd_usb_transport_receive(const void *bytes, uint32_t count);
uint32_t revd_usb_transport_send_peek(void *bytes, uint32_t cap);
void revd_usb_transport_send_advance(uint32_t count);
typedef enum { REVD_USB_MOUNT, REVD_USB_UNMOUNT, REVD_USB_RESUME, REVD_USB_DTR } revd_usb_callback_kind_t;
void revd_usb_callback(revd_usb_callback_kind_t type, bool value);
bool revd_usb_callback_pop(revd_usb_callback_kind_t *type, bool *value);
bool revd_usb_core_ready(void);
#endif
