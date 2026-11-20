/* SPDX-License-Identifier: MIT
 * Production FDCAN ISR/ingress/ring -> storage/diskio/FatFs and production
 * display/LVGL in one deterministic wall-clock model. All media is RAM only.
 * HAL_GetTick is a pure read; time is advanced by the explicit event scheduler,
 * cooperative polling cost, configured foreground cost and DMA wire duration.
 */
#include "stm32h7xx_hal.h"
#include "storage_sd.h"
#include "board.h"
#include "can_ingress.h"
#include "services/core/port.h"
#include "services/core/app_evt.h"
#include "services/frame_ring/frame_ring.h"
#include "services/player/play_sm.h"
#include "services/config/cfg.h"
#include "services/logfmt/candump.h"
#include "sim/send_plan.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
/* White-box seam ONLY for emitting deterministic LVGL redraws and checking
 * DMA snapshot ownership. The actual display transport is unchanged. */
#include "../../platform/stm32h743_revd/display.c"

#define CARD_BLOCKS 32768u
#define MAX_FRAMES 300000u
#ifndef STRESS_MAIN_STEP
#define STRESS_MAIN_STEP() main_step()
#endif
#ifndef STRESS_REPORT_EXTRA
#define STRESS_REPORT_EXTRA() ((void)0)
#endif
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"stress assertion line %d: %s\n",__LINE__,#x); exit(2); } } while (0)
static uint8_t ram_card[CARD_BLOCKS * 512u];
SPI_TypeDef test_spi4;
TIM_TypeDef test_tim1, test_tim;
DMA_Stream_TypeDef test_stream0;
uint32_t test_compare;
GPIO_TypeDef stress_gpio[3];
FDCAN_GlobalTypeDef test_fdcan[2];
static uint64_t clock_us, start_us, finish_us, next_frame_us[2], next_redraw_us;
enum { OP_WRITE, OP_SYNC, OP_CLOSE };
static unsigned storage_operation, busy_target=OP_WRITE;
static uint32_t fps=2500, duration_ms=5000, busy_ms=250, rotate_frames=200;
static uint32_t lcd_cpu_us=200, cpu_loop_us=40, encode_cpu_us=10, segment=1, next_rotate;
static uint32_t max_segment=1, source_limit, observed_faults, duplicate_error, premature_error;
static uint32_t injected[2], accepted[2], rejected[2], oncard[2];
static uint32_t *expected_segment[2], *seen[2];
static bool generating, recording_live, busy_injected, in_irq, sd_irq, display_running;
static bool pumping, expect_fault, card_full, close_started;
/* Pipeline model has no file UI/model requests. Production storage still
 * links its real JSON codec, validators and FatFs long-name implementation. */
bool sp_io_peek(sp_io_request_t *out){(void)out;return false;}
int sp_io_complete(uint32_t id,int result,const char *message){(void)id;(void)message;return result;}
unsigned sp_catalog_count(void){return 0;}
bool sp_catalog_get(unsigned index,sp_file_t *out){(void)index;(void)out;return false;}
int sp_catalog_replace(const sp_file_t *files,uint32_t count){(void)files;return count?SP_ERR_FILE:0;}
const sp_status_t *sp_state(void){static const sp_status_t idle={0};return &idle;}
static uint64_t program_busy_until, longest_pump_gap, last_pump_us, max_loop_us;
static uint64_t observed_ui_at, observed_ui_gap, control_probe_due, control_probe_gap;
static uint32_t event_count[EV_COUNT];
static int32_t event_result[EV_COUNT];
static uint64_t error_time_us;
static uint32_t error_tick_ms;
static player_t players[2];
static power_t test_power = {.screen_on=true};
static ring_reader_t recorder;
static lv_obj_t *label;
static uint32_t redraws, spi_transfers, spi_snapshot_errors, sd_writes;
static uint32_t preclose_accepted, after_close_reject;
static struct { bool pending; SPI_HandleTypeDef *handle; const uint8_t *data;
 uint16_t count; uint32_t crc; uint64_t due; } spi_hw;
static struct { bool pending, writing; SD_HandleTypeDef *handle; uint8_t *data;
 uint32_t lba,count; uint64_t due; } sd_hw;
typedef struct { FDCAN_RxHeaderTypeDef header; uint8_t data[64]; } hw_frame_t;
static struct { hw_frame_t queue[48]; unsigned in,out,count; uint32_t flags,notify;
 bool enabled; FDCAN_HandleTypeDef *handle; uint32_t hardware_lost; } can_hw[2];

