/* SPDX-License-Identifier: MIT */
#ifndef REVD_TUSB_CONFIG_H
#define REVD_TUSB_CONFIG_H
#define CFG_TUSB_MCU OPT_MCU_STM32H7
#ifdef REVD_FREERTOS
#define CFG_TUSB_OS OPT_OS_FREERTOS
#else
#define CFG_TUSB_OS OPT_OS_NONE
#endif
#define CFG_TUSB_DEBUG 0
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_TASK_QUEUE_SZ 32
/* PA9 is UART1_TX on Rev D, not VBUS. Use host traffic as link evidence. */
#define CFG_TUD_VBUS_DETECT_HW 0
#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 1
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_EP_BUFSIZE 64
#define CFG_TUD_MSC_EP_BUFSIZE 512
/* FIFO slave mode avoids USB DMA/cache coherency and DMA-inaccessible DTCM.
 * SD has its own aligned bounce buffer and never DMA-transfers TinyUSB memory. */
#define CFG_TUD_DWC2_DMA_ENABLE 0
#define CFG_TUD_DWC2_SLAVE_ENABLE 1
#define CFG_TUD_MEM_DCACHE_ENABLE 0
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#define CFG_TUD_MEM_ALIGN __attribute__((aligned(4)))
#endif
