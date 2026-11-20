/* SPDX-License-Identifier: MIT */
#ifdef CANFD_REVE_TARGET
#include "board.h"
#include "power_reve.h"
#include <string.h>
static input_policy_t policy;
static reve_power_diag_t diag;
static I2C_HandleTypeDef monitor;
static bool i2c_ready;
static volatile bool ready, configured, suspended, expired;
static volatile uint32_t sampled_at;
static volatile input_mode_t mode;
static uint32_t last_sample, last_monitor;
static bool external(void) { return (GPIOD->IDR & GPIO_PIN_12)==0; }
bool reve_power_vbus(void) { return (GPIOE->IDR & GPIO_PIN_0)==0; }
const reve_power_diag_t *reve_power_diagnostics(void) { return &diag; }
static void apply(input_mode_t next)
{
    uint32_t irq_mask=__get_PRIMASK(); __disable_irq();
    if(next==mode) { __set_PRIMASK(irq_mask); return; }
    /* Safe sequencing: drop EN2 first, then EN1. Going to ILIM clears EN1
     * before raising EN2; going to suspend raises EN1 before EN2. External
     * input forces EN2 high/EN1 low in hardware independently of firmware. */
    GPIOE->BSRR=(uint32_t)GPIO_PIN_4<<16;
    GPIOD->BSRR=(uint32_t)GPIO_PIN_11<<16;
    if(next==INPUT_USB_500 || next==INPUT_SUSPEND) GPIOD->BSRR=GPIO_PIN_11;
    if(next==INPUT_TYPEC || next==INPUT_SUSPEND) GPIOE->BSRR=GPIO_PIN_4;
    mode=next; diag.mode=next;
    __set_PRIMASK(irq_mask);
}
void reve_power_usb_configured(bool on) { configured=on; if(!on) reve_power_usb_reset(); }
void reve_power_usb_suspend(bool on)
{
    suspended=on;
    if(on && ready && !external() && mode!=INPUT_TYPEC) apply(INPUT_SUSPEND);
}
void reve_power_usb_reset(void)
{
    configured=false; suspended=false; expired=true;
    if(ready && !external()) apply(reve_power_vbus()?INPUT_USB_100:INPUT_NONE);
}
void reve_power_tick(void)
{
    if(!ready) return;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    bool vbus=reve_power_vbus(), ext=external();
    if(!vbus) configured=false;
    uint32_t age=HAL_GetTick()-sampled_at;
    input_mode_t safe=input_policy_expire(mode,vbus,ext,suspended,age);
    /* A stalled foreground must also break a not-yet-qualified CC interval,
     * even when it was already in USB100 and no GPIO mode change is needed. */
    if(age>30u || !vbus) expired=true;
    if(safe!=mode) {
        if(age>30u && (mode==INPUT_TYPEC || mode==INPUT_USB_500)) ++diag.stale_fallbacks;
        expired=true; apply(safe);
    }
    __set_PRIMASK(mask);
}
static bool reg_read(uint8_t reg, uint16_t *value)
{
    uint8_t b[2];
    /* HAL's initial BUSY wait is 25 ms regardless of the 2 ms transfer
     * timeout. A stuck monitor must not stall the foreground CAN task. */
    if(__HAL_I2C_GET_FLAG(&monitor,I2C_FLAG_BUSY)!=RESET) return false;
    if(HAL_I2C_Mem_Read(&monitor,0x40u<<1,reg,I2C_MEMADD_SIZE_8BIT,b,2,2)!=HAL_OK) return false;
    *value=(uint16_t)((uint16_t)b[0]<<8)|b[1]; return true;
}
static bool monitor_start(void)
{
    if(HAL_I2C_Init(&monitor)!=HAL_OK) return false;
    uint16_t mfr, die;
    if(!reg_read(0xfe,&mfr) || mfr!=0x5449u || !reg_read(0xff,&die) || (die&0xfff0u)!=0x2260u) return false;
    /* 4 averages, 1.1ms bus/shunt conversion, continuous: ~8.8ms cycle. */
    uint8_t cfg[2]={0x43,0x27};
    if(__HAL_I2C_GET_FLAG(&monitor,I2C_FLAG_BUSY)!=RESET) return false;
    return HAL_I2C_Mem_Write(&monitor,0x40u<<1,0,I2C_MEMADD_SIZE_8BIT,cfg,2,2)==HAL_OK;
}
void reve_power_init(void)
{
    __HAL_RCC_I2C4_CLK_ENABLE();
    GPIO_InitTypeDef p={.Pin=GPIO_PIN_6|GPIO_PIN_7,.Mode=GPIO_MODE_AF_OD,
      .Pull=GPIO_NOPULL,.Speed=GPIO_SPEED_FREQ_LOW,.Alternate=GPIO_AF6_I2C4};
    HAL_GPIO_Init(GPIOB,&p);
    monitor.Instance=I2C4; monitor.Init.Timing=0x90423031u;
    monitor.Init.AddressingMode=I2C_ADDRESSINGMODE_7BIT;
    monitor.Init.DualAddressMode=I2C_DUALADDRESS_DISABLE;
    monitor.Init.GeneralCallMode=I2C_GENERALCALL_DISABLE;
    monitor.Init.NoStretchMode=I2C_NOSTRETCH_DISABLE;
    i2c_ready=monitor_start();
    sampled_at=HAL_GetTick(); last_sample=sampled_at-10u; last_monitor=sampled_at;
    apply(INPUT_NONE); ready=true;
}
void reve_power_task(void)
{
    uint32_t now=HAL_GetTick();
    if(!ready || (uint32_t)(now-last_sample)<10u) return;
    last_sample=now;
    input_sample_t s={.vbus=reve_power_vbus(),.external=external()};
    s.cc_valid=board_reve_adc_mv(ADC_CHANNEL_4,&s.cc1_mv) && board_reve_adc_mv(ADC_CHANNEL_8,&s.cc2_mv);
    /* Exclude IRQ interleaving while publishing a fresh qualification. */
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    if(expired) { policy.qualifying=false; expired=false; }
    s.vbus=reve_power_vbus(); s.external=external();
    s.configured=configured; s.suspended=suspended;
    input_mode_t next=input_policy_step(&policy,&s,now);
    sampled_at=HAL_GetTick(); apply(next);
    __set_PRIMASK(mask);
    diag.mode=mode; diag.cc1_mv=s.cc1_mv; diag.cc2_mv=s.cc2_mv; diag.cc_valid=s.cc_valid;
    if((uint32_t)(now-last_monitor)>=250u) {
        last_monitor=now;
        uint16_t shunt=0,bus=0;
        if(!i2c_ready) i2c_ready=monitor_start();
        diag.monitor_valid=i2c_ready && reg_read(1,&shunt) && reg_read(2,&bus);
        if(diag.monitor_valid) {
            diag.battery_ma=(int32_t)(int16_t)shunt/4; /* 2.5uV / 10mOhm */
            diag.cell_mv=(uint16_t)((uint32_t)bus*5u/4u);
        } else { ++diag.monitor_errors; i2c_ready=false; }
    }
}
#endif
