/* Real ST LL translation unit on RAM-backed CMSIS SDMMC registers. Only CPU
 * context, independent TIM2 clock and the board cooperative hook are doubled. */
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static SDMMC_TypeDef card;
static TIM_TypeDef timer;
static uint32_t tick,ipsr,hooks,timer_reads,timer_step,tick_step;
static uint32_t response_after_hook,response_after_timer;
/* The CMSIS view intentionally marks hardware-owned status as read-only to
 * firmware. The RAM card model writes it as the peripheral would. */
static void hardware_write(volatile const uint32_t *reg,uint32_t value)
{ *(volatile uint32_t *)(uintptr_t)reg=value; }
static uint32_t model_ipsr(void) {return ipsr;}
static TIM_TypeDef *model_timer(void)
{
    timer.CNT+=timer_step;timer_reads++;
    if(response_after_timer && timer_reads==response_after_timer)hardware_write(&card.STA,SDMMC_FLAG_CMDREND);
    return &timer;
}
#undef TIM2
#define TIM2 model_timer()
#define __get_IPSR() model_ipsr()
#include "vendor/stm32_hal/Src/stm32h7xx_ll_sdmmc.c"

uint32_t SystemCoreClock=400000000;
uint32_t HAL_GetTick(void) {return tick;}
void revd_sdmmc_cmd_wait_poll(void)
{
    assert(!ipsr);hooks++;tick+=tick_step;
    if(response_after_hook && hooks==response_after_hook)hardware_write(&card.STA,SDMMC_FLAG_CMDREND);
}
static void reset(void)
{
    memset(&card,0,sizeof card);memset(&timer,0,sizeof timer);
    hardware_write(&card.RESPCMD,13);tick=ipsr=hooks=timer_reads=0;
    timer_step=250;tick_step=1;response_after_hook=response_after_timer=0;
}
static uint32_t run(unsigned response)
{
    uint32_t resp;uint16_t rca;uint8_t data;
    switch(response) {
    case 0:return SDMMC_GetCmdError(&card);
    case 1:return SDMMC_GetCmdResp1(&card,13,10);
    case 2:return SDMMC_GetCmdResp2(&card);
    case 3:return SDMMC_GetCmdResp3(&card);
    case 4:return SDMMC_GetCmdResp4(&card,&resp);
    case 5:return SDMMC_GetCmdResp5(&card,13,&data);
    case 6:return SDMMC_GetCmdResp6(&card,13,&rca);
    case 7:return SDMMC_GetCmdResp7(&card);
    default:assert(0);return 0;
    }
}
static void test_response_and_original_errors(void)
{
    for(unsigned response=0;response<8;response++) {
        reset();hardware_write(&card.STA,response?SDMMC_FLAG_CMDREND:SDMMC_FLAG_CMDSENT);
        assert(run(response)==SDMMC_ERROR_NONE&&!hooks);
    }
    reset();hardware_write(&card.STA,SDMMC_FLAG_CTIMEOUT);
    assert(run(1)==SDMMC_ERROR_CMD_RSP_TIMEOUT&&!hooks);
    reset();hardware_write(&card.STA,SDMMC_FLAG_CCRCFAIL);
    assert(run(1)==SDMMC_ERROR_CMD_CRC_FAIL&&!hooks);
    reset();hardware_write(&card.STA,SDMMC_FLAG_CMDREND);hardware_write(&card.RESPCMD,12);
    assert(run(1)==SDMMC_ERROR_CMD_CRC_FAIL&&!hooks);
    reset();hardware_write(&card.STA,SDMMC_FLAG_CMDREND);hardware_write(&card.RESP1,SDMMC_OCR_ADDR_OUT_OF_RANGE);
    assert(run(1)==SDMMC_ERROR_ADDR_OUT_OF_RANGE&&!hooks);
}
static void test_all_thread_timeouts_and_hook(void)
{
    for(unsigned response=0;response<8;response++) {
        reset();tick_step=1000;
        assert(run(response)==SDMMC_ERROR_TIMEOUT);
        assert(hooks==(response==1?1u:5u));
    }
    reset();response_after_hook=3;
    assert(run(1)==SDMMC_ERROR_NONE&&hooks==3);
    reset();tick=UINT32_MAX-4;
    assert(run(1)==SDMMC_ERROR_TIMEOUT&&hooks==10&&tick==5);
    /* Hook work is included in the real timeout; it cannot extend the old
     * instruction-count deadline by 400 ms on each cooperative poll. */
    reset();tick_step=400;
    assert(SDMMC_GetCmdResp1(&card,13,5000)==SDMMC_ERROR_TIMEOUT&&hooks==13);
}
static void test_irq_deadline_no_application_hook(void)
{
    for(unsigned response=0;response<8;response++) {
        reset();ipsr=65;timer.CNT=UINT32_MAX-500;
        assert(run(response)==SDMMC_ERROR_TIMEOUT);
        assert(!hooks&&timer_reads==5); /* 1 start read + 4 x 250 us polls. */
    }
    reset();ipsr=65;response_after_timer=2;
    assert(run(1)==SDMMC_ERROR_NONE&&!hooks&&timer_reads==2);
}
int main(void)
{
    test_response_and_original_errors();test_all_thread_timeouts_and_hook();
    test_irq_deadline_no_application_hook();
    puts("SDMMC LL: real vendor translation unit, all 8 response loops + thread/IRQ/wrap/CRC/timeout models passed");
    return 0;
}
