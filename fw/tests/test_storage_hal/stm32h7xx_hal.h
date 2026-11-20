/* Host-only HAL seam. Tests use a RAM card; this header is never in ARM builds. */
#ifndef TEST_STORAGE_HAL_H
#define TEST_STORAGE_HAL_H
#include <stdint.h>
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
typedef enum { GPIO_PIN_RESET, GPIO_PIN_SET } GPIO_PinState;
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef uint32_t HAL_SD_CardStateTypeDef;
typedef struct {
    void *Instance;
    struct { uint32_t ClockEdge, ClockPowerSave, BusWide, HardwareFlowControl, ClockDiv; } Init;
} SD_HandleTypeDef;
typedef struct { uint32_t LogBlockSize, LogBlockNbr; } HAL_SD_CardInfoTypeDef;
extern GPIO_TypeDef mock_gpio_c, mock_gpio_d;
#define GPIOC (&mock_gpio_c)
#define GPIOD (&mock_gpio_d)
#define GPIO_PIN_2 (1u << 2)
#define GPIO_PIN_4 (1u << 4)
#define GPIO_PIN_8 (1u << 8)
#define GPIO_PIN_9 (1u << 9)
#define GPIO_PIN_10 (1u << 10)
#define GPIO_PIN_11 (1u << 11)
#define GPIO_PIN_12 (1u << 12)
#define GPIO_MODE_AF_PP 2u
#define GPIO_MODE_INPUT 0u
#define GPIO_PULLUP 1u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_VERY_HIGH 3u
#define GPIO_AF12_SDMMC1 12u
#define SDMMC1 ((void *)(uintptr_t)0x52007000u)
#define SDMMC_CLOCK_EDGE_RISING 0u
#define SDMMC_CLOCK_POWER_SAVE_DISABLE 0u
#define SDMMC_BUS_WIDE_1B 0u
#define SDMMC_BUS_WIDE_4B 1u
#define SDMMC_HARDWARE_FLOW_CONTROL_DISABLE 0u
#define HAL_SD_CARD_TRANSFER 4u
#define HAL_SD_CARD_ERROR 0xFFu
#define HAL_SD_CARD_DISCONNECTED 8u
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOD_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SDMMC1_CLK_ENABLE() ((void)0)
void HAL_GPIO_Init(GPIO_TypeDef *gpio, const GPIO_InitTypeDef *init);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *gpio, uint16_t pin);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);
void HAL_GPIO_WritePin(GPIO_TypeDef *gpio, uint16_t pin, GPIO_PinState state);
HAL_StatusTypeDef HAL_SD_Init(SD_HandleTypeDef *handle);
HAL_StatusTypeDef HAL_SD_DeInit(SD_HandleTypeDef *handle);
HAL_StatusTypeDef HAL_SD_ConfigWideBusOperation(SD_HandleTypeDef *handle, uint32_t width);
HAL_StatusTypeDef HAL_SD_GetCardInfo(const SD_HandleTypeDef *handle, HAL_SD_CardInfoTypeDef *info);
HAL_SD_CardStateTypeDef HAL_SD_GetCardState(SD_HandleTypeDef *handle);
#define SDMMC1_IRQn 49
#define __DMB() ((void)0)
void HAL_NVIC_SetPriority(int irq, uint32_t preempt, uint32_t sub);
void HAL_NVIC_EnableIRQ(int irq);
void HAL_NVIC_DisableIRQ(int irq);
void HAL_NVIC_ClearPendingIRQ(int irq);
void test_sd_force_reset(void);
#define __HAL_RCC_SDMMC1_FORCE_RESET() test_sd_force_reset()
#define __HAL_RCC_SDMMC1_RELEASE_RESET() ((void)0)
HAL_StatusTypeDef HAL_SD_ReadBlocks_DMA(SD_HandleTypeDef *handle, uint8_t *data,
                                      uint32_t lba, uint32_t count);
HAL_StatusTypeDef HAL_SD_WriteBlocks_DMA(SD_HandleTypeDef *handle, const uint8_t *data,
                                       uint32_t lba, uint32_t count);
HAL_StatusTypeDef HAL_SD_Abort_IT(SD_HandleTypeDef *handle);
void HAL_SD_IRQHandler(SD_HandleTypeDef *handle);
void HAL_SD_TxCpltCallback(SD_HandleTypeDef *handle);
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *handle);
void HAL_SD_ErrorCallback(SD_HandleTypeDef *handle);
#endif
