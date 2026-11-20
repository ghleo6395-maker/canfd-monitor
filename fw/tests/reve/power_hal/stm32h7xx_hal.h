#ifndef TEST_REVE_POWER_HAL_H
#define TEST_REVE_POWER_HAL_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { volatile uint32_t IDR, BSRR; } GPIO_TypeDef;
extern GPIO_TypeDef port_b,port_d,port_e;
#define GPIOB (&port_b)
#define GPIOD (&port_d)
#define GPIOE (&port_e)
#define GPIO_PIN_0 (1u<<0)
#define GPIO_PIN_4 (1u<<4)
#define GPIO_PIN_6 (1u<<6)
#define GPIO_PIN_7 (1u<<7)
#define GPIO_PIN_11 (1u<<11)
#define GPIO_PIN_12 (1u<<12)
#define GPIO_MODE_AF_OD 0
#define GPIO_NOPULL 0
#define GPIO_SPEED_FREQ_LOW 0
#define GPIO_AF6_I2C4 6
#define ADC_CHANNEL_4 4
#define ADC_CHANNEL_8 8
#define I2C4 ((void*)4)
#define I2C_ADDRESSINGMODE_7BIT 0
#define I2C_DUALADDRESS_DISABLE 0
#define I2C_GENERALCALL_DISABLE 0
#define I2C_NOSTRETCH_DISABLE 0
#define I2C_MEMADD_SIZE_8BIT 1
#define HAL_OK 0
#define RESET 0
#define I2C_FLAG_BUSY 1
extern bool mock_monitor_online,mock_monitor_busy;
extern unsigned mock_monitor_reads;
extern int16_t mock_shunt_raw;
#define __HAL_I2C_GET_FLAG(h,f) (mock_monitor_busy)
#define __HAL_RCC_I2C4_CLK_ENABLE() ((void)0)
typedef struct { unsigned Pin,Mode,Pull,Speed,Alternate; } GPIO_InitTypeDef;
typedef struct { void *Instance; struct { unsigned Timing,AddressingMode,DualAddressMode,GeneralCallMode,NoStretchMode; } Init; } I2C_HandleTypeDef;
extern uint32_t mock_irq_mask,mock_tick;
static inline uint32_t __get_PRIMASK(void){ return mock_irq_mask; }
static inline void __disable_irq(void){ mock_irq_mask=1; }
static inline void __set_PRIMASK(uint32_t m){ mock_irq_mask=m; }
static inline uint32_t HAL_GetTick(void){return mock_tick;}
static inline void HAL_GPIO_Init(GPIO_TypeDef*p,GPIO_InitTypeDef*g){(void)p;(void)g;}
static inline int HAL_I2C_Init(I2C_HandleTypeDef*p){(void)p;return mock_monitor_online?HAL_OK:1;}
static inline int HAL_I2C_Mem_Read(I2C_HandleTypeDef*p,unsigned a,unsigned r,unsigned s,uint8_t*b,unsigned n,unsigned t){
    ++mock_monitor_reads;
    if(!mock_monitor_online || mock_monitor_busy) return 1;
    uint16_t v=r==0xfe?0x5449:r==0xff?0x2260:r==1?(uint16_t)mock_shunt_raw:r==2?3000:0;
    b[0]=(uint8_t)(v>>8);b[1]=(uint8_t)v;return HAL_OK;
}
static inline int HAL_I2C_Mem_Write(I2C_HandleTypeDef*p,unsigned a,unsigned r,unsigned s,uint8_t*b,unsigned n,unsigned t){return mock_monitor_online&&!mock_monitor_busy?HAL_OK:1;}
#endif
