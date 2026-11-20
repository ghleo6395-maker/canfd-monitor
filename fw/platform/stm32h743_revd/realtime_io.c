/* SPDX-License-Identifier: MIT */
#include "realtime_io.h"
#include "board.h"
#include "display.h"
#include "usb_device.h"
#include "app/app.h"
#include "services/core/port.h"
#include "can_ingress.h"
#include <stdbool.h>
#ifdef REVD_FREERTOS
#include "rtos/runtime.h"
#endif

static uint32_t loop_clock_sample;
static bool loop_clock_started;
void revd_loop_clock_start(uint32_t now_ms)
{
    loop_clock_sample=now_ms;loop_clock_started=true;
}
uint32_t revd_loop_clock_take(uint32_t now_ms)
{
    if(!loop_clock_started)return 0;
    uint32_t elapsed=now_ms-loop_clock_sample;
    loop_clock_sample=now_ms;
    return elapsed;
}

void revd_io_wait_poll(void)
{
#if defined(CANFD_REVD_TARGET)
    if (__get_IPSR() != 0u) return;
#endif
#ifdef REVD_FREERTOS
    /* A wait yields only the current owner; no cross-task app/LVGL/TinyUSB
     * reentry and no global mutex held while the card is programming. */
    taskYIELD();
    return;
#endif
    static bool active;
    if (active) return;
    active = true;
    /* CAN IRQs fill SPSC ingress independently. Bound foreground delivery,
     * then immediately release all broadcast readers into the SD frame queue. */
    board_can_task();
    app_realtime_poll(revd_loop_clock_take(HAL_GetTick()));
    /* Pure DMA progression and guarded USB transport; no UI/host commands. */
    display_dma_poll(HAL_GetTick());
    usb_device_poll_transport();
    active = false;
}

void revd_sdmmc_cmd_wait_poll(void)
{
    /* The vendored SDMMC LL also runs from SD IRQ stop-command handling. */
#if defined(CANFD_REVD_TARGET)
    if (__get_IPSR() != 0u) return;
#endif
#ifdef REVD_FREERTOS
    revd_rtos_poll_command_wait();
#else
    revd_io_wait_poll();
#endif
}

void port_can_flush_rx(void)
{
    /* Bounded drain after both hardware RX producers stop. */
    for (unsigned batch = 0; batch < (2u * CAN_INGRESS_LEN + 64u + 127u) / 128u; ++batch) {
        board_can_task();
        app_capture_poll();
    }
}