static uint32_t crc32(const uint8_t *p,size_t n) {
 uint32_t crc=0xffffffffu; while(n--) { crc^=*p++; for(unsigned i=0;i<8;i++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u); } return ~crc;
}
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8u*i));}
static void put64(uint8_t *p,uint64_t n) {for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(8u*i));}
static uint64_t get64(const uint8_t *p) {uint64_t n=0;for(unsigned i=0;i<8;i++)n|=(uint64_t)p[i]<<(8u*i);return n;}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void payload(uint8_t p[64],unsigned ch,uint32_t seq,uint64_t at) {
 const uint64_t run_id=UINT64_C(0x5245564400000001);
 memset(p,0,64);memcpy(p,"RVDB",4);p[4]=1;p[5]=(uint8_t)ch;
 put64(p+8,run_id);put64(p+16,seq);put64(p+24,(at-start_us)*1000u);
 for(unsigned i=0;i<28;i++)p[32+i]=(uint8_t)(run_id+(uint64_t)seq*17u+ch*31u+i*13u);
 put32(p+60,crc32(p,60));
}
static void payload_fixture_check(void) {
 static const uint8_t expected[64]={0x52,0x56,0x44,0x42,0x01,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x44,0x56,0x45,0x52,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0xc2,0x67,0x00,0x00,0x00,0x00,0x00,0x41,0x4e,0x5b,0x68,0x75,0x82,0x8f,0x9c,0xa9,0xb6,0xc3,0xd0,0xdd,0xea,0xf7,0x04,0x11,0x1e,0x2b,0x38,0x45,0x52,0x5f,0x6c,0x79,0x86,0x93,0xa0,0x39,0xe5,0x6e,0x60};
 uint8_t actual[64];payload(actual,1,17,start_us+6800u);REQUIRE(!memcmp(actual,expected,64));
}
static unsigned can_index(FDCAN_HandleTypeDef *h) { REQUIRE(h->Instance==FDCAN1||h->Instance==FDCAN2);return h->Instance==FDCAN2; }
extern void FDCAN1_IT0_IRQHandler(void),FDCAN2_IT0_IRQHandler(void),SDMMC1_IRQHandler(void);
static void run_can_irq(unsigned ch) {
 if(!can_hw[ch].enabled)return;
 in_irq=true; if(ch)FDCAN2_IT0_IRQHandler();else FDCAN1_IT0_IRQHandler();in_irq=false;
}
static void inject(unsigned ch,uint64_t at) {
 uint32_t seq=injected[ch]++;
 REQUIRE(seq<MAX_FRAMES);
 if(can_hw[ch].count==48) {can_hw[ch].hardware_lost++;can_hw[ch].flags|=FDCAN_IT_RX_FIFO0_MESSAGE_LOST;return;}
 hw_frame_t *f=&can_hw[ch].queue[can_hw[ch].in++%48u];
 *f=(hw_frame_t){.header={.Identifier=0x5a0u+ch,.DataLength=15,.FDFormat=FDCAN_FD_CAN,.BitRateSwitch=FDCAN_BRS_ON}};
 payload(f->data,ch,seq,at);can_hw[ch].count++;can_hw[ch].flags|=FDCAN_IT_RX_FIFO0_NEW_MESSAGE;
 test_tim.CNT=(uint32_t)at;run_can_irq(ch);
}
static void advance(uint64_t delta) {
 uint64_t until=clock_us+delta;
 while(clock_us<until) {
  uint64_t at=until;
  if(generating)for(unsigned ch=0;ch<2;ch++)if(next_frame_us[ch]<at&&next_frame_us[ch]<finish_us)at=next_frame_us[ch];
  if(sd_hw.pending&&sd_hw.due<at)at=sd_hw.due;
  if(spi_hw.pending&&spi_hw.due<at)at=spi_hw.due;
  clock_us=at;test_tim.CNT=(uint32_t)clock_us;
  if(sd_hw.pending&&sd_hw.due<=clock_us&&sd_irq)SDMMC1_IRQHandler();
  if(spi_hw.pending&&spi_hw.due<=clock_us) {
   if(crc32(spi_hw.data,spi_hw.count)!=spi_hw.crc)spi_snapshot_errors++;
   spi_hw.pending=false;HAL_SPI_TxCpltCallback(spi_hw.handle);
  }
  if(generating)for(unsigned ch=0;ch<2;ch++)if(next_frame_us[ch]<=clock_us&&next_frame_us[ch]<finish_us) {
   inject(ch,next_frame_us[ch]); next_frame_us[ch]=start_us+((uint64_t)injected[ch]*1000000u)/fps+(ch?1u:0u);
  }
 }
}
/* Host-only compile seam renames ONLY storage_sd.c's encoder call. The actual
 * BSD codec still runs, plus an explicit configurable CPU cost on the model
 * time line. Interrupt arrivals continue; foreground draining cannot recurse. */
int stress_format_line(const can_frame_t *frame,char *line,size_t capacity) {
 advance(encode_cpu_us);return logfmt_format_line(frame,line,capacity);
}
/* Host-only FatFs call seams select where long-busy faults occur; each calls
 * the unmodified FatFs API and restores context before returning. */
