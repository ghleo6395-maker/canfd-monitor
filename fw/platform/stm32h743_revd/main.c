/* SPDX-License-Identifier: MIT */
#include "board.h"
#include "display.h"
#include "storage_sd.h"
#include "usb_device.h"
#include "app/app.h"
#include "services/core/port.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/power/power_sm.h"
#include "realtime_io.h"
#include "power_reve.h"
#if defined(REVD_FREERTOS)
#include "rtos/runtime.h"
void xPortSysTickHandler(void);
#endif

void SysTick_Handler(void) {
    HAL_IncTick();
#ifdef CANFD_REVE_TARGET
    reve_power_tick();
    usb_device_vbus_tick();
#endif
#if defined(REVD_FREERTOS)
    if (revd_rtos_running()) xPortSysTickHandler();
#endif
}
void HardFault_Handler(void) { board_fatal(0x10); }
void MemManage_Handler(void) { board_fatal(0x11); }
void BusFault_Handler(void) { board_fatal(0x12); }
void UsageFault_Handler(void) { board_fatal(0x13); }

int main(void)
{
    /* D-cache remains disabled: DMA buffers/queues are placed in AXI or D2 SRAM. */
    SCB_EnableICache();
    HAL_Init();
    board_clock_init();
    board_gpio_init();
#if defined(REVD_FREERTOS)
    revd_rtos_start();
    for (;;) {}
#else
    app_init();
    storage_sd_init();
    /* A debugger/system reset can occur with POWER released. Resume a normal
     * software boot then; cold power-on still enforces the physical 1 s hold. */
    if(!port_key_down(KEY_POWER)) {
        app_key_raw(KEY_POWER,true);
        app_tick(1000);
    }
    if(!usb_device_init()) board_fatal(4);
    revd_loop_clock_start(HAL_GetTick());
    for(;;) {
        /* Collect completed TX before advancing software deadlines after a
         * long SD/SPI transaction. Both producers remain in this foreground. */
        board_can_task();
        app_capture_poll();
        uint32_t dt=revd_loop_clock_take(HAL_GetTick());
        if(dt) app_tick(dt);
        board_io_task();
        storage_sd_task();
        usb_device_task();
        display_task(HAL_GetTick());
        /* Service events emitted by drivers in this loop iteration. */
        app_tick(0);
        (void)board_time_us();
    }
#endif
}
