/* Exercise the actual Rev E board power task with physical input changes during
 * ADC and stalled foreground execution. It is not electrical/USB compliance. */
#include "stm32h7xx_hal.h"
#include "power_reve.h"
#include <assert.h>
#include <stdio.h>
GPIO_TypeDef port_b,port_d,port_e;
uint32_t mock_irq_mask,mock_tick;
bool mock_monitor_online,mock_monitor_busy;
unsigned mock_monitor_reads;
int16_t mock_shunt_raw;
static bool drop_during_adc,adc_valid=true;
static uint16_t cc1=1000,cc2;
static unsigned checks;
#define C(x) do { ++checks; assert(x); } while(0)
bool board_reve_adc_mv(uint32_t ch,uint16_t *v)
{
    if(drop_during_adc) { port_e.IDR=GPIO_PIN_0; reve_power_tick(); drop_during_adc=false; }
    *v=ch==ADC_CHANNEL_4?cc1:cc2;return adc_valid;
}
static input_mode_t mode(void){return reve_power_diagnostics()->mode;}
static void step(uint32_t n){mock_tick=n;reve_power_tick();reve_power_task();C(mock_irq_mask==0);}
int main(void)
{
    port_d.IDR=GPIO_PIN_12; /* no 12V; USB physically attached */
    reve_power_init();step(0);C(mode()==INPUT_USB_100);
    /* A 140ms foreground stall cannot count as continuous CC qualification. */
    step(140);C(mode()==INPUT_USB_100);step(150);C(mode()==INPUT_USB_100);
    for(uint32_t n=160;n<=290;n+=10)step(n);
    C(mode()==INPUT_TYPEC);
    mock_tick=321;reve_power_tick();C(mode()==INPUT_USB_100);
    C(reve_power_diagnostics()->stale_fallbacks==1);
    for(uint32_t n=330;n<=480;n+=10)step(n);
    C(mode()==INPUT_TYPEC);
    drop_during_adc=true;step(490);C(mode()==INPUT_NONE); /* recheck after ADC */
    port_e.IDR=0;cc1=500;reve_power_usb_configured(true);step(500);C(mode()==INPUT_USB_500);
    reve_power_usb_suspend(true);C(mode()==INPUT_SUSPEND);
    reve_power_usb_reset();C(mode()==INPUT_USB_100);
    adc_valid=false;step(510);C(mode()==INPUT_USB_100);
    port_d.IDR=0;step(520);C(mode()==INPUT_EXTERNAL);
    port_e.IDR=GPIO_PIN_0;step(530);C(mode()==INPUT_EXTERNAL);
    port_d.IDR=GPIO_PIN_12;step(540);C(mode()==INPUT_NONE);
    mock_irq_mask=1;reve_power_usb_reset();C(mock_irq_mask==1);mock_irq_mask=0;
    mock_monitor_online=true;mock_shunt_raw=1600;step(800);
    C(reve_power_diagnostics()->monitor_valid);
    C(reve_power_diagnostics()->battery_ma==400);
    C(reve_power_diagnostics()->cell_mv==3750);
    mock_shunt_raw=-1200;step(1050);
    C(reve_power_diagnostics()->battery_ma==-300);
    unsigned reads=mock_monitor_reads;
    mock_monitor_busy=true;step(1300);
    C(!reve_power_diagnostics()->monitor_valid);
    C(mock_monitor_reads==reads); /* Never enter HAL's 25 ms BUSY wait. */
    mock_monitor_busy=false;step(1550);
    C(reve_power_diagnostics()->monitor_valid);
    printf("Rev E actual power task: %u checks passed (GPIO/ADC timing model, not hardware)\n",checks);
}