FRESULT stress_f_write(FIL *file,const void *data,UINT bytes,UINT *written) {
 unsigned previous=storage_operation;storage_operation=OP_WRITE;
 FRESULT result=f_write(file,data,bytes,written);storage_operation=previous;return result;
}
FRESULT stress_f_sync(FIL *file) {
 unsigned previous=storage_operation;storage_operation=OP_SYNC;
 FRESULT result=f_sync(file);storage_operation=previous;return result;
}
FRESULT stress_f_close(FIL *file) {
 unsigned previous=storage_operation;storage_operation=OP_CLOSE;
 FRESULT result=f_close(file);storage_operation=previous;return result;
}
static void capture(void) {
 if (generating) {
  if(last_pump_us&&clock_us-last_pump_us>longest_pump_gap)longest_pump_gap=clock_us-last_pump_us;
  last_pump_us=clock_us;
 }
 board_can_task();const can_frame_t *f;
 while((f=ring_peek(&recorder))) {
  uint32_t seq=(uint32_t)get64(f->data+16);unsigned ch=f->ch;REQUIRE(ch<2&&seq<MAX_FRAMES);
  if(port_sd_record_frame(f)) {accepted[ch]++;expected_segment[ch][seq]=segment;}
  else {rejected[ch]++;if(close_started)after_close_reject++;}
  ring_advance(&recorder);
 }
}
void revd_io_wait_poll(void) {
 if(pumping)return;pumping=true;
 advance(display_running?lcd_cpu_us:50u);
 capture();display_dma_poll(HAL_GetTick());pumping=false;
}
uint64_t board_time_us(void) {REQUIRE(!in_irq);return clock_us;}
uint32_t HAL_GetTick(void) {return (uint32_t)(clock_us/1000u);}
void HAL_Delay(uint32_t ms) {advance((uint64_t)ms*1000u);}
uint32_t __get_IPSR(void) {return in_irq?1u:0u;}
void test_clock_enable(void) { }
void HAL_GPIO_Init(void *g,GPIO_InitTypeDef *p) {(void)g;(void)p;}
void HAL_GPIO_WritePin(void *g,uint32_t p,GPIO_PinState value) {(void)g;(void)p;(void)value;}
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *g,uint16_t p) {(void)g;(void)p;return GPIO_PIN_RESET;}
void HAL_NVIC_SetPriority(int i,uint32_t p,uint32_t s) {(void)i;(void)p;(void)s;}
void HAL_NVIC_EnableIRQ(int i) {if(i==SDMMC1_IRQn)sd_irq=true;else if(i<2)can_hw[i].enabled=true;}
void HAL_NVIC_DisableIRQ(int i) {if(i==SDMMC1_IRQn)sd_irq=false;else if(i<2)can_hw[i].enabled=false;}
void HAL_NVIC_ClearPendingIRQ(int i) {(void)i;}
void HAL_NVIC_SetPendingIRQ(int i) {(void)i;}
HAL_StatusTypeDef HAL_FDCAN_Init(FDCAN_HandleTypeDef *h) {can_hw[can_index(h)].handle=h;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_DeInit(FDCAN_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_Stop(FDCAN_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(FDCAN_HandleTypeDef *h,uint32_t a,uint32_t b,uint32_t c,uint32_t d) {(void)h;(void)a;(void)b;(void)c;(void)d;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ConfigRxFifoOverwrite(FDCAN_HandleTypeDef *h,uint32_t f,uint32_t m) {(void)h;(void)f;(void)m;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ConfigInterruptLines(FDCAN_HandleTypeDef *h,uint32_t f,uint32_t l) {(void)h;(void)f;(void)l;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ActivateNotification(FDCAN_HandleTypeDef *h,uint32_t f,uint32_t t) {(void)t;can_hw[can_index(h)].notify=f;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_ConfigTxDelayCompensation(FDCAN_HandleTypeDef *h,uint32_t a,uint32_t b) {(void)h;(void)a;(void)b;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_EnableTxDelayCompensation(FDCAN_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_AbortTxRequest(FDCAN_HandleTypeDef *h,uint32_t n) {(void)h;(void)n;return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *h,FDCAN_TxHeaderTypeDef *p,uint8_t *d) {(void)h;(void)p;(void)d;return HAL_ERROR;}
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *h,uint32_t f) {(void)f;return can_hw[can_index(h)].count;}
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef *h,uint32_t f,FDCAN_RxHeaderTypeDef *p,uint8_t *d) {
 (void)f;unsigned ch=can_index(h);REQUIRE(in_irq&&can_hw[ch].count);
 hw_frame_t *q=&can_hw[ch].queue[can_hw[ch].out++%48u];can_hw[ch].count--;*p=q->header;memcpy(d,q->data,64);return HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_GetTxEvent(FDCAN_HandleTypeDef *h,FDCAN_TxEventFifoTypeDef *e) {(void)h;(void)e;return HAL_ERROR;}
HAL_StatusTypeDef HAL_FDCAN_GetErrorCounters(FDCAN_HandleTypeDef *h,FDCAN_ErrorCountersTypeDef *e) {(void)h;*e=(FDCAN_ErrorCountersTypeDef){0};return HAL_OK;}
HAL_StatusTypeDef HAL_FDCAN_GetProtocolStatus(FDCAN_HandleTypeDef *h,FDCAN_ProtocolStatusTypeDef *s) {(void)h;*s=(FDCAN_ProtocolStatusTypeDef){0};return HAL_OK;}
void HAL_FDCAN_IRQHandler(FDCAN_HandleTypeDef *h) {unsigned ch=can_index(h);uint32_t flags=can_hw[ch].flags&can_hw[ch].notify;can_hw[ch].flags&=~flags;if(flags)HAL_FDCAN_RxFifo0Callback(h,flags);}
HAL_StatusTypeDef HAL_SD_Init(SD_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_SD_DeInit(SD_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_SD_ConfigWideBusOperation(SD_HandleTypeDef *h,uint32_t w) {(void)h;(void)w;return HAL_OK;}
HAL_StatusTypeDef HAL_SD_GetCardInfo(const SD_HandleTypeDef *h,HAL_SD_CardInfoTypeDef *p) {(void)h;p->LogBlockSize=512;p->LogBlockNbr=CARD_BLOCKS;return HAL_OK;}
HAL_SD_CardStateTypeDef HAL_SD_GetCardState(SD_HandleTypeDef *h) {(void)h;return clock_us<program_busy_until?0u:HAL_SD_CARD_TRANSFER;}
static HAL_StatusTypeDef sd_start(SD_HandleTypeDef *h,uint8_t *data,uint32_t lba,uint32_t count,bool writing) {
 REQUIRE(!sd_hw.pending&&((uintptr_t)data&31u)==0&&count<=8u);
 REQUIRE(lba<CARD_BLOCKS&&count&&count<=CARD_BLOCKS-lba);
 sd_hw.pending=true;sd_hw.writing=writing;sd_hw.handle=h;sd_hw.data=data;sd_hw.lba=lba;sd_hw.count=count;
 sd_hw.due=clock_us+30u+((uint64_t)count*512u*1000000u+6249999u)/6250000u;
 return HAL_OK;
}
HAL_StatusTypeDef HAL_SD_ReadBlocks_DMA(SD_HandleTypeDef *h,uint8_t *p,uint32_t lba,uint32_t n) {return sd_start(h,p,lba,n,false);}
HAL_StatusTypeDef HAL_SD_WriteBlocks_DMA(SD_HandleTypeDef *h,const uint8_t *p,uint32_t lba,uint32_t n) {return sd_start(h,(uint8_t *)(uintptr_t)p,lba,n,true);}
HAL_StatusTypeDef HAL_SD_Abort_IT(SD_HandleTypeDef *h) {(void)h;sd_hw.pending=false;return HAL_OK;}
void test_sd_force_reset(void) {sd_hw.pending=false;}
void HAL_SD_IRQHandler(SD_HandleTypeDef *h) {
 REQUIRE(sd_hw.pending&&h==sd_hw.handle);sd_hw.pending=false;
 uint8_t *card=ram_card+(size_t)sd_hw.lba*512u;size_t bytes=(size_t)sd_hw.count*512u;
 if(sd_hw.writing) {
  memcpy(card,sd_hw.data,bytes);sd_writes++;
  if(recording_live&&!busy_injected&&storage_operation==busy_target&&accepted[0]+accepted[1]>=100u&&busy_ms) {program_busy_until=clock_us+(uint64_t)busy_ms*1000u;busy_injected=true;}
  else program_busy_until=clock_us+100u;
  HAL_SD_TxCpltCallback(h);
 } else {memcpy(sd_hw.data,card,bytes);HAL_SD_RxCpltCallback(h);}
}
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h,uint8_t *p,uint16_t n,uint32_t t) {(void)h;(void)p;(void)n;(void)t;REQUIRE(!recording_live);return HAL_OK;}
HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *h,const uint8_t *p,uint16_t n) {
 REQUIRE(!spi_hw.pending);spi_hw.pending=true;spi_hw.handle=h;spi_hw.data=p;spi_hw.count=n;spi_hw.crc=crc32(p,n);
 spi_hw.due=clock_us+2u+((uint64_t)n*8u*1000000u+12499999u)/12500000u;spi_transfers++;return HAL_OK;
}
void HAL_SPI_IRQHandler(SPI_HandleTypeDef *h) {(void)h;}
HAL_StatusTypeDef HAL_DMA_Init(DMA_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_DMA_DeInit(DMA_HandleTypeDef *h) {(void)h;spi_hw.pending=false;return HAL_OK;}
void HAL_DMA_IRQHandler(DMA_HandleTypeDef *h) {(void)h;}
HAL_StatusTypeDef HAL_TIM_PWM_Init(TIM_HandleTypeDef *h) {(void)h;return HAL_OK;}
HAL_StatusTypeDef HAL_TIM_PWM_ConfigChannel(TIM_HandleTypeDef *h,TIM_OC_InitTypeDef *o,uint32_t c) {(void)h;(void)o;(void)c;return HAL_OK;}
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *h,uint32_t c) {(void)h;(void)c;return HAL_OK;}
HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef *h,uint32_t c) {(void)h;(void)c;return HAL_OK;}
HAL_StatusTypeDef HAL_TIM_PWM_DeInit(TIM_HandleTypeDef *h) {(void)h;return HAL_OK;}
bool touch_ft6336_init(void) {return true;}
bool touch_ft6336_read(uint16_t *x,uint16_t *y,bool *p) {*x=*y=0;*p=false;return true;}
void touch_ft6336_deinit(void) { }
const power_t *power_get(void) {return &test_power;}
void app_touch(ev_type_t e,int32_t a) {(void)e;(void)a;}
/* This pipeline model observes completions without linking product TX state. */
void app_tx_complete(int ch) {(void)evt_post4(EV_CAN_TX_DONE,(uint8_t)ch,SRC_SYS,0,0);}
void ui_lvgl_init(lv_display_t *d) {label=lv_label_create(lv_display_get_screen_active(d));lv_label_set_text(label,"Dual CAN FD SD stress");}
void ui_lvgl_task(uint32_t ms) {(void)ms;}
uint32_t ui_lvgl_snapshot_errors(void) {return 0;}
bool evt_post4(ev_type_t e,uint8_t ch,src_t s,int32_t a,int32_t b) {
 (void)ch;(void)s;(void)b;REQUIRE(!in_irq);event_count[e]++;event_result[e]=a;
 if(e==EV_SD_WRITE_ERR) {
  if(a==1) {
   if(error_time_us==clock_us)duplicate_error++;
   /* Driver throttles with uint32 HAL milliseconds. Sub-millisecond event
    * phases can make 200 tick steps take 199001..200999 model microseconds. */
   if(error_time_us&&(uint32_t)(HAL_GetTick()-error_tick_ms)<200u)premature_error++;
   error_time_us=clock_us;
   error_tick_ms=HAL_GetTick();
  }
  observed_faults++;
 }
 return true;
}
void evt_msg(msg_t m,uint8_t ch,int32_t b) {(void)m;(void)ch;(void)b;}
bool evt_post(const app_evt_t *event) {
 return evt_post4((ev_type_t)event->type,event->ch,(src_t)event->src,event->a,event->b);
}
const player_t *play_get(int ch) {return &players[ch];}
bool play_any_active(void) {return false;}
bool play_file_in_use(int n) {(void)n;return false;}
void play_file_removed(int n) {(void)n;}
static void await_event(ev_type_t e,uint32_t previous) {
 uint64_t deadline=clock_us+10000000u;
 while(event_count[e]==previous&&clock_us<deadline) {storage_sd_task();advance(50);}
 REQUIRE(event_count[e]>previous);
}
/* Observation only: identical entry/marker semantics in both schedulers. No
 * virtual time is charged and this is not the complete product UI/USB path. */
static void observe_control_probe(void) {
 if(clock_us>=control_probe_due) {
  if(clock_us-control_probe_due>control_probe_gap)control_probe_gap=clock_us-control_probe_due;
  control_probe_due=clock_us+10000u;
 }
}
static void observe_ui_entry(void) {
 if(observed_ui_at&&clock_us-observed_ui_at>observed_ui_gap)observed_ui_gap=clock_us-observed_ui_at;
 observed_ui_at=clock_us;
}
static void main_step(void) {
 uint64_t before=clock_us;
 advance(cpu_loop_us);capture();observe_control_probe();
 if(generating&&accepted[0]+accepted[1]>=next_rotate&&segment<max_segment&&!close_started) {
  port_sd_next_segment(++segment);next_rotate=accepted[0]+accepted[1]+rotate_frames;
 }
 storage_sd_task();
 if(clock_us>=next_redraw_us) {
  next_redraw_us=clock_us+20000u;redraws++;char text[64];snprintf(text,sizeof text,"Dual CAN stress %lu",(unsigned long)redraws);
  lv_label_set_text(label,text);lv_obj_invalidate(lv_display_get_screen_active(lcd));lv_timer_ready(lv_display_get_refr_timer(lcd));
 }
 observe_ui_entry();display_running=true;display_task(HAL_GetTick());display_running=false;
 if(clock_us-before>max_loop_us)max_loop_us=clock_us-before;
}
static void leave_small_free(void) {
 DWORD clusters;FATFS *vol;REQUIRE(f_getfree("0:",&clusters,&vol)==FR_OK);
 FSIZE_t leave=128u*1024u;FSIZE_t freebytes=(FSIZE_t)clusters*vol->csize*512u;
 FIL filler;REQUIRE(f_open(&filler,"0:/FILL.BIN",FA_CREATE_ALWAYS|FA_WRITE)==FR_OK);
 REQUIRE(f_expand(&filler,freebytes-leave,1)==FR_OK);REQUIRE(f_close(&filler)==FR_OK);
}
static uint32_t line_errors,crc_errors,sequence_errors,boundary_errors,read_errors,duplicates,truncated_tails;
static uint32_t payload_errors,timestamp_errors,sequence_gaps,last_sequence[2];
static bool have_sequence[2],have_time;
static uint64_t previous_log_time;
static void check_line(const char *line,size_t n,uint32_t file_segment) {
 can_frame_t f;logfmt_result_t result=logfmt_parse_line(line,n,&f);
 if(result==LOGFMT_SKIP)return;
 if(result!=LOGFMT_OK||f.ch>=2||f.len!=64||memcmp(f.data,"RVDB",4)||f.data[4]!=1||f.data[5]!=f.ch) {line_errors++;return;}
 uint32_t seq=(uint32_t)get64(f.data+16);unsigned ch=f.ch;
 if(seq>=injected[ch]) {sequence_errors++;return;}
 if(crc32(f.data,60)!=get32(f.data+60))crc_errors++;
 uint64_t planned=((uint64_t)seq*1000000u)/fps+(ch?1u:0u);
 uint8_t expected[64];payload(expected,ch,seq,start_us+planned);
 if(memcmp(expected,f.data,64)||f.id!=0x5a0u+ch||f.flags!=(CANF_FD|CANF_BRS))payload_errors++;
 if(f.ts_us!=start_us+planned||(have_time&&f.ts_us<previous_log_time))timestamp_errors++;
 previous_log_time=f.ts_us;have_time=true;
 if(have_sequence[ch]&&seq<=last_sequence[ch])sequence_errors++;
 else if(have_sequence[ch])sequence_gaps+=seq-last_sequence[ch]-1u;
 else sequence_gaps+=seq;
 last_sequence[ch]=seq;have_sequence[ch]=true;
 if(seen[ch][seq]++)duplicates++;
 if(!expected_segment[ch][seq]||expected_segment[ch][seq]!=file_segment)boundary_errors++;
 oncard[ch]++;
}
static void inspect_card(void) {
 for(uint32_t seg=1;seg<=segment;seg++) {
  char path[80];snprintf(path,sizeof path,"0:/CANLOG/S0000001/%08lu.LOG",(unsigned long)seg);FIL file;
  if(f_open(&file,path,FA_READ)!=FR_OK) {read_errors++;continue;}
  char line[LOGFMT_MAX_LINE+1];size_t used=0;uint8_t block[4096];UINT n;
  for(;;) {
   FRESULT result=f_read(&file,block,sizeof block,&n);if(result!=FR_OK) {read_errors++;break;}if(!n)break;
   for(UINT i=0;i<n;i++) {
    if(used>=LOGFMT_MAX_LINE) {line_errors++;used=0;}
    line[used++]=(char)block[i];if(block[i]=='\n') {line[used]=0;check_line(line,used,seg);used=0;}
   }
  }
  if(used)truncated_tails++;if(f_close(&file)!=FR_OK)read_errors++;
 }
}
static uint32_t number(const char *s) {char *end;unsigned long n=strtoul(s,&end,10);REQUIRE(*s&&!*end&&n<=UINT32_MAX);return (uint32_t)n;}
int main(int argc,char **argv) {
 const char *name="custom";
 for(int i=1;i<argc;i++) {
  REQUIRE(i+1<argc);const char *key=argv[i++],*value=argv[i];
  if(!strcmp(key,"--name"))name=value;
  else if(!strcmp(key,"--fps"))fps=number(value);
  else if(!strcmp(key,"--duration-ms"))duration_ms=number(value);
  else if(!strcmp(key,"--busy-ms"))busy_ms=number(value);
  else if(!strcmp(key,"--busy-target")) {
   if(!strcmp(value,"write"))busy_target=OP_WRITE;
   else if(!strcmp(value,"sync"))busy_target=OP_SYNC;
   else if(!strcmp(value,"close"))busy_target=OP_CLOSE;
   else REQUIRE(false);
  }
  else if(!strcmp(key,"--rotate-frames"))rotate_frames=number(value);
  else if(!strcmp(key,"--segments"))max_segment=number(value);
  else if(!strcmp(key,"--lcd-cpu-us"))lcd_cpu_us=number(value);
  else if(!strcmp(key,"--loop-cpu-us"))cpu_loop_us=number(value);
  else if(!strcmp(key,"--encode-cpu-us"))encode_cpu_us=number(value);
  else if(!strcmp(key,"--expect-fault"))expect_fault=number(value)!=0;
  else if(!strcmp(key,"--card-full"))card_full=number(value)!=0;
  else if(!strcmp(key,"--clock-start-ms"))clock_us=(uint64_t)number(value)*1000u;
  else {fprintf(stderr,"unknown argument %s\n",key);return 2;}
 }
 payload_fixture_check();
 REQUIRE(fps&&fps<=100000&&duration_ms&&rotate_frames&&max_segment<=10000);
 source_limit=(uint32_t)(((uint64_t)fps*duration_ms+999u)/1000u);REQUIRE(source_limit<MAX_FRAMES);
 for(unsigned ch=0;ch<2;ch++) {expected_segment[ch]=calloc(MAX_FRAMES,sizeof(uint32_t));seen[ch]=calloc(MAX_FRAMES,sizeof(uint32_t));REQUIRE(expected_segment[ch]&&seen[ch]);}
 cfg_defaults();ring_init();ring_reader_attach(&recorder,true);storage_sd_init();
 uint32_t before=event_count[EV_SD_FORMAT_DONE];port_sd_format();await_event(EV_SD_FORMAT_DONE,before);REQUIRE(event_result[EV_SD_FORMAT_DONE]==SD_OK);
 if(card_full)leave_small_free();
 before=event_count[EV_SD_PREALLOC_DONE];port_sd_prealloc(1,1,64,500000,5000000,true);await_event(EV_SD_PREALLOC_DONE,before);REQUIRE(event_result[EV_SD_PREALLOC_DONE]==SD_OK);
 REQUIRE(display_init());display_set_backlight(50);
 port_can_start(0,500000,5000000,true,CAN_MODE_LISTEN);port_can_start(1,500000,5000000,true,CAN_MODE_LISTEN);
 start_us=clock_us+1000u;finish_us=start_us+(uint64_t)duration_ms*1000u;next_frame_us[0]=start_us;next_frame_us[1]=start_us+1u;next_redraw_us=start_us;
 control_probe_due=start_us;
 next_rotate=rotate_frames;recording_live=generating=true;
 while(clock_us<finish_us)STRESS_MAIN_STEP();generating=false;
 /* All IRQ ingress is drained before STOP's immutable accepted-frame fence. */
 for(unsigned i=0;i<32;i++) {capture();advance(50);}
 preclose_accepted=accepted[0]+accepted[1];before=event_count[EV_SD_CLOSE_DONE];port_sd_close();close_started=true;
 /* A post-close producer must be rejected and cannot move the close fence. */
 can_frame_t after={.ch=0,.len=64,.flags=CANF_FD};REQUIRE(!port_sd_record_frame(&after));
 uint64_t close_at=clock_us,deadline=clock_us+10000000u;
 while(event_count[EV_SD_CLOSE_DONE]==before&&clock_us<deadline)STRESS_MAIN_STEP();
 REQUIRE(event_count[EV_SD_CLOSE_DONE]>before);uint64_t close_latency=clock_us-close_at;
 recording_live=false;display_poweroff();
 storage_sd_stats_t sd_stats;storage_sd_stats(&sd_stats);board_can_metrics_t cm[2];for(unsigned ch=0;ch<2;ch++)REQUIRE(board_can_metrics(ch,&cm[ch]));
 /* Faults quiesce the controller. Re-mount RAM image ONLY after capture stops;
  * never mistake a sticky invalid FIL for successfully closed durable data. */
 if(!storage_sd_disk_ready()) {port_sd_unmount();for(unsigned i=0;i<8;i++)storage_sd_task();before=event_count[EV_SD_MOUNT_DONE];port_sd_mount();await_event(EV_SD_MOUNT_DONE,before);}
 inspect_card();
 bool catalog_recount=true;
 if(segment>128u) {
  port_sd_unmount();for(unsigned i=0;i<8;i++)storage_sd_task();
  before=event_count[EV_SD_MOUNT_DONE];port_sd_mount();await_event(EV_SD_MOUNT_DONE,before);
  storage_sd_stats_t recounted;storage_sd_stats(&recounted);
  catalog_recount=event_result[EV_SD_MOUNT_DONE]==SD_OK&&port_sd_file_count()==128&&
      recounted.catalog_omitted==segment-128u&&recounted.catalog_limited&&port_sd_next_session()==2u;
 }
 uint32_t missing=0;for(unsigned ch=0;ch<2;ch++)for(uint32_t seq=0;seq<injected[ch];seq++)if(expected_segment[ch][seq]&&!seen[ch][seq])missing++;
 uint64_t losses=(uint64_t)cm[0].ingress_drops+cm[1].ingress_drops+cm[0].ring_drops+cm[1].ring_drops+can_hw[0].hardware_lost+can_hw[1].hardware_lost+rejected[0]+rejected[1]+missing;
 bool integrity=!line_errors&&!crc_errors&&!payload_errors&&!timestamp_errors&&!sequence_errors&&!boundary_errors&&!duplicates&&!spi_snapshot_errors&&catalog_recount;
 bool zero_loss=integrity&&!truncated_tails&&!read_errors&&!losses&&!sd_stats.queue_overflows&&!sd_stats.discarded_frames&&!sd_stats.dma_errors&&event_result[EV_SD_CLOSE_DONE]==SD_OK&&oncard[0]==injected[0]&&oncard[1]==injected[1];
 bool detected_fault=observed_faults||event_result[EV_SD_CLOSE_DONE]!=SD_OK||event_count[EV_SD_FULL]||sd_stats.queue_overflows;
 bool fault_contract=true;
 if(busy_ms>2000u)fault_contract=sd_stats.card_busy_timeouts>0&&sd_stats.dma_errors>0;
 else if(expect_fault&&!card_full)fault_contract=sd_stats.queue_overflows>0&&!sd_stats.dma_errors;
 if(card_full)fault_contract=event_count[EV_SD_FULL]>0&&event_result[EV_SD_CLOSE_DONE]!=SD_OK;
 bool injection_contract=!busy_ms||busy_injected;
 bool pass=injection_contract&&(expect_fault?(detected_fault&&fault_contract&&!zero_loss&&!premature_error&&integrity):
     (zero_loss&&!premature_error&&segment==max_segment));
 printf("{\"scenario\":\"%s\",\"model\":\"production-pipeline-ram-card\",\"expectation\":\"%s\",\"pass\":%s,\"zero_loss\":%s,\"fps_per_channel\":%u,\"duration_ms\":%u,\"injected_busy_ms\":%u,\"busy_injected\":%s,\"lcd_cpu_us_per_yield\":%u,",name,expect_fault?"fault-must-be-detected":"strict-zero-loss",pass?"true":"false",zero_loss?"true":"false",fps,duration_ms,busy_ms,busy_injected?"true":"false",lcd_cpu_us);
 printf("\"injected\":[%u,%u],\"accepted\":[%u,%u],\"rejected\":[%u,%u],\"on_card\":[%u,%u],\"accepted_missing\":%u,\"encoded\":%u,\"discarded\":%u,\"queue_peak\":%u,\"queue_overflows\":%u,",injected[0],injected[1],accepted[0],accepted[1],rejected[0],rejected[1],oncard[0],oncard[1],missing,sd_stats.encoded_frames,sd_stats.discarded_frames,sd_stats.queue_high_water,sd_stats.queue_overflows);
 printf("\"queue_capacity\":%u,\"ingress_capacity\":%u,",REVD_SD_RECORD_QUEUE_FRAMES,CAN_INGRESS_LEN);
 printf("\"max_ui_service_gap_us\":%" PRIu64 ",\"max_synthetic_control_probe_us\":%" PRIu64 ",",observed_ui_gap,control_probe_gap);
 printf("\"segments_requested\":%u,\"segments_opened\":%u,\"segments_closed\":%u,\"catalog_limited\":%u,\"catalog_omitted\":%u,\"dma_errors\":%u,\"dma_timeouts\":%u,\"card_busy_timeouts\":%u,\"max_card_busy_ms\":%u,\"max_write_ms\":%u,\"max_sync_ms\":%u,\"max_task_ms\":%u,",segment,sd_stats.segments_opened,sd_stats.segments_closed,sd_stats.catalog_limited,sd_stats.catalog_omitted,sd_stats.dma_errors,sd_stats.dma_timeouts,sd_stats.card_busy_timeouts,sd_stats.max_card_busy_ms,sd_stats.max_write_ms,sd_stats.max_sync_ms,sd_stats.max_task_ms);
 printf("\"ingress_peak\":[%u,%u],\"ingress_drops\":[%u,%u],\"ring_drops\":[%u,%u],\"hardware_lost\":[%u,%u],\"line_errors\":%u,\"crc_errors\":%u,\"sequence_errors\":%u,\"boundary_errors\":%u,\"duplicates\":%u,\"read_errors\":%u,\"truncated_tails\":%u,",cm[0].high_water,cm[1].high_water,cm[0].ingress_drops,cm[1].ingress_drops,cm[0].ring_drops,cm[1].ring_drops,can_hw[0].hardware_lost,can_hw[1].hardware_lost,line_errors,crc_errors,sequence_errors,boundary_errors,duplicates,read_errors,truncated_tails);
 printf("\"busy_target\":\"%s\",",busy_target==OP_WRITE?"write":busy_target==OP_SYNC?"sync":"close");
 printf("\"payload_errors\":%u,\"timestamp_errors\":%u,\"sequence_gaps\":%u,\"premature_error_events\":%u,\"catalog_recount_matches\":%s,",payload_errors,timestamp_errors,sequence_gaps,premature_error,catalog_recount?"true":"false");
 printf("\"redraws\":%u,\"lcd_dma_transfers\":%u,\"lcd_dma_bands\":%u,\"lcd_snapshot_errors\":%u,\"close_result\":%d,\"close_latency_us\":%" PRIu64 ",\"preclose_accepted\":%u,\"fault_events\":%u,\"duplicate_error_same_tick\":%u,\"sd_full_events\":%u,\"max_loop_us\":%" PRIu64 ",\"longest_capture_pump_gap_us\":%" PRIu64,redraws,spi_transfers,display_diagnostics()->dma_bands,spi_snapshot_errors,event_result[EV_SD_CLOSE_DONE],close_latency,preclose_accepted,observed_faults,duplicate_error,event_count[EV_SD_FULL],max_loop_us,longest_pump_gap);
 STRESS_REPORT_EXTRA();printf("}\n");
 for(unsigned ch=0;ch<2;ch++){free(expected_segment[ch]);free(seen[ch]);}return pass?0:1;
}
