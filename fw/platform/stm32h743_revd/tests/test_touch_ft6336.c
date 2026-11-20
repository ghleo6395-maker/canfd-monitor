#include "stm32h7xx_hal.h"
#include "../touch_ft6336.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
GPIO_TypeDef mock_b={1},mock_d={2},mock_e={3};
static GPIO_InitTypeDef last_pb;
static uint8_t packet[5], id=0x64;
static bool io_error;
static unsigned reads;
unsigned mock_i2c_busy;
void HAL_GPIO_Init(GPIO_TypeDef *p,const GPIO_InitTypeDef *g){if(p==GPIOB)last_pb=*g;}
void HAL_GPIO_WritePin(GPIO_TypeDef *p,uint16_t pin,unsigned state){(void)p;(void)pin;(void)state;}
void HAL_Delay(uint32_t ms){(void)ms;}
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *i){assert(i->Init.Timing==0x90423031u);return HAL_OK;}
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *i){(void)i;return HAL_OK;}
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef *i,uint32_t f){(void)i;assert(f==1);return HAL_OK;}
HAL_StatusTypeDef HAL_I2CEx_ConfigDigitalFilter(I2C_HandleTypeDef *i,uint32_t f){(void)i;assert(f==0);return HAL_OK;}
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *i,uint16_t a,uint32_t trials,uint32_t timeout)
{(void)i;assert(a==0x70&&trials==1&&timeout==3);return HAL_OK;}
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
int main(void)
{
    assert(touch_ft6336_init());
    assert(last_pb.Pin==(GPIO_PIN_8|GPIO_PIN_9));
    assert(last_pb.Mode==GPIO_MODE_AF_OD&&last_pb.Pull==GPIO_NOPULL&&last_pb.Alternate==4);
    uint16_t x=999,y=999;bool pressed=true;
    point(0,0,0,1);assert(touch_ft6336_read(&x,&y,&pressed)&&pressed&&x==0&&y==319);
    point(319,479,2,1);assert(touch_ft6336_read(&x,&y,&pressed)&&pressed&&x==479&&y==0);
    point(100,200,1,1);assert(touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    point(100,200,2,0);assert(touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    point(100,200,2,3);assert(touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    point(320,200,2,1);assert(!touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    point(100,480,2,1);assert(!touch_ft6336_read(&x,&y,&pressed)&&!pressed);
    io_error=true;assert(!touch_ft6336_read(&x,&y,&pressed)&&!pressed);io_error=false;
    mock_i2c_busy=1;unsigned stuck_before=reads;
    assert(!touch_ft6336_read(&x,&y,&pressed)&&!pressed&&reads==stuck_before);
    mock_i2c_busy=0;
    assert(touch_ft6336_errors()==4);
    touch_ft6336_deinit();assert(last_pb.Mode==GPIO_MODE_INPUT&&last_pb.Pull==GPIO_NOPULL);
    unsigned before=reads;assert(!touch_ft6336_read(&x,&y,&pressed)&&reads==before);
    id=0xFF;assert(!touch_ft6336_init());assert(last_pb.Mode==GPIO_MODE_INPUT);
    puts("FT6336: probe, OD/NOPULL, corner transforms, lift, malformed points, I/O errors, safe deinit passed");
    return 0;
}
