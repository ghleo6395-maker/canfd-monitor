/* Test-only HAL facade; ARM target separately uses the unchanged ST HAL. */
#ifndef DISPLAY_TEST_HAL_H
#define DISPLAY_TEST_HAL_H
#include <stdint.h>
#include <stddef.h>
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t IER, CFG1; } SPI_TypeDef;
typedef struct { int dummy; } TIM_TypeDef;
typedef struct { int dummy; } DMA_Stream_TypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
typedef int GPIO_PinState;
typedef struct { uint32_t BaudRatePrescaler; uint32_t CLKPhase; uint32_t CLKPolarity; uint32_t CRCCalculation; uint32_t CRCPolynomial; uint32_t DataSize; uint32_t Direction; uint32_t FifoThreshold; uint32_t FirstBit; uint32_t IOSwap; uint32_t MasterInterDataIdleness; uint32_t MasterKeepIOState; uint32_t MasterReceiverAutoSusp; uint32_t MasterSSIdleness; uint32_t Mode; uint32_t NSS; uint32_t NSSPMode; uint32_t NSSPolarity; uint32_t RxCRCInitializationPattern; uint32_t TIMode; uint32_t TxCRCInitializationPattern; } SPI_InitTypeDef;
typedef struct { uint32_t Direction; uint32_t FIFOMode; uint32_t FIFOThreshold; uint32_t MemBurst; uint32_t MemDataAlignment; uint32_t MemInc; uint32_t Mode; uint32_t PeriphBurst; uint32_t PeriphDataAlignment; uint32_t PeriphInc; uint32_t Priority; uint32_t Request; } DMA_InitTypeDef;
typedef struct { uint32_t AutoReloadPreload; uint32_t ClockDivision; uint32_t CounterMode; uint32_t Period; uint32_t Prescaler; uint32_t RepetitionCounter; } TIM_Base_InitTypeDef;
typedef struct { uint32_t OCFastMode; uint32_t OCIdleState; uint32_t OCMode; uint32_t OCNIdleState; uint32_t OCNPolarity; uint32_t OCPolarity; uint32_t Pulse; } TIM_OC_InitTypeDef;
typedef struct DMA_HandleTypeDef { DMA_Stream_TypeDef *Instance; DMA_InitTypeDef Init; void *Parent; } DMA_HandleTypeDef;
typedef struct { SPI_TypeDef *Instance; SPI_InitTypeDef Init; DMA_HandleTypeDef *hdmatx; } SPI_HandleTypeDef;
typedef struct { TIM_TypeDef *Instance; TIM_Base_InitTypeDef Init; } TIM_HandleTypeDef;
extern SPI_TypeDef test_spi4;
extern TIM_TypeDef test_tim1;
extern DMA_Stream_TypeDef test_stream0;
extern uint32_t test_compare;
#define SPI4 (&test_spi4)
#define TIM1 (&test_tim1)
#define DMA1_Stream0 (&test_stream0)
#define GPIOE NULL
#define GPIOD ((void *)1)
#define SPI4_IRQn 84
#define DMA1_Stream0_IRQn 11
#define __HAL_LINKDMA(h, field, d) do { (h)->field = &(d); (d).Parent = (h); } while (0)
#define __HAL_TIM_SET_COMPARE(h,c,v) ((void)(h), (void)(c), test_compare = (v))
#define CLEAR_BIT(reg, bits) ((reg) &= ~(bits))
#define GPIO_PIN_0 (1u << 0)
#define GPIO_PIN_1 (1u << 1)
#define GPIO_PIN_2 (1u << 2)
#define GPIO_PIN_3 (1u << 3)
#define GPIO_PIN_4 (1u << 4)
#define GPIO_PIN_5 (1u << 5)
#define GPIO_PIN_6 (1u << 6)
#define GPIO_PIN_7 (1u << 7)
#define GPIO_PIN_8 (1u << 8)
#define GPIO_PIN_9 (1u << 9)
#define GPIO_PIN_10 (1u << 10)
#define GPIO_PIN_11 (1u << 11)
#define GPIO_PIN_12 (1u << 12)
#define GPIO_PIN_13 (1u << 13)
#define GPIO_PIN_14 (1u << 14)
#define GPIO_PIN_15 (1u << 15)
#define DMA_FIFOMODE_DISABLE 1u
#define DMA_FIFO_THRESHOLD_FULL 2u
#define DMA_MBURST_SINGLE 3u
#define DMA_MDATAALIGN_BYTE 4u
#define DMA_MEMORY_TO_PERIPH 5u
#define DMA_MINC_ENABLE 6u
#define DMA_NORMAL 7u
#define DMA_PBURST_SINGLE 8u
#define DMA_PDATAALIGN_BYTE 9u
#define DMA_PINC_DISABLE 10u
#define DMA_PRIORITY_LOW 11u
#define DMA_REQUEST_SPI4_TX 12u
#define GPIO_AF1_TIM1 13u
#define GPIO_AF5_SPI4 14u
#define GPIO_MODE_AF_PP 15u
#define GPIO_MODE_INPUT 16u
#define GPIO_MODE_OUTPUT_PP 17u
#define GPIO_NOPULL 18u
#define GPIO_PIN_RESET 26u
#define GPIO_PIN_SET 27u
#define GPIO_SPEED_FREQ_LOW 28u
#define GPIO_SPEED_FREQ_VERY_HIGH 29u
#define SPI_BAUDRATEPRESCALER_8 30u
#define SPI_CFG1_RXDMAEN 31u
#define SPI_CFG1_TXDMAEN 32u
#define SPI_CRCCALCULATION_DISABLE 33u
#define SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN 34u
#define SPI_DATASIZE_8BIT 35u
#define SPI_DIRECTION_2LINES 36u
#define SPI_FIFO_THRESHOLD_01DATA 37u
#define SPI_FIRSTBIT_MSB 38u
#define SPI_IO_SWAP_DISABLE 39u
#define SPI_MASTER_INTERDATA_IDLENESS_00CYCLE 40u
#define SPI_MASTER_KEEP_IO_STATE_DISABLE 41u
#define SPI_MASTER_RX_AUTOSUSP_DISABLE 42u
#define SPI_MASTER_SS_IDLENESS_00CYCLE 43u
#define SPI_MODE_MASTER 44u
#define SPI_NSS_POLARITY_LOW 45u
#define SPI_NSS_PULSE_DISABLE 46u
#define SPI_NSS_SOFT 47u
#define SPI_PHASE_1EDGE 48u
#define SPI_POLARITY_LOW 49u
#define SPI_TIMODE_DISABLE 50u
#define TIM_AUTORELOAD_PRELOAD_DISABLE 51u
#define TIM_CHANNEL_1 52u
#define TIM_CLOCKDIVISION_DIV1 53u
#define TIM_COUNTERMODE_UP 54u
#define TIM_OCFAST_DISABLE 55u
#define TIM_OCIDLESTATE_RESET 56u
#define TIM_OCMODE_PWM1 57u
#define TIM_OCNIDLESTATE_RESET 58u
#define TIM_OCNPOLARITY_HIGH 59u
#define TIM_OCPOLARITY_HIGH 60u
#define __HAL_RCC_DMA1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOE_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SPI4_CLK_DISABLE() ((void)0)
#define __HAL_RCC_SPI4_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SPI4_FORCE_RESET() ((void)0)
#define __HAL_RCC_SPI4_RELEASE_RESET() ((void)0)
#define __HAL_RCC_TIM1_CLK_DISABLE() ((void)0)
#define __HAL_RCC_TIM1_CLK_ENABLE() ((void)0)
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);
void HAL_GPIO_WritePin(void *, uint32_t, GPIO_PinState);
void HAL_GPIO_Init(void *, GPIO_InitTypeDef *);
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *);
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *, const uint8_t *, uint16_t);
void HAL_SPI_IRQHandler(SPI_HandleTypeDef *);
HAL_StatusTypeDef HAL_DMA_Init(DMA_HandleTypeDef *);
HAL_StatusTypeDef HAL_DMA_DeInit(DMA_HandleTypeDef *);
void HAL_DMA_IRQHandler(DMA_HandleTypeDef *);
void HAL_NVIC_SetPriority(int, unsigned, unsigned);
void HAL_NVIC_ClearPendingIRQ(int);
void HAL_NVIC_EnableIRQ(int);
void HAL_NVIC_DisableIRQ(int);
HAL_StatusTypeDef HAL_TIM_PWM_Init(TIM_HandleTypeDef *);
HAL_StatusTypeDef HAL_TIM_PWM_ConfigChannel(TIM_HandleTypeDef *, TIM_OC_InitTypeDef *, uint32_t);
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *, uint32_t);
HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef *, uint32_t);
HAL_StatusTypeDef HAL_TIM_PWM_DeInit(TIM_HandleTypeDef *);
#endif
