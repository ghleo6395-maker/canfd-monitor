/* Narrow HAL double for the real can_fdcan.c receive/dispatch implementation. */
#ifndef TEST_CAN_HAL_H
#define TEST_CAN_HAL_H
#include <stdint.h>
#define HAL_OK 0
#define HAL_ERROR 1
typedef int HAL_StatusTypeDef;
typedef int IRQn_Type;
enum {FDCAN1_IT0_IRQn,FDCAN2_IT0_IRQn};
typedef struct {uint32_t IR,TXEFS,TXBRP;} FDCAN_GlobalTypeDef;
extern FDCAN_GlobalTypeDef test_fdcan[2];
#define FDCAN1 (&test_fdcan[0])
#define FDCAN2 (&test_fdcan[1])
typedef struct {uint32_t CNT;} TIM_TypeDef;
extern TIM_TypeDef test_tim;
#define TIM2 (&test_tim)
typedef struct {int dummy;} GPIO_TypeDef;
extern GPIO_TypeDef test_gpio[2];
#define GPIOB (&test_gpio[0])
#define GPIOD (&test_gpio[1])
typedef struct {uint32_t Pin,Mode,Pull,Speed,Alternate;} GPIO_InitTypeDef;
typedef struct {
 uint32_t FrameFormat,Mode,AutoRetransmission,TransmitPause,ProtocolException;
 uint32_t NominalPrescaler,NominalTimeSeg1,NominalTimeSeg2,NominalSyncJumpWidth;
 uint32_t DataPrescaler,DataTimeSeg1,DataTimeSeg2,DataSyncJumpWidth,MessageRAMOffset;
 uint32_t StdFiltersNbr,ExtFiltersNbr,RxFifo0ElmtsNbr,RxFifo0ElmtSize;
 uint32_t RxFifo1ElmtsNbr,RxFifo1ElmtSize,TxEventsNbr,TxFifoQueueElmtsNbr;
 uint32_t TxElmtSize,TxFifoQueueMode;
} FDCAN_InitTypeDef;
typedef struct {FDCAN_GlobalTypeDef *Instance;FDCAN_InitTypeDef Init;} FDCAN_HandleTypeDef;
typedef struct {
 uint32_t Identifier,IdType,RxFrameType,DataLength,ErrorStateIndicator;
 uint32_t BitRateSwitch,FDFormat;
} FDCAN_RxHeaderTypeDef;
typedef struct {
 uint32_t Identifier,IdType,TxFrameType,DataLength,ErrorStateIndicator;
 uint32_t BitRateSwitch,FDFormat,TxEventFifoControl,MessageMarker;
} FDCAN_TxHeaderTypeDef;
typedef struct {uint32_t MessageMarker;} FDCAN_TxEventFifoTypeDef;
typedef struct {uint32_t TxErrorCnt,RxErrorCnt;} FDCAN_ErrorCountersTypeDef;
typedef struct {uint32_t BusOff,LastErrorCode;} FDCAN_ProtocolStatusTypeDef;
#define GPIO_PIN_0 1u
#define GPIO_PIN_1 2u
#define GPIO_PIN_3 8u
#define GPIO_PIN_12 4096u
#define GPIO_PIN_13 8192u
#define GPIO_PIN_14 16384u
#define GPIO_PIN_SET 1
#define GPIO_PIN_RESET 0
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_AF_PP 1
#define GPIO_NOPULL 0
#define GPIO_SPEED_FREQ_VERY_HIGH 3
#define GPIO_AF9_FDCAN1 9
#define DISABLE 0
#define ENABLE 1
#define FDCAN_FRAME_FD_BRS 1
#define FDCAN_FRAME_CLASSIC 0
#define FDCAN_MODE_BUS_MONITORING 1
#define FDCAN_MODE_RESTRICTED_OPERATION 2
#define FDCAN_MODE_NORMAL 0
#define FDCAN_DATA_BYTES_64 18
#define FDCAN_TX_FIFO_OPERATION 0
#define FDCAN_ACCEPT_IN_RX_FIFO0 0
#define FDCAN_FILTER_REMOTE 1
#define FDCAN_RX_FIFO0 0
#define FDCAN_RX_FIFO_BLOCKING 0
#define FDCAN_INTERRUPT_LINE0 1
#define FDCAN_IT_RX_FIFO0_NEW_MESSAGE 1u
#define FDCAN_IT_RX_FIFO0_MESSAGE_LOST 8u
#define FDCAN_EXTENDED_ID 1
#define FDCAN_STANDARD_ID 0
#define FDCAN_REMOTE_FRAME 1
#define FDCAN_DATA_FRAME 0
#define FDCAN_FD_CAN 1
#define FDCAN_CLASSIC_CAN 0
#define FDCAN_BRS_ON 1
#define FDCAN_BRS_OFF 0
#define FDCAN_ESI_PASSIVE 1
#define FDCAN_ESI_ACTIVE 0
#define FDCAN_STORE_TX_EVENTS 1
#define FDCAN_TXEFS_EFFL 63u
void test_clock_enable(void);
#define __HAL_RCC_FDCAN_CLK_ENABLE() test_clock_enable()
void HAL_GPIO_WritePin(GPIO_TypeDef*,uint32_t,int);
void HAL_GPIO_Init(GPIO_TypeDef*,GPIO_InitTypeDef*);
void HAL_NVIC_DisableIRQ(IRQn_Type);
void HAL_NVIC_ClearPendingIRQ(IRQn_Type);
void HAL_NVIC_SetPendingIRQ(IRQn_Type);
void HAL_NVIC_SetPriority(IRQn_Type,uint32_t,uint32_t);
void HAL_NVIC_EnableIRQ(IRQn_Type);
uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_FDCAN_Init(FDCAN_HandleTypeDef*);
HAL_StatusTypeDef HAL_FDCAN_DeInit(FDCAN_HandleTypeDef*);
HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef*);
HAL_StatusTypeDef HAL_FDCAN_Stop(FDCAN_HandleTypeDef*);
HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(FDCAN_HandleTypeDef*,uint32_t,uint32_t,uint32_t,uint32_t);
HAL_StatusTypeDef HAL_FDCAN_ConfigRxFifoOverwrite(FDCAN_HandleTypeDef*,uint32_t,uint32_t);
HAL_StatusTypeDef HAL_FDCAN_ConfigInterruptLines(FDCAN_HandleTypeDef*,uint32_t,uint32_t);
HAL_StatusTypeDef HAL_FDCAN_ActivateNotification(FDCAN_HandleTypeDef*,uint32_t,uint32_t);
HAL_StatusTypeDef HAL_FDCAN_ConfigTxDelayCompensation(FDCAN_HandleTypeDef*,uint32_t,uint32_t);
HAL_StatusTypeDef HAL_FDCAN_EnableTxDelayCompensation(FDCAN_HandleTypeDef*);
HAL_StatusTypeDef HAL_FDCAN_AbortTxRequest(FDCAN_HandleTypeDef*,uint32_t);
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef*,FDCAN_TxHeaderTypeDef*,uint8_t*);
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef*,uint32_t);
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef*,uint32_t,FDCAN_RxHeaderTypeDef*,uint8_t*);
HAL_StatusTypeDef HAL_FDCAN_GetTxEvent(FDCAN_HandleTypeDef*,FDCAN_TxEventFifoTypeDef*);
HAL_StatusTypeDef HAL_FDCAN_GetErrorCounters(FDCAN_HandleTypeDef*,FDCAN_ErrorCountersTypeDef*);
HAL_StatusTypeDef HAL_FDCAN_GetProtocolStatus(FDCAN_HandleTypeDef*,FDCAN_ProtocolStatusTypeDef*);
void HAL_FDCAN_IRQHandler(FDCAN_HandleTypeDef*);
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef*,uint32_t);
#endif

uint32_t __get_IPSR(void); /* Test context only. */
