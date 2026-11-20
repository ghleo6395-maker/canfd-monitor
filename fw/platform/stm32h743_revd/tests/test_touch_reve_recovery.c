#include "stm32h7xx_hal.h"
#include "../touch_ft6336.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
GPIO_TypeDef mock_b={1},mock_d={2},mock_e={3};
static GPIO_InitTypeDef last_pb;
static uint8_t packet[5], id=0x64;
static bool io_error;
static unsigned reads, probes, writes, delay_calls, checks;
static uint32_t tick;
uint32_t HAL_GetTick(void){return tick;}
unsigned mock_i2c_busy;
void HAL_GPIO_Init(GPIO_TypeDef *p,const GPIO_InitTypeDef *g){if(p==GPIOB)last_pb=*g;}
void HAL_GPIO_WritePin(GPIO_TypeDef *p,uint16_t pin,unsigned state){(void)p;(void)pin;(void)state;writes++;}
void HAL_Delay(uint32_t ms){tick+=ms;delay_calls++;}
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *i){assert(i->Init.Timing==0x90423031u);return HAL_OK;}
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *i){(void)i;return HAL_OK;}
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef *i,uint32_t f){(void)i;assert(f==1);return HAL_OK;}
HAL_StatusTypeDef HAL_I2CEx_ConfigDigitalFilter(I2C_HandleTypeDef *i,uint32_t f){(void)i;assert(f==0);return HAL_OK;}
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *i,uint16_t a,uint32_t trials,uint32_t timeout)
{(void)i;assert(a==0x70&&trials==1&&timeout==3);probes++;return io_error?HAL_ERROR:HAL_OK;}
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *i,uint16_t a,uint16_t reg,uint16_t regsz,uint8_t *out,uint16_t n,uint32_t timeout)
{
    (void)i; assert(a==0x70&&regsz==1&&timeout==3); reads++;
    if(io_error)return HAL_ERROR;
    if(reg==0xA3){assert(n==1);out[0]=id;}
    else {assert(reg==2&&n==5);memcpy(out,packet,n);}
    return HAL_OK;
}
static void point(unsigned x,unsigned y,unsigned event,unsigned count)
{
    packet[0]=(uint8_t)count;packet[1]=(uint8_t)((event<<6)|(x>>8));
    packet[2]=(uint8_t)x;packet[3]=(uint8_t)(y>>8);packet[4]=(uint8_t)y;
}
#define CHECK(c) do { assert(c); checks++; } while (0)
static uint16_t x,y;
static bool pressed;
static bool service(uint32_t t){tick=t;return touch_ft6336_service(t);}
static void fail_three(void)
{
    io_error=true;
    for(unsigned i=0;i<3;i++)CHECK(!touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    io_error=false;
}
static void recover(uint32_t start)
{
    unsigned delays=delay_calls, before=probes;
    CHECK(!service(start));
    CHECK(!service(start+9));
    CHECK(!service(start+10));
    CHECK(!service(start+209));
    CHECK(probes==before);
    CHECK(!service(start+210));
    CHECK(!service(start+211));
    CHECK(service(start+212));
    CHECK(delay_calls==delays);
    CHECK(last_pb.Mode==GPIO_MODE_AF_OD&&last_pb.Pull==GPIO_NOPULL);
}
int main(void)
{
    CHECK(touch_ft6336_init());
    point(100,200,0,1);CHECK(touch_ft6336_read(&x,&y,&pressed)&&pressed);
    tick=1000;fail_three();
    unsigned reads_before=reads,writes_before=writes;
    CHECK(!touch_ft6336_read(&x,&y,&pressed)&&reads==reads_before);
    CHECK(!service(1249)&&writes==writes_before);
    recover(1250);
    /* A held finger across reset cannot click a dialog on reconnection. */
    CHECK(touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    CHECK(touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    point(0,0,0,0);CHECK(touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    point(100,200,0,1);CHECK(touch_ft6336_read(&x,&y,&pressed)&&pressed&&x==200&&y==219);
    /* One successful packet breaks a run of failures. */
    for(unsigned i=0;i<2;i++){
        io_error=true;CHECK(!touch_ft6336_read(&x,&y,&pressed));io_error=false;
        CHECK(touch_ft6336_read(&x,&y,&pressed)&&!pressed);
        point(0,0,0,0);CHECK(touch_ft6336_read(&x,&y,&pressed));
        point(100,200,0,1);
        CHECK(touch_ft6336_read(&x,&y,&pressed)&&pressed);
    }
    CHECK(service(tick));
    /* Stuck BUSY never reaches HAL's hidden 25 ms initial wait, including probe. */
    tick=3000;mock_i2c_busy=1;reads_before=reads;
    for(unsigned i=0;i<3;i++)CHECK(!touch_ft6336_read(&x,&y,&pressed));
    CHECK(reads==reads_before);
    CHECK(!service(3250));CHECK(!service(3260));CHECK(!service(3460));
    unsigned probes_before=probes;
    CHECK(!service(3461)&&probes==probes_before);
    writes_before=writes;CHECK(!service(5460)&&writes==writes_before);
    mock_i2c_busy=0;recover(5461);
    /* Timer wrap is handled with signed deadline differences. */
    tick=UINT32_MAX-100;fail_three();
    writes_before=writes;CHECK(!service(148)&&writes==writes_before);recover(149);
    /* Poweroff cancels pending recovery and leaves pins high impedance. */
    touch_ft6336_deinit();writes_before=writes;reads_before=reads;
    CHECK(!service(tick+10000)&&writes==writes_before&&reads==reads_before);
    CHECK(last_pb.Mode==GPIO_MODE_INPUT&&last_pb.Pull==GPIO_NOPULL);
    /* A module absent at startup can recover later; wrong ID backs off. */
    id=0xff;CHECK(!touch_ft6336_init());uint32_t due=tick+2000;
    CHECK(!service(due));CHECK(!service(due+10));CHECK(!service(due+210));
    CHECK(!service(due+211));CHECK(!service(due+212));
    id=0x64;recover(due+2212);
    CHECK(touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    point(0,0,0,0);CHECK(touch_ft6336_read(&x,&y,&pressed));
    point(319,479,2,1);CHECK(touch_ft6336_read(&x,&y,&pressed)&&pressed&&x==479&&y==0);
    printf("Rev E touch recovery: %u checks passed (runtime delays, BUSY, backoff, wrap, held touch, poweroff, reconnect)\n",checks);
    return 0;
}
