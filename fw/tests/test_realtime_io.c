/* Production cooperative hook must stay ordered and reject nested work. */
#include "platform/stm32h743_revd/realtime_io.h"
#include "services/core/port.h"
#include <assert.h>
#include <stdio.h>
static unsigned phase, ticks, total, clock_advanced;
static bool drain;
static uint32_t ipsr;
uint32_t __get_IPSR(void) { return ipsr; }
uint32_t HAL_GetTick(void) { return 42; }
void board_can_task(void) { assert(phase==0); phase=1; ++total; }
void app_capture_poll(void) { assert(phase==1); phase=drain?0:2; }
void app_realtime_poll(uint32_t ms) {clock_advanced+=ms;app_capture_poll();}
void display_dma_poll(uint32_t t) { assert(t==42 && phase==2); phase=3; revd_io_wait_poll(); }
void usb_device_poll_transport(void) { assert(phase==3); phase=0; ++ticks; revd_io_wait_poll(); }
int main(void)
{
    assert(revd_loop_clock_take(100)==0);
    revd_loop_clock_start(0xFFFFFFFDu);
    assert(revd_loop_clock_take(0xFFFFFFFFu)==2);
    assert(revd_loop_clock_take(2)==3);
    assert(revd_loop_clock_take(2)==0);
    revd_loop_clock_start(40);
    for(unsigned i=0;i<1000;i++) revd_io_wait_poll();
    assert(total==1000 && ticks==1000 && phase==0);
    assert(clock_advanced==2); /* 1,000 waits at the same HAL tick count once. */
    revd_sdmmc_cmd_wait_poll();
    assert(total==1001 && ticks==1001);
    drain=true; port_can_flush_rx();
    assert(total==1010 && ticks==1001 && phase==0);
    ipsr=65; revd_io_wait_poll(); revd_sdmmc_cmd_wait_poll();
    assert(total==1010 && ticks==1001);
    puts("real-time cooperative hook: 1001 ordered calls + 9 tail-drain batches; nested/ISR calls suppressed");
}
