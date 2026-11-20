/* SPDX-License-Identifier: MIT
 * Composite descriptor layout follows TinyUSB's MIT cdc_msc example.
 * Copyright (c) 2019 Ha Thach (tinyusb.org), as distributed in vendor/tinyusb/LICENSE.
 */
#include "tusb.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

/* Local laboratory identifiers, NOT assigned by USB-IF. Never ship with these.
 * Maintainers must define both REVD_USB_VID/PID using their own allocation before
 * distributing hardware. They do not imply certification or a third-party ID. */
#ifndef REVD_USB_VID
#define REVD_USB_VID 0xCAFDu
#endif
#ifndef REVD_USB_PID
#define REVD_USB_PID 0x0001u
#endif

static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = REVD_USB_VID, .idProduct = REVD_USB_PID, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1
};
enum { ITF_CDC, ITF_CDC_DATA, ITF_MSC, ITF_COUNT };
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t configuration[] = {
    /* Self powered capability: battery/12 V can run the board. USB500 charger
     * hardware permits 450..500 mA, so the configuration declares 500 mA.
     * Rev D cannot reduce its charging limit to USB100 before configuration;
     * this descriptor does not establish USB power-compliance certification. */
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 500),
    TUD_CDC_DESCRIPTOR(ITF_CDC, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 5, 0x03, 0x83, 64)
};
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    return index == 0 ? configuration : NULL;
}
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t descriptor[33];
    static char serial[25];
    const char *s = NULL;
    if (index == 0) {
        descriptor[1] = 0x0409;
        descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        return descriptor;
    }
    switch (index) {
#ifdef CANFD_REVE_TARGET
    case 1: s = "Rev E laboratory"; break;
    case 2: s = "CAN FD Rev E draft"; break;
#else
    case 1: s = "Rev D laboratory"; break;
    case 2: s = "CAN FD Rev D development"; break;
#endif
    case 3:
        (void)snprintf(serial, sizeof serial, "%08lX%08lX%08lX",
                       (unsigned long)HAL_GetUIDw0(), (unsigned long)HAL_GetUIDw1(),
                       (unsigned long)HAL_GetUIDw2());
        s = serial; break;
    case 4: s = "Control and candump stream"; break;
    case 5: s = "SD card (device permission)"; break;
    default: return NULL;
    }
    size_t len = strlen(s);
    if (len > 32) len = 32;
    for (size_t i = 0; i < len; ++i) descriptor[i + 1u] = (uint8_t)s[i];
    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * len + 2u));
    return descriptor;
}
