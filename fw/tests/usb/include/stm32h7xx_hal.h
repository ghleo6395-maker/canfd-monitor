/* Test-only HAL facade. Firmware is separately ARM-compiled against real HAL. */
#ifndef TEST_HAL_H
#define TEST_HAL_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
typedef struct { uint32_t DSTS; } USB_OTG_DeviceTypeDef;
extern USB_OTG_DeviceTypeDef test_usb_regs;
#define USB2_OTG_FS_PERIPH_BASE ((uintptr_t)&test_usb_regs)
#define USB_OTG_DEVICE_BASE 0u
#define USB_OTG_DSTS_FNSOF_Msk 0x3fff00u
#define USB_OTG_DSTS_FNSOF_Pos 8u
#define GPIO_PIN_11 (1u<<11)
#define GPIO_PIN_12 (1u<<12)
#define GPIO_MODE_AF_PP 2u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_VERY_HIGH 3u
#define GPIOA NULL
#define OTG_FS_IRQn 67
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_USB2_OTG_FS_CLK_ENABLE() ((void)0)
uint32_t HAL_GetTick(void);
void HAL_GPIO_Init(void *port, GPIO_InitTypeDef *gpio);
void HAL_PWREx_EnableUSBVoltageDetector(void);
void NVIC_SetPriority(int irq, unsigned pri);
#endif
