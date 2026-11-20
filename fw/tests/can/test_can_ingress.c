/* Exercises the production ISR producer and foreground dispatch with a
 * 48-entry hardware FIFO model. These are not bandwidth claims for a board. */
#include "stm32h7xx_hal.h"
#include "can_ingress.h"
#include "board.h"
#include "services/core/port.h"
#include "services/core/app_evt.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

FDCAN_GlobalTypeDef test_fdcan[2];
TIM_TypeDef test_tim;
GPIO_TypeDef test_gpio[2];
typedef struct {FDCAN_RxHeaderTypeDef h;uint8_t data[64];} hw_frame_t;
typedef struct {
 hw_frame_t fifo[48];unsigned head,tail,count;
 uint32_t flags,notifications;bool enabled,read_fail,tx_fail;
 bool init_fail,start_fail,tdc_fail,counter_fail,status_fail;
 bool defer_abort,tx_event_fail,tx_event_override;
 FDCAN_ErrorCountersTypeDef counters;
 FDCAN_ProtocolStatusTypeDef status;
 uint32_t tx_marker,tx_event_marker,priority,reads,refill;FDCAN_HandleTypeDef *handle;
} model_t;
static model_t model[2];
static bool in_irq,deny_events;
static uint64_t time_us;
static uint32_t broadcast_cost_us;
static unsigned clock_enables,events_n,broadcast_n,broadcast_cap=10000;
static unsigned completed[2];
static app_evt_t events[4096];
static can_frame_t broadcast[10000];
static void receive(unsigned ch,uint32_t id);
void FDCAN1_IT0_IRQHandler(void);
void FDCAN2_IT0_IRQHandler(void);

