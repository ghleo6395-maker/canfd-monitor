/* SPDX-License-Identifier: MIT */
#ifndef REVD_USB_DEVICE_H
#define REVD_USB_DEVICE_H
#include <stdbool.h>
#include <stdint.h>

/* Requires HAL clocks, HSI48+CRS and app_init() first. No HAL PCD middleware. */
bool usb_device_init(void);
/* Call every superloop iteration. Callbacks and service access run here only. */
void usb_device_task(void);
/* Foreground only, recursion guarded; handles transport without parsing commands. */
void usb_device_poll_transport(void);
uint32_t usb_device_stream_dropped(void);
uint32_t usb_device_reply_dropped(void);
bool usb_device_media_ready(void);
#ifdef CANFD_REVE_TARGET
/* Constant-time electrical detach; no TinyUSB task or application callbacks. */
void usb_device_vbus_tick(void);
#endif
#endif
