/* SPDX-License-Identifier: MIT */
#include "board.h"
#include "can_ingress.h"
#include "services/core/port.h"
#include "services/core/app_evt.h"
#include "services/frame_ring/frame_ring.h"
#include "app/app.h"
#include <string.h>
#if defined(REVD_FREERTOS)
#include "rtos/runtime.h"
#endif

typedef struct {
 FDCAN_HandleTypeDef h;
 _Atomic bool active;
 bool probe, fd_seen, irq_rx_serviced;
 bool status_pending, probe_pending, tx_aborting;
 uint32_t generation;
 app_evt_t status_event, probe_event;
 uint32_t nbr,dbr,probe_due,valid,errors,status_ms;
 uint32_t probe_rx_fence;
 bool fd,pending[8];
 can_mode_t mode;
 can_frame_t sent[8];
 can_ingress_t rx;
 _Atomic uint32_t hw_lost_events, irq_read_errors;
 uint32_t ring_drops, reported_hw, reported_ingress, reported_ring, done_events;
 uint32_t tx_echo_drops, reported_tx_echo;
 uint32_t last_service_us, max_service_gap_us, max_service_us;
 bool service_time_valid;
} can_port_t;
#define TX_ECHO_LEN 64u
#ifdef CANFD_REVD_TARGET
static can_port_t cp[2] __attribute__((section(".revd_d2_bss"),aligned(32)));
static can_frame_t tx_echo[TX_ECHO_LEN] __attribute__((section(".revd_d2_bss"),aligned(32)));
#else
static can_port_t cp[2];
static can_frame_t tx_echo[TX_ECHO_LEN];
#endif
static uint32_t tx_echo_in,tx_echo_out;
static const uint8_t dlc_len[16]={0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
static IRQn_Type can_irq(unsigned ch) { return ch ? FDCAN2_IT0_IRQn : FDCAN1_IT0_IRQn; }

static uint64_t extend_rx_time(uint32_t low)
{
 /* Only foreground may use board_time_us()'s wrap-extension state. Read now
  * after copying the queued frame, so an IRQ cannot give it a future stamp. */
 uint64_t now=board_time_us();
 uint64_t stamp=(now & ~UINT64_C(0xFFFFFFFF)) | low;
 if(stamp>now && stamp>UINT32_MAX) stamp-=UINT64_C(1)<<32;
 return stamp;
}
static void broadcast_rx(unsigned ch,can_frame_t *f)
{
 can_port_t *c=&cp[ch];
 f->ts_us=extend_rx_time((uint32_t)f->ts_us);
 if(ring_push(f)) {
  /* The queue preserves old-rate frames for recording across reconfigure.
   * Only entries beyond this probe's producer fence can validate its rate.
   * The foreground advances tail before dispatching this copied frame. */
  uint32_t tail=atomic_load_explicit(&c->rx.tail,memory_order_relaxed);
  if(c->probe && (int32_t)(tail-c->probe_rx_fence)>0) {
   c->valid++;c->fd_seen|=(f->flags&CANF_FD)!=0;
  }
 }
 else c->ring_drops++;
}
static void report_drops(unsigned ch)
{
 can_port_t *c=&cp[ch];
 uint32_t totals[4]={atomic_load_explicit(&c->hw_lost_events,memory_order_relaxed),can_ingress_dropped(&c->rx),c->ring_drops,c->tx_echo_drops};
 uint32_t *reported[4]={&c->reported_hw,&c->reported_ingress,&c->reported_ring,&c->reported_tx_echo};
 for(unsigned kind=0;kind<4;kind++) {
  uint32_t delta=totals[kind]-*reported[kind];
  /* Retain an undelivered delta when the foreground application event queue
   * is full. A failed event enqueue must not hide a known data loss. */
  if(delta && evt_post4(EV_CAN_DROPS,(uint8_t)ch,SRC_SYS,(int32_t)delta,(int32_t)(kind+1)))
   *reported[kind]=totals[kind];
 }
}

bool board_can_metrics(unsigned ch,board_can_metrics_t *out)
{
 if(ch>=2 || !out)return false;
 can_port_t *c=&cp[ch];
 *out=(board_can_metrics_t){
  .hw_lost_events=atomic_load_explicit(&c->hw_lost_events,memory_order_relaxed),
  .ingress_drops=can_ingress_dropped(&c->rx),.ring_drops=c->ring_drops,
  .irq_read_errors=atomic_load_explicit(&c->irq_read_errors,memory_order_relaxed),.tx_echo_drops=c->tx_echo_drops,
  .queued=can_ingress_level(&c->rx),.high_water=can_ingress_high_water(&c->rx),
  .max_service_gap_us=c->max_service_gap_us,.max_service_us=c->max_service_us};
 return true;
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *handle,uint32_t flags)
{
 unsigned ch=handle==&cp[1].h ? 1u : 0u;
 can_port_t *c=&cp[ch];
 if(handle!=&c->h || !atomic_load_explicit(&c->active,memory_order_acquire))return;
 c->irq_rx_serviced=true;
 if(flags&FDCAN_IT_RX_FIFO0_MESSAGE_LOST)
  atomic_fetch_add_explicit(&c->hw_lost_events,1,memory_order_relaxed);
 /* At most one FIFO's worth of work in one interrupt, even if traffic is
  * continuous. The controller can keep receiving while foreground is inside
  * FatFs. Never call the broadcast ring, event queue or SD/LVGL here. */
 unsigned budget=48;
 bool read_failed=false;
 while(budget-- && HAL_FDCAN_GetRxFifoFillLevel(handle,FDCAN_RX_FIFO0)) {
  FDCAN_RxHeaderTypeDef h;
  can_frame_t f={.ch=(uint8_t)ch,.ts_us=TIM2->CNT};
  if(HAL_FDCAN_GetRxMessage(handle,FDCAN_RX_FIFO0,&h,f.data)!=HAL_OK) {
   atomic_fetch_add_explicit(&c->irq_read_errors,1,memory_order_relaxed);read_failed=true;break;
  }
  unsigned dlc=h.DataLength&15u;
  f.id=h.Identifier;
  /* Classical CAN DLC 9..15 carries eight bytes, not an FD-sized payload. */
  f.len=h.FDFormat==FDCAN_FD_CAN?dlc_len[dlc]:(uint8_t)(dlc>8u?8u:dlc);
  if(h.IdType==FDCAN_EXTENDED_ID)f.flags|=CANF_EXT;
  if(h.RxFrameType==FDCAN_REMOTE_FRAME)f.flags|=CANF_RTR;
  if(h.FDFormat==FDCAN_FD_CAN)f.flags|=CANF_FD;
  if(h.BitRateSwitch==FDCAN_BRS_ON)f.flags|=CANF_BRS;
  if(h.ErrorStateIndicator==FDCAN_ESI_PASSIVE)f.flags|=CANF_ESI;
  (void)can_ingress_push(&c->rx,&f);
 }
 if(!read_failed && HAL_FDCAN_GetRxFifoFillLevel(handle,FDCAN_RX_FIFO0))
  HAL_NVIC_SetPendingIRQ(can_irq(ch));
#if defined(REVD_FREERTOS)
 /* CAN priority 4 cannot invoke the kernel; a software-only IRQ6 performs
  * the FromISR notification after this higher-priority ISR returns. */
 revd_rtos_notify_core_from_isr_deferred();
#endif
}

static void can_irq_service(unsigned ch)
{
 can_port_t *c=&cp[ch];
 if(!atomic_load_explicit(&c->active,memory_order_acquire))return;
 c->irq_rx_serviced=false;
 HAL_FDCAN_IRQHandler(&c->h);
 /* A software re-pend may retry a FIFO read after HAL already cleared RF0N.
  * Do not process a second 48-frame batch after the normal HAL callback. */
 if(!c->irq_rx_serviced && HAL_FDCAN_GetRxFifoFillLevel(&c->h,FDCAN_RX_FIFO0))
  HAL_FDCAN_RxFifo0Callback(&c->h,0);
}
void FDCAN1_IT0_IRQHandler(void) {can_irq_service(0);}
void FDCAN2_IT0_IRQHandler(void) {can_irq_service(1);}
static bool timing(uint32_t rate,bool data,uint32_t *pres,uint32_t *s1,uint32_t *s2)
{
 if(!rate) return false;
 uint32_t best=0xFFFFFFFFu;
 for(uint32_t p=1;p<=(data?32u:512u);p++) {
  uint64_t divisor=(uint64_t)rate*p;
  uint32_t total=(uint32_t)((80000000u+divisor/2)/divisor);
  if(total<4 || total>(data?49u:385u)) continue;
  uint32_t b2=total/5; if(b2<1)b2=1; if(b2>(data?16u:128u)) b2=data?16:128;
  uint32_t b1=total-b2-1;
  if(b1<1 || b1>(data?32u:256u))continue;
  uint32_t actual=80000000u/(p*total);
  uint32_t err=actual>rate?actual-rate:rate-actual;
  if(err<best) { best=err;*pres=p;*s1=b1;*s2=b2; }
 }
 return best<=rate/200u; /* at most 0.5% error */
}
void port_can_standby(int ch)
{
 if((unsigned)ch>=2)return;
 can_port_t *c=&cp[ch];
 c->generation++;if(!c->generation)c->generation++;
 c->status_pending=c->probe_pending=false;
 /* Disable the transceiver before a potentially slow HAL stop/deinit. */
 HAL_GPIO_WritePin(ch?GPIOB:GPIOD,ch?GPIO_PIN_14:GPIO_PIN_3,GPIO_PIN_SET);
 HAL_NVIC_DisableIRQ(can_irq((unsigned)ch));
 atomic_store_explicit(&c->active,false,memory_order_relaxed);
 c->service_time_valid=false;
 if(c->h.Instance) {HAL_FDCAN_Stop(&c->h);HAL_FDCAN_DeInit(&c->h);}
 HAL_NVIC_ClearPendingIRQ(can_irq((unsigned)ch));
 /* Preserve queued frames across a mode change. Unified foreground merging
  * retains their order relative to the other channel and delayed TX echoes. */
 report_drops((unsigned)ch);
 c->probe=false;
 c->tx_aborting=false;
 c->done_events=0;
 memset(c->pending,0,sizeof c->pending);
 GPIO_InitTypeDef g={.Pin=ch?(GPIO_PIN_12|GPIO_PIN_13):(GPIO_PIN_0|GPIO_PIN_1),.Mode=GPIO_MODE_INPUT,.Pull=GPIO_NOPULL};
 HAL_GPIO_Init(ch?GPIOB:GPIOD,&g);
}
uint32_t port_can_generation(int ch)
{
 return (unsigned)ch<2?cp[ch].generation:0;
}
bool port_can_start(int ch,uint32_t nbr,uint32_t dbr,bool fd,can_mode_t mode)
{
 if((unsigned)ch>=2)return false;
 port_can_standby(ch);
 if((unsigned)mode>CAN_MODE_NORMAL)return false;
 can_port_t *c=&cp[ch];
 c->nbr=nbr;c->dbr=dbr;c->fd=fd;c->mode=mode;
 c->h.Instance=ch?FDCAN2:FDCAN1;
 __HAL_RCC_FDCAN_CLK_ENABLE();
 GPIO_InitTypeDef g={.Mode=GPIO_MODE_AF_PP,.Pull=GPIO_NOPULL,.Speed=GPIO_SPEED_FREQ_VERY_HIGH,.Alternate=GPIO_AF9_FDCAN1};
 g.Pin=ch?(GPIO_PIN_12|GPIO_PIN_13):(GPIO_PIN_0|GPIO_PIN_1);
 HAL_GPIO_Init(ch?GPIOB:GPIOD,&g);
 FDCAN_InitTypeDef *i=&c->h.Init;
 memset(i,0,sizeof *i);
 i->FrameFormat=fd?FDCAN_FRAME_FD_BRS:FDCAN_FRAME_CLASSIC;
 i->Mode=mode==CAN_MODE_LISTEN?FDCAN_MODE_BUS_MONITORING:
         mode==CAN_MODE_RESTRICTED?FDCAN_MODE_RESTRICTED_OPERATION:FDCAN_MODE_NORMAL;
 i->AutoRetransmission=DISABLE; i->TransmitPause=ENABLE; i->ProtocolException=ENABLE;
 if(!timing(nbr,false,&i->NominalPrescaler,&i->NominalTimeSeg1,&i->NominalTimeSeg2) ||
    !timing(fd?dbr:2000000,true,&i->DataPrescaler,&i->DataTimeSeg1,&i->DataTimeSeg2)) {
  evt_msg(MSG_CH_NO_RATE,(uint8_t)ch,0);port_can_standby(ch);return false;
 }
 i->NominalSyncJumpWidth=i->NominalTimeSeg2>4?4:i->NominalTimeSeg2;
 i->DataSyncJumpWidth=i->DataTimeSeg2>4?4:i->DataTimeSeg2;
 i->MessageRAMOffset=ch?1248:0;
 i->StdFiltersNbr=32;i->ExtFiltersNbr=16;
 i->RxFifo0ElmtsNbr=48;i->RxFifo0ElmtSize=FDCAN_DATA_BYTES_64;
 i->RxFifo1ElmtsNbr=8;i->RxFifo1ElmtSize=FDCAN_DATA_BYTES_64;
 i->TxEventsNbr=16;i->TxFifoQueueElmtsNbr=8;i->TxElmtSize=FDCAN_DATA_BYTES_64;
 i->TxFifoQueueMode=FDCAN_TX_FIFO_OPERATION;
 if(HAL_FDCAN_Init(&c->h)!=HAL_OK ||
    HAL_FDCAN_ConfigGlobalFilter(&c->h,FDCAN_ACCEPT_IN_RX_FIFO0,FDCAN_ACCEPT_IN_RX_FIFO0,FDCAN_FILTER_REMOTE,FDCAN_FILTER_REMOTE)!=HAL_OK ||
    HAL_FDCAN_ConfigRxFifoOverwrite(&c->h,FDCAN_RX_FIFO0,FDCAN_RX_FIFO_BLOCKING)!=HAL_OK ||
    HAL_FDCAN_ConfigInterruptLines(&c->h,FDCAN_IT_RX_FIFO0_NEW_MESSAGE|FDCAN_IT_RX_FIFO0_MESSAGE_LOST,FDCAN_INTERRUPT_LINE0)!=HAL_OK ||
    HAL_FDCAN_ActivateNotification(&c->h,FDCAN_IT_RX_FIFO0_NEW_MESSAGE|FDCAN_IT_RX_FIFO0_MESSAGE_LOST,0)!=HAL_OK) {
  evt_msg(MSG_BUS_ABNORMAL,(uint8_t)ch,0);port_can_standby(ch);return false;
 }
 if(fd && dbr>=2000000u) {
  if(HAL_FDCAN_ConfigTxDelayCompensation(&c->h,i->DataPrescaler*(1+i->DataTimeSeg1),0)!=HAL_OK ||
     HAL_FDCAN_EnableTxDelayCompensation(&c->h)!=HAL_OK) {
   evt_msg(MSG_BUS_ABNORMAL,(uint8_t)ch,0);port_can_standby(ch);return false;
  }
 }
 if(HAL_FDCAN_Start(&c->h)!=HAL_OK) {evt_msg(MSG_BUS_ABNORMAL,(uint8_t)ch,0);port_can_standby(ch);return false;}
 c->status_ms=HAL_GetTick();
 atomic_store_explicit(&c->active,true,memory_order_release);
 /* Shared FDCAN kernel/RAM are never reset or disabled when the other
  * channel is reconfigured. CAN preempts SDMMC(5), SPI DMA(6) and USB(7). */
 HAL_NVIC_SetPriority(can_irq((unsigned)ch),4,0);
 HAL_NVIC_EnableIRQ(can_irq((unsigned)ch));
 HAL_GPIO_WritePin(ch?GPIOB:GPIOD,ch?GPIO_PIN_14:GPIO_PIN_3,GPIO_PIN_RESET);
 return true;
}
void port_can_tx_abort(int ch)
{
 if((unsigned)ch>=2)return;
 can_port_t *c=&cp[ch];
 c->tx_aborting=c->active;
 if(c->active) HAL_FDCAN_AbortTxRequest(&c->h,0xFFu);
 c->done_events=0;
 memset(c->pending,0,sizeof c->pending);
}
static bool tx_abort_ready(can_port_t *c)
{
 if(!c->tx_aborting)return true;
 /* TXBCR is an asynchronous cancellation request. A frame can finish while
  * it is being cancelled and leave a late event carrying its old marker.
  * Reuse no marker until all eight hardware requests have retired, then
  * discard their events. No new TX is admitted anywhere during this fence. */
 if(c->h.Instance->TXBRP&0xFFu)return false;
 while(c->h.Instance->TXEFS&FDCAN_TXEFS_EFFL) {
  FDCAN_TxEventFifoTypeDef old;
  if(HAL_FDCAN_GetTxEvent(&c->h,&old)!=HAL_OK)return false;
 }
 c->tx_aborting=false;
 return true;
}
bool port_can_recover(int ch)
{
 return (unsigned)ch<2 && port_can_start(ch,cp[ch].nbr,cp[ch].dbr,cp[ch].fd,cp[ch].mode);
}
bool port_can_probe(int ch,uint32_t nbr,uint32_t dbr,bool data_phase,can_mode_t mode)
{
 if(!port_can_start(ch,nbr,data_phase?dbr:2000000u,true,mode))return false;
 cp[ch].probe=true;cp[ch].probe_due=HAL_GetTick()+180;
 cp[ch].probe_rx_fence=atomic_load_explicit(&cp[ch].rx.head,memory_order_acquire);
 cp[ch].valid=cp[ch].errors=0;cp[ch].fd_seen=false;
 return true;
}
bool port_can_tx(int ch,const can_frame_t *f)
{
 if((unsigned)ch>=2 || !f)return false;
 can_port_t *c=&cp[ch];
 if(!c->active || c->mode!=CAN_MODE_NORMAL) return false;
 if(!tx_abort_ready(c))return false;
 unsigned slot;
 for(slot=0;slot<8 && c->pending[slot];slot++){}
 if(slot==8)return false;
 unsigned dlc;
 for(dlc=0;dlc<16 && dlc_len[dlc]<f->len;dlc++){}
 if(dlc==16)return false;
 FDCAN_TxHeaderTypeDef h={0};
 h.Identifier=f->id;h.IdType=(f->flags&CANF_EXT)?FDCAN_EXTENDED_ID:FDCAN_STANDARD_ID;
 h.TxFrameType=(f->flags&CANF_RTR)?FDCAN_REMOTE_FRAME:FDCAN_DATA_FRAME;
 h.DataLength=dlc;h.ErrorStateIndicator=FDCAN_ESI_ACTIVE;
 h.BitRateSwitch=(f->flags&CANF_BRS)?FDCAN_BRS_ON:FDCAN_BRS_OFF;
 h.FDFormat=(f->flags&CANF_FD)?FDCAN_FD_CAN:FDCAN_CLASSIC_CAN;
 h.TxEventFifoControl=FDCAN_STORE_TX_EVENTS;h.MessageMarker=slot;
 if(HAL_FDCAN_AddMessageToTxFifoQ(&c->h,&h,(uint8_t*)f->data)!=HAL_OK) {
  return false; /* No pending credit, dynamic counter advance or NoACK deadline. */
 }
 c->pending[slot]=true;c->sent[slot]=*f;
 return true;
}
void board_can_task(void)
{
 /* Read only the free-running TIM2 low word: no wrap-extension state or
  * application clock work is needed for bounded foreground intervals. */
 uint32_t service_started=TIM2->CNT;
 bool serviced[2];
 for(unsigned ch=0;ch<2;ch++) {
  can_port_t *c=&cp[ch];
  serviced[ch]=atomic_load_explicit(&c->active,memory_order_relaxed);
  if(!serviced[ch]) {c->service_time_valid=false;continue;}
  if(c->service_time_valid) {
   uint32_t gap=service_started-c->last_service_us;
   if(gap>c->max_service_gap_us)c->max_service_gap_us=gap;
  }
  c->last_service_us=service_started;c->service_time_valid=true;
 }
 /* Resolve true TX completions before RX work and before app_tick advances
  * software TX deadlines. A failed hardware enqueue never becomes TX_DONE. */
 for(int ch=0;ch<2;ch++) {
  can_port_t *c=&cp[ch];if(!c->active)continue;
  if(c->tx_aborting) {(void)tx_abort_ready(c);continue;}
  while((c->h.Instance->TXEFS & FDCAN_TXEFS_EFFL)) {
   FDCAN_TxEventFifoTypeDef e;
   if(HAL_FDCAN_GetTxEvent(&c->h,&e)!=HAL_OK)break;
   unsigned slot=e.MessageMarker;
   if(slot<8 && c->pending[slot]) {
    can_frame_t f=c->sent[slot];f.flags|=CANF_TX;f.ts_us=board_time_us();
    f.ch=(uint8_t)ch;
    if(tx_echo_in-tx_echo_out==TX_ECHO_LEN)c->tx_echo_drops++;
    else tx_echo[tx_echo_in++%TX_ECHO_LEN]=f;
    c->pending[slot]=false;c->done_events++;
   }
  }
#ifdef REVD_FREERTOS
  while(c->done_events && evt_post4(EV_CAN_TX_DONE,(uint8_t)ch,SRC_SYS,0,0))c->done_events--;
#else
  /* This is foreground, never ISR context. Returning TX credits directly is
   * safe in an SD wait and avoids filling the software FIFO while TX_DONE
   * events wait for the outer command dispatcher. Do not post a second event. */
  while(c->done_events) {app_tx_complete(ch);c->done_events--;}
#endif
 }
 /* Merge by reception time, retaining fairness when a busy channel has a
  * backlog. Bound dispatch so recorder/stats can consume the 512-frame ring
  * every loop rather than letting a 2 x 512-frame burst fill it at once. */
 for(unsigned budget=128;budget;budget--) {
  can_frame_t f[2];
  bool have0=can_ingress_peek(&cp[0].rx,&f[0]);
  bool have1=can_ingress_peek(&cp[1].rx,&f[1]);
  bool have_tx=tx_echo_in!=tx_echo_out;
  if(!have0 && !have1 && !have_tx)break;
  unsigned ch=!have0 ? 1u : !have1 ? 0u :
   (int32_t)((uint32_t)f[0].ts_us-(uint32_t)f[1].ts_us)>0 ? 1u : 0u;
  if(have_tx && ((!have0 && !have1) ||
      (int32_t)((uint32_t)tx_echo[tx_echo_out%TX_ECHO_LEN].ts_us-(uint32_t)f[ch].ts_us)<0)) {
   can_frame_t *echo=&tx_echo[tx_echo_out++%TX_ECHO_LEN];
   if(!ring_push(echo))cp[echo->ch].ring_drops++;
  } else {
   can_ingress_advance(&cp[ch].rx);broadcast_rx(ch,&f[ch]);
  }
 }
 for(int ch=0;ch<2;ch++) {
  can_port_t *c=&cp[ch];report_drops((unsigned)ch);
  if(!c->active)continue;
  /* Retry a transient HAL read error at foreground cadence rather than
   * continually re-pending a failed FIFO read inside the ISR. */
  if(HAL_FDCAN_GetRxFifoFillLevel(&c->h,FDCAN_RX_FIFO0))HAL_NVIC_SetPendingIRQ(can_irq((unsigned)ch));
  uint32_t now=HAL_GetTick();
  if(c->status_pending && evt_post(&c->status_event))c->status_pending=false;
  if(now-c->status_ms>=100) {
   c->status_ms=now;
   FDCAN_ErrorCountersTypeDef er={0};FDCAN_ProtocolStatusTypeDef ps={0};
   if(HAL_FDCAN_GetErrorCounters(&c->h,&er)==HAL_OK &&
      HAL_FDCAN_GetProtocolStatus(&c->h,&ps)==HAL_OK) {
    /* Keep an undelivered Bus-Off observation sticky until dispatch. A later
     * healthy sample may update counters but cannot erase that transition. */
    bool busoff=ps.BusOff || (c->status_pending && (c->status_event.b&0x10000));
    c->status_event=(app_evt_t){.type=EV_CAN_STATUS,.ch=(uint8_t)ch,.src=SRC_SYS,
      .a=(int32_t)er.TxErrorCnt,.b=(int32_t)(er.RxErrorCnt|(busoff?0x10000u:0)),.generation=c->generation};
    c->status_pending=!evt_post(&c->status_event);
    if(c->probe && ps.LastErrorCode && ps.LastErrorCode!=7)c->errors++;
   } else if(c->probe) {
    c->errors++; /* An unreadable status cannot validate an automatic rate. */
   }
  }
  if(c->probe && (int32_t)(now-c->probe_due)>=0) {
   c->probe=false;
   c->probe_event=(app_evt_t){.type=EV_CAN_PROBE_RESULT,.ch=(uint8_t)ch,.src=SRC_SYS,
    .a=(int32_t)c->valid,.b=(int32_t)((c->errors>UINT16_MAX?UINT16_MAX:c->errors)|(c->fd_seen?0x10000u:0)),.generation=c->generation};
   c->probe_pending=true;
  }
  if(c->probe_pending && evt_post(&c->probe_event))c->probe_pending=false;
 }
 uint32_t service_elapsed=TIM2->CNT-service_started;
 for(unsigned ch=0;ch<2;ch++)
  if(serviced[ch] && service_elapsed>cp[ch].max_service_us)
   cp[ch].max_service_us=service_elapsed;
}