static unsigned index_of(FDCAN_HandleTypeDef *h) {assert(h->Instance==FDCAN1||h->Instance==FDCAN2);return h->Instance==FDCAN2;}
uint64_t board_time_us(void) {assert(!in_irq);return time_us;}
uint32_t HAL_GetTick(void) {return (uint32_t)(time_us/1000);}
void test_clock_enable(void) {clock_enables++;}
void HAL_GPIO_WritePin(GPIO_TypeDef *g,uint32_t p,int v) {(void)g;(void)p;(void)v;}
void HAL_GPIO_Init(GPIO_TypeDef *g,GPIO_InitTypeDef *p) {(void)g;(void)p;}
void HAL_NVIC_DisableIRQ(IRQn_Type i) {model[i].enabled=false;}
void HAL_NVIC_EnableIRQ(IRQn_Type i) {model[i].enabled=true;}
void HAL_NVIC_ClearPendingIRQ(IRQn_Type i) {(void)i;}
void HAL_NVIC_SetPendingIRQ(IRQn_Type i) {(void)i;}
void HAL_NVIC_SetPriority(IRQn_Type i,uint32_t p,uint32_t sub) {assert(!sub);model[i].priority=p;}
HAL_StatusTypeDef HAL_FDCAN_Init(FDCAN_HandleTypeDef *h) {
 unsigned ch=index_of(h);model[ch].handle=h;
 assert(h->Init.MessageRAMOffset==(ch?1248u:0u));
 assert(h->Init.RxFifo0ElmtsNbr==48);return model[ch].init_fail?HAL_ERROR:HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_Stop(FDCAN_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_DeInit(FDCAN_HandleTypeDef *h) {
 unsigned ch=index_of(h);model[ch].head=model[ch].tail=model[ch].count=0;
 model[ch].flags=0;test_fdcan[ch].TXEFS=test_fdcan[ch].TXBRP=0;
 model[ch].defer_abort=model[ch].tx_event_fail=model[ch].tx_event_override=false;return HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef *h) {return model[index_of(h)].start_fail?HAL_ERROR:HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(FDCAN_HandleTypeDef*h,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {(void)h;(void)a;(void)b;(void)c;(void)d;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ConfigRxFifoOverwrite(FDCAN_HandleTypeDef*h,uint32_t f,uint32_t mode) {(void)h;assert(f==FDCAN_RX_FIFO0&&mode==FDCAN_RX_FIFO_BLOCKING);return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ConfigInterruptLines(FDCAN_HandleTypeDef*h,uint32_t flags,uint32_t line) {(void)h;(void)flags;assert(line==FDCAN_INTERRUPT_LINE0);return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ActivateNotification(FDCAN_HandleTypeDef*h,uint32_t flags,uint32_t tx) {assert(!tx);model[index_of(h)].notifications=flags;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ConfigTxDelayCompensation(FDCAN_HandleTypeDef*h,uint32_t a,uint32_t b) {(void)a;(void)b;return model[index_of(h)].tdc_fail?HAL_ERROR:HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_EnableTxDelayCompensation(FDCAN_HandleTypeDef*h) {return model[index_of(h)].tdc_fail?HAL_ERROR:HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_AbortTxRequest(FDCAN_HandleTypeDef*h,uint32_t mask) {
 unsigned ch=index_of(h);assert(mask==255);
 if(!model[ch].defer_abort)test_fdcan[ch].TXBRP&=~mask;
 return HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef*h,FDCAN_TxHeaderTypeDef*hdr,uint8_t*d) {
 (void)d;model_t *m=&model[index_of(h)];assert(hdr->DataLength<16);
 if(m->tx_fail)return HAL_ERROR;
 m->tx_marker=hdr->MessageMarker;test_fdcan[index_of(h)].TXBRP|=1u<<hdr->MessageMarker;return HAL_OK;
}
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef*h,uint32_t fifo) {assert(fifo==FDCAN_RX_FIFO0);return model[index_of(h)].count;}
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef*h,uint32_t fifo,FDCAN_RxHeaderTypeDef*hdr,uint8_t*d) {
 assert(in_irq&&fifo==FDCAN_RX_FIFO0);model_t *m=&model[index_of(h)];
 if(m->read_fail)return HAL_ERROR;
 assert(m->count);hw_frame_t *f=&m->fifo[m->tail++%48];m->count--;
 *hdr=f->h;memcpy(d,f->data,64);m->reads++;
 if(m->refill)receive(index_of(h),1000u+m->refill--);
 return HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_GetTxEvent(FDCAN_HandleTypeDef*h,FDCAN_TxEventFifoTypeDef*e) {
 assert(!in_irq);unsigned ch=index_of(h);assert(test_fdcan[ch].TXEFS);
 if(model[ch].tx_event_fail)return HAL_ERROR;
 e->MessageMarker=model[ch].tx_event_override?model[ch].tx_event_marker:model[ch].tx_marker;
 model[ch].tx_event_override=false;test_fdcan[ch].TXEFS=0;
 test_fdcan[ch].TXBRP&=~(1u<<e->MessageMarker);return HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_GetErrorCounters(FDCAN_HandleTypeDef*h,FDCAN_ErrorCountersTypeDef*e) {model_t*m=&model[index_of(h)];if(m->counter_fail)return HAL_ERROR;*e=m->counters;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_GetProtocolStatus(FDCAN_HandleTypeDef*h,FDCAN_ProtocolStatusTypeDef*p) {model_t*m=&model[index_of(h)];if(m->status_fail)return HAL_ERROR;*p=m->status;return HAL_OK;}
void HAL_FDCAN_IRQHandler(FDCAN_HandleTypeDef*h) {
 assert(in_irq);model_t *m=&model[index_of(h)];uint32_t flags=m->flags&m->notifications;
 m->flags&=~flags;if(flags)HAL_FDCAN_RxFifo0Callback(h,flags);
}
bool ring_push(const can_frame_t*f) {
 assert(!in_irq);if(broadcast_n>=broadcast_cap)return false;
 assert(broadcast_n<10000);broadcast[broadcast_n++]=*f;
 time_us+=broadcast_cost_us;test_tim.CNT=(uint32_t)time_us;return true;
}
bool evt_post4(ev_type_t type,uint8_t ch,src_t src,int32_t a,int32_t b) {
 assert(!in_irq);if(deny_events)return false;assert(events_n<4096);
 events[events_n++]=(app_evt_t){.type=type,.ch=ch,.src=src,.a=a,.b=b};return true;
}
bool evt_post(const app_evt_t *event) {
 assert(!in_irq);if(deny_events)return false;assert(events_n<4096);
 events[events_n++]=*event;return true;
}
void app_tx_complete(int ch) {assert(!in_irq&&ch>=0&&ch<2);completed[ch]++;}
void evt_msg(msg_t msg,uint8_t ch,int32_t b) {(void)msg;(void)ch;(void)b;assert(!in_irq);}
static void receive(unsigned ch,uint32_t id) {
 model_t*m=&model[ch];
 if(m->count==48) {m->flags|=FDCAN_IT_RX_FIFO0_MESSAGE_LOST;return;}
 hw_frame_t *f=&m->fifo[m->head++%48];*f=(hw_frame_t){.h={.Identifier=id,.DataLength=15,.IdType=FDCAN_EXTENDED_ID,.FDFormat=FDCAN_FD_CAN,.BitRateSwitch=FDCAN_BRS_ON,.ErrorStateIndicator=FDCAN_ESI_PASSIVE}};
 for(unsigned i=0;i<64;i++)f->data[i]=(uint8_t)(id+i);
 m->count++;m->flags|=FDCAN_IT_RX_FIFO0_NEW_MESSAGE;
}
static void irq(unsigned ch) {
 assert(model[ch].enabled);in_irq=true;
 if(ch)FDCAN2_IT0_IRQHandler();else FDCAN1_IT0_IRQHandler();in_irq=false;
}
static void setup(void) {
 port_can_standby(0);port_can_standby(1);board_can_task();broadcast_n=events_n=0;broadcast_cap=10000;deny_events=false;
 completed[0]=completed[1]=0;
 port_can_start(0,500000,2000000,true,CAN_MODE_NORMAL);
 port_can_start(1,500000,2000000,true,CAN_MODE_LISTEN);
 assert(model[0].priority==4&&model[1].priority==4);
}
static uint32_t drops(unsigned ch,int kind) {
 uint32_t n=0;for(unsigned i=0;i<events_n;i++)if(events[i].type==EV_CAN_DROPS&&events[i].ch==ch&&events[i].b==kind)n+=(uint32_t)events[i].a;return n;
}
static void test_service_latency_metrics(void) {
 setup();board_can_metrics_t a,b;
 time_us=UINT32_MAX-100u;test_tim.CNT=(uint32_t)time_us;
 board_can_task();assert(board_can_metrics(0,&a));
 assert(a.max_service_gap_us==0&&a.max_service_us==0); /* First call excludes boot. */
 time_us+=250;test_tim.CNT=(uint32_t)time_us;receive(0,7);irq(0);
 broadcast_cost_us=37;board_can_task();broadcast_cost_us=0;
 assert(board_can_metrics(0,&a));assert(board_can_metrics(1,&b));
 assert(a.max_service_gap_us==250&&a.max_service_us==37);
 assert(b.max_service_gap_us==250&&b.max_service_us==37); /* Shared dispatcher. */
 port_can_standby(1);time_us+=1000000;test_tim.CNT=(uint32_t)time_us;board_can_task();
 assert(board_can_metrics(1,&a));assert(a.max_service_gap_us==b.max_service_gap_us);
 port_can_start(1,500000,2000000,true,CAN_MODE_LISTEN);board_can_task();
 time_us+=100;test_tim.CNT=(uint32_t)time_us;board_can_task();
 assert(board_can_metrics(1,&a));assert(a.max_service_gap_us==250&&a.max_service_us==37);
 /* All cumulative diagnostics survive stop/start, but disabled time is excluded. */
 port_can_standby(0);port_can_standby(1);assert(board_can_metrics(0,&a));
 time_us+=100000000;test_tim.CNT=(uint32_t)time_us;board_can_task();
 port_can_start(0,500000,2000000,true,CAN_MODE_LISTEN);board_can_task();
 assert(board_can_metrics(0,&b));
 assert(a.max_service_gap_us==b.max_service_gap_us&&a.max_service_us==b.max_service_us);
 assert(a.ingress_drops==b.ingress_drops&&a.ring_drops==b.ring_drops);
}
static void test_stall_dual(void) {
 setup();board_can_metrics_t before[2],after;
 for(unsigned ch=0;ch<2;ch++)assert(board_can_metrics(ch,&before[ch]));
 for(unsigned i=0;i<CAN_INGRESS_LEN;i++)for(unsigned ch=0;ch<2;ch++) {
  time_us++;test_tim.CNT=(uint32_t)time_us;receive(ch,i*2+ch);irq(ch);
 }
 assert(!broadcast_n&&!events_n); /* All reception occurred without foreground. */
 board_can_task();assert(broadcast_n==128);
 for(unsigned i=128;i<2u*CAN_INGRESS_LEN;i+=128)board_can_task();
 assert(broadcast_n==2u*CAN_INGRESS_LEN);
 for(unsigned i=0;i<2u*CAN_INGRESS_LEN;i++) {
  can_frame_t*f=&broadcast[i];assert(f->id==i&&f->ch==(i&1)&&f->len==64);
  assert(f->flags==(CANF_EXT|CANF_FD|CANF_BRS|CANF_ESI));
  for(unsigned j=0;j<64;j++)assert(f->data[j]==(uint8_t)(i+j));
  if(i)assert(f->ts_us>broadcast[i-1].ts_us);
 }
 for(unsigned ch=0;ch<2;ch++) {
  assert(board_can_metrics(ch,&after));assert(after.ingress_drops==before[ch].ingress_drops);
  assert(!after.queued&&after.high_water==CAN_INGRESS_LEN);
 }
}
static void test_overflow_and_retry(void) {
 setup();board_can_metrics_t before,after;assert(board_can_metrics(0,&before));
 for(unsigned i=0;i<CAN_INGRESS_LEN+44u;i++) {time_us++;test_tim.CNT=(uint32_t)time_us;receive(0,i);irq(0);}
 assert(board_can_metrics(0,&after));assert(after.queued==CAN_INGRESS_LEN&&after.ingress_drops-before.ingress_drops==44);
 deny_events=true;board_can_task();assert(!events_n&&broadcast_n==128);
 deny_events=false;
 for(unsigned i=128;i<CAN_INGRESS_LEN;i+=128)board_can_task();
 assert(broadcast_n==CAN_INGRESS_LEN&&drops(0,CAN_DROP_INGRESS)==44);
 for(unsigned i=0;i<CAN_INGRESS_LEN;i++)assert(broadcast[i].id==i);
 for(unsigned i=0;i<60;i++)receive(1,i);
 irq(1);board_can_task();assert(drops(1,CAN_DROP_HW_LOST_EVENT)==1);
 /* Twelve real hardware losses coalesced to one RF0L bit: explicitly a lower bound. */
 assert(drops(1,CAN_DROP_INGRESS)==0);
}
static void test_ring_full_wrap_and_restart(void) {
 setup();time_us=(UINT64_C(1)<<32)-8;test_tim.CNT=(uint32_t)time_us;receive(0,1);irq(0);
 time_us+=16;test_tim.CNT=(uint32_t)time_us;receive(1,2);irq(1);
 board_can_task();assert(broadcast_n==2);assert(broadcast[0].ts_us==(UINT64_C(1)<<32)-8);
 assert(broadcast[1].ts_us==(UINT64_C(1)<<32)+8);
 broadcast_cap=broadcast_n;receive(0,3);irq(0);board_can_task();assert(drops(0,CAN_DROP_RING)==1);
 broadcast_cap=10000;receive(0,4);irq(0);
 unsigned old_clocks=clock_enables;port_can_start(1,250000,2000000,true,CAN_MODE_LISTEN);
 assert(clock_enables==old_clocks+1&&model[0].enabled);board_can_task();assert(broadcast[broadcast_n-1].id==4);
 receive(0,5);irq(0);port_can_standby(0);board_can_task();assert(broadcast[broadcast_n-1].id==5);
}
static void test_tx_and_rx_errors(void) {
 setup();can_frame_t f={.id=0x123,.len=64,.flags=CANF_FD|CANF_BRS};
 model[0].tx_fail=true;assert(!port_can_tx(0,&f));board_can_task();assert(!broadcast_n);
 for(unsigned i=0;i<events_n;i++)assert(events[i].type!=EV_CAN_TX_DONE);
 model[0].tx_fail=false;assert(port_can_tx(0,&f));test_fdcan[0].TXEFS=1;
 time_us++;test_tim.CNT=(uint32_t)time_us;receive(1,42);irq(1);
 deny_events=true;board_can_task();assert(broadcast_n==2&&(broadcast[1].flags&CANF_TX));
 assert(completed[0]==1); /* Full app event queue cannot delay TX credit. */
 deny_events=false;board_can_task();assert(broadcast_n==2&&completed[0]==1);
 assert(broadcast[0].id==42);
 for(unsigned i=0;i<events_n;i++)assert(events[i].type!=EV_CAN_TX_DONE);
 board_can_metrics_t b,a;assert(board_can_metrics(1,&b));
 receive(1,43);model[1].read_fail=true;irq(1);assert(board_can_metrics(1,&a));
 assert(a.irq_read_errors==b.irq_read_errors+1);model[1].read_fail=false;
 /* IRQ re-pend must recover even though the HAL cleared the original RF0N. */
 irq(1);board_can_task();assert(broadcast[broadcast_n-1].id==43);
}
static void test_bounded_irq_under_refill(void) {
 setup();for(unsigned i=0;i<48;i++)receive(0,i);
 model[0].refill=48;uint32_t before=model[0].reads;
 irq(0);assert(model[0].reads-before==48&&model[0].count==48);
 /* Another channel gets CPU time before the original backlog is drained. */
 receive(1,50);irq(1);irq(0);assert(model[0].reads-before==96&&!model[0].count);
 board_can_task();assert(broadcast_n==97);
}
static void test_tx_echo_and_standby_merge_order(void) {
 setup();
 for(unsigned i=0;i<200;i++)for(unsigned ch=0;ch<2;ch++) {
  time_us++;test_tim.CNT=(uint32_t)time_us;receive(ch,i*2+ch);irq(ch);
 }
 can_frame_t sent={.id=999,.len=8};port_can_tx(0,&sent);test_fdcan[0].TXEFS=1;
 time_us++;test_tim.CNT=(uint32_t)time_us;
 /* A mode change must retain the stopped channel's frames for common merge. */
 port_can_standby(1);assert(!broadcast_n);
 board_can_task();assert(broadcast_n==128);
 assert(completed[0]==1); /* Completion precedes the old RX logging tail. */
 for(unsigned i=0;i<3;i++)board_can_task();assert(broadcast_n==401);
 for(unsigned i=0;i<400;i++)assert(broadcast[i].id==i);
 assert(broadcast[400].id==999&&(broadcast[400].flags&CANF_TX));
 for(unsigned i=1;i<401;i++)assert(broadcast[i-1].ts_us<=broadcast[i].ts_us);
}

static void test_classic_and_fd_dlc(void) {
 static const uint8_t lengths[16]={0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
 setup();
 for(unsigned ch=0;ch<2;ch++)for(unsigned kind=0;kind<3;kind++)for(unsigned dlc=0;dlc<16;dlc++) {
  model_t *m=&model[ch];
  receive(ch,0x123);
  hw_frame_t *queued=&m->fifo[m->tail%48];
  queued->h.DataLength=dlc;
  queued->h.IdType=FDCAN_STANDARD_ID;
  queued->h.FDFormat=kind==2?FDCAN_FD_CAN:FDCAN_CLASSIC_CAN;
  queued->h.RxFrameType=kind==1?FDCAN_REMOTE_FRAME:0;
  queued->h.BitRateSwitch=0;
  queued->h.ErrorStateIndicator=0;
  unsigned previous=broadcast_n;
  irq(ch);board_can_task();assert(broadcast_n==previous+1);
  const can_frame_t *frame=&broadcast[previous];
  /* Classical DLC 9..15 still means eight bytes, including remote requests. */
  assert(frame->len==(kind==2?lengths[dlc]:(dlc>8?8:dlc)));
  assert(frame->flags==(kind==2?CANF_FD:kind==1?CANF_RTR:0));
  assert(frame->ch==ch&&frame->id==0x123);
 }
}

static void test_tx_enqueue_rejections(void) {
 setup();can_frame_t frame={.id=0x123,.len=8};
 assert(!port_can_tx(-1,&frame)&&!port_can_tx(2,&frame)&&!port_can_tx(0,NULL));
 assert(!port_can_tx(1,&frame)); /* Listen mode. */
 model[0].tx_fail=true;assert(!port_can_tx(0,&frame));
 model[0].tx_fail=false;
 /* A failed HAL enqueue must not occupy any of the eight marker slots. */
 for(unsigned slot=0;slot<8;slot++)assert(port_can_tx(0,&frame));
 assert(!port_can_tx(0,&frame));
 port_can_tx_abort(0);
 assert(port_can_tx(0,&frame));
 port_can_standby(0);assert(!port_can_tx(0,&frame));
}

static void test_tx_abort_completion_fence(void) {
 setup();assert(port_can_start(1,500000,2000000,true,CAN_MODE_NORMAL));
 can_frame_t old={.id=0x123,.len=1,.data={0x11}};
 can_frame_t fresh={.id=0x456,.len=1,.data={0x22}};
 assert(port_can_tx(0,&old));
 const uint32_t old_marker=model[0].tx_marker;
 model[0].defer_abort=true;
 port_can_tx_abort(0);
 /* Cancellation only requests TXBCR; no marker can be reused while the
  * controller can still publish an old completion. The other CAN is free. */
 assert(!port_can_tx(0,&fresh));
 assert(port_can_tx(1,&fresh));
 board_can_task();assert(!port_can_tx(0,&fresh));
 assert(completed[0]==0&&broadcast_n==0);
 /* The old transmission won the cancellation race. Its event becomes
  * readable only after hardware releases the pending request. */
 test_fdcan[0].TXBRP=0;
 model[0].tx_event_override=true;model[0].tx_event_marker=old_marker;
 test_fdcan[0].TXEFS=1;
 model[0].tx_event_fail=true;
 assert(!port_can_tx(0,&fresh));board_can_task();
 assert(!port_can_tx(0,&fresh));
 assert(completed[0]==0&&broadcast_n==0);
 model[0].tx_event_fail=false;
 assert(port_can_tx(0,&fresh));
 board_can_task();
 assert(completed[0]==0&&broadcast_n==0);
 /* Only the fresh hardware completion returns a new credit and echo. */
 test_fdcan[0].TXEFS=1;
 board_can_task();
 assert(completed[0]==1&&broadcast_n==1);
 assert(broadcast[0].id==fresh.id&&broadcast[0].data[0]==fresh.data[0]);
 assert(broadcast[0].flags&CANF_TX);
 /* A stop/start resets the controller and ends a stuck cancellation. */
 assert(port_can_tx(0,&old));port_can_tx_abort(0);
 assert(!port_can_tx(0,&fresh));
 assert(port_can_start(0,500000,2000000,true,CAN_MODE_NORMAL));
 assert(port_can_tx(0,&fresh));
}

static unsigned event_count(ev_type_t type,unsigned ch) {
 unsigned count=0;for(unsigned i=0;i<events_n;i++)if(events[i].type==type&&events[i].ch==ch)count++;
 return count;
}
static void test_status_failures_and_backpressure(void) {
 setup();events_n=0;
 model[0].counter_fail=true;time_us+=100000;board_can_task();
 assert(event_count(EV_CAN_STATUS,0)==0);
 model[0].counter_fail=false;model[0].status_fail=true;
 time_us+=100000;board_can_task();assert(event_count(EV_CAN_STATUS,0)==0);
 model[0].status_fail=false;model[0].status.BusOff=1;model[0].counters.TxErrorCnt=255;
 deny_events=true;time_us+=100000;board_can_task();
 model[0].status.BusOff=0;time_us+=100000;board_can_task();
 deny_events=false;board_can_task();
 bool busoff=false;
 for(unsigned i=0;i<events_n;i++)if(events[i].type==EV_CAN_STATUS&&events[i].ch==0)
  busoff|=(events[i].b&0x10000)!=0;
 assert(busoff);
 model[0].counters.TxErrorCnt=0;
}
static void test_probe_result_backpressure(void) {
 setup();receive(0,99);irq(0); /* Old rate's frame remains queued for recording. */
 port_can_probe(0,500000,2000000,false,CAN_MODE_LISTEN);events_n=0;
 receive(0,1);receive(0,2);irq(0);board_can_task();
 deny_events=true;time_us+=180000;board_can_task();
 receive(0,3);irq(0);board_can_task();
 deny_events=false;board_can_task();
 assert(event_count(EV_CAN_PROBE_RESULT,0)==1);
 for(unsigned i=0;i<events_n;i++)if(events[i].type==EV_CAN_PROBE_RESULT&&events[i].ch==0)
  assert(events[i].a==2); /* Queue congestion cannot extend the measured probe. */
 board_can_task();assert(event_count(EV_CAN_PROBE_RESULT,0)==1);
}
static void test_start_failures(void) {
 setup();can_frame_t frame={.id=0x123,.len=8};
 bool *faults[]={&model[0].init_fail,&model[0].tdc_fail,&model[0].start_fail};
 for(unsigned i=0;i<sizeof faults/sizeof faults[0];i++) {
  uint32_t old=port_can_generation(0);
  *faults[i]=true;
  assert(!port_can_start(0,500000,2000000,true,CAN_MODE_NORMAL));
  assert(!model[0].enabled&&model[1].enabled&&!port_can_tx(0,&frame));
  assert(port_can_generation(0)!=old);
  *faults[i]=false;
  assert(port_can_start(0,500000,2000000,true,CAN_MODE_NORMAL));
 }
 assert(!port_can_start(0,0,2000000,true,CAN_MODE_NORMAL));
 assert(!port_can_start(0,UINT32_C(1)<<30,2000000,true,CAN_MODE_NORMAL));
 assert(!port_can_start(0,UINT32_MAX,UINT32_MAX,true,CAN_MODE_NORMAL));
 assert(!model[0].enabled&&!port_can_tx(0,&frame));
 assert(port_can_start(0,500000,2000000,true,CAN_MODE_NORMAL));
 model[0].start_fail=true;assert(!port_can_recover(0));
 assert(!model[0].enabled&&!port_can_tx(0,&frame));
 assert(!port_can_probe(0,500000,2000000,false,CAN_MODE_LISTEN));
 model[0].start_fail=false;
 assert(port_can_start(0,500000,2000000,true,CAN_MODE_NORMAL));
}
static void test_reconfigure_discards_pending_events(void) {
 setup();assert(port_can_probe(0,500000,2000000,false,CAN_MODE_LISTEN));
 uint32_t old=port_can_generation(0);
 model[0].status.BusOff=1;deny_events=true;time_us+=180000;board_can_task();
 model[0].status.BusOff=0;
 assert(port_can_start(0,500000,2000000,true,CAN_MODE_NORMAL));
 assert(port_can_generation(0)!=old);
 events_n=0;deny_events=false;board_can_task();
 assert(event_count(EV_CAN_STATUS,0)==0&&event_count(EV_CAN_PROBE_RESULT,0)==0);
 time_us+=100000;board_can_task();
 for(unsigned i=0;i<events_n;i++)if(events[i].type==EV_CAN_STATUS&&events[i].ch==0) {
  assert(events[i].generation==port_can_generation(0));assert(!(events[i].b&0x10000));
 }
}

static can_ingress_t concurrent_q;
static void *producer(void *arg) {
 (void)arg;for(uint32_t i=0;i<100000;i++) {
  can_frame_t f={.id=i,.ts_us=i,.len=64};memset(f.data,(int)(i&255),64);
  while(!can_ingress_push(&concurrent_q,&f))sched_yield();
 }return NULL;
}
static void test_spsc_concurrent_and_index_wrap(void) {
 can_ingress_init(&concurrent_q);
 atomic_store(&concurrent_q.head,UINT32_MAX-7);atomic_store(&concurrent_q.tail,UINT32_MAX-7);
 pthread_t thread;assert(!pthread_create(&thread,NULL,producer,NULL));
 for(uint32_t i=0;i<100000;i++) {
  can_frame_t f;while(!can_ingress_peek(&concurrent_q,&f))sched_yield();
  assert(f.id==i&&f.ts_us==i);for(unsigned j=0;j<64;j++)assert(f.data[j]==(i&255));
  can_ingress_advance(&concurrent_q);
 }
 assert(!pthread_join(thread,NULL));assert(!can_ingress_level(&concurrent_q));
 assert(can_ingress_high_water(&concurrent_q)<=CAN_INGRESS_LEN);
}
int main(void) {
 test_service_latency_metrics();
 test_status_failures_and_backpressure();
 test_probe_result_backpressure();
 test_start_failures();
 test_reconfigure_discards_pending_events();
 test_tx_abort_completion_fence();
 test_classic_and_fd_dlc();
 test_tx_enqueue_rejections();
 test_stall_dual();test_overflow_and_retry();test_ring_full_wrap_and_restart();
 test_tx_and_rx_errors();test_bounded_irq_under_refill();test_tx_echo_and_standby_merge_order();
 test_spsc_concurrent_and_index_wrap();
 printf("CAN ingress: 15 scenarios, 96 DLC cases + real production ISR/dispatch + 100000 concurrent SPSC frames passed\n");return 0;
}
