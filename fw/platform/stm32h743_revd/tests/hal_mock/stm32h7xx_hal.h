/* Minimal host test doubles for the FT6336 transport, not a firmware HAL. */
#ifndef FT_TEST_HAL_H
#define FT_TEST_HAL_H
#include <stdint.h>
#include <stddef.h>
typedef unsigned HAL_StatusTypeDef;
#define HAL_OK 0u
#define HAL_ERROR 1u
#define RESET 0u
#define I2C_FLAG_BUSY 1u
extern unsigned mock_i2c_busy;
#define __HAL_I2C_GET_FLAG(i, f) (mock_i2c_busy)
typedef struct { unsigned port; } GPIO_TypeDef;
extern GPIO_TypeDef mock_b, mock_d, mock_e;
#define GPIOB (&mock_b)
#define GPIOD (&mock_d)
#define GPIOE (&mock_e)
#define I2C1 ((void *)1)
#define GPIO_PIN_1 0x2u
#define GPIO_PIN_7 0x80u
#define GPIO_PIN_8 0x100u
#define GPIO_PIN_9 0x200u
#define GPIO_MODE_INPUT 0u
#define GPIO_MODE_OUTPUT_PP 1u
#define GPIO_MODE_AF_OD 2u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_LOW 0u
#define GPIO_AF4_I2C1 4u
#define GPIO_PIN_RESET 0u
#define GPIO_PIN_SET 1u
#define I2C_ADDRESSINGMODE_7BIT 0u
#define I2C_DUALADDRESS_DISABLE 0u
#define I2C_OA2_NOMASK 0u
#define I2C_GENERALCALL_DISABLE 0u
#define I2C_NOSTRETCH_DISABLE 0u
#define I2C_ANALOGFILTER_ENABLE 1u
#define I2C_MEMADD_SIZE_8BIT 1u
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOD_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOE_CLK_ENABLE() ((void)0)
#define __HAL_RCC_I2C1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_I2C1_CLK_DISABLE() ((void)0)
typedef struct { uint32_t Pin,Mode,Pull,Speed,Alternate; } GPIO_InitTypeDef;
typedef struct {
    uint32_t Timing,OwnAddress1,AddressingMode,DualAddressMode,OwnAddress2,
             OwnAddress2Masks,GeneralCallMode,NoStretchMode;
} I2C_InitTypeDef;
typedef struct { void *Instance; I2C_InitTypeDef Init; } I2C_HandleTypeDef;
void HAL_GPIO_Init(GPIO_TypeDef *, const GPIO_InitTypeDef *);
void HAL_GPIO_WritePin(GPIO_TypeDef *,uint16_t,unsigned);
void HAL_Delay(uint32_t);
uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *);
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *);
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef *,uint32_t);
HAL_StatusTypeDef HAL_I2CEx_ConfigDigitalFilter(I2C_HandleTypeDef *,uint32_t);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *,uint16_t,uint32_t,uint32_t);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *,uint16_t,uint16_t,uint16_t,uint8_t *,uint16_t,uint32_t);
#endif
