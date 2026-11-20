/* SPDX-License-Identifier: MIT
 * Real upstream FreeRTOS POSIX tasks + production storage/diskio/FatFs.
 * The only hardware double is a 16 MiB RAM SD card. No disk device is opened.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx_hal.h"
#include "storage_sd.h"
#include "rtos/storage_owner.h"
#include "services/core/port.h"
#include "services/core/app_evt.h"
#include "services/player/play_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/config/cfg.h"
#include "ff.h"
#include "diskio.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CARD_BLOCKS 32768u
#define REQUIRE(x) do { if(!(x)) {fprintf(stderr,"RTOS storage assertion %d: %s\n",__LINE__,#x);abort();} } while(0)
static uint8_t card[CARD_BLOCKS*512u];
GPIO_TypeDef mock_gpio_c,mock_gpio_d;
static StaticTask_t tcbs[4];
static StackType_t stacks[4][8192];
static TaskHandle_t storage_task_handle;
static player_t players[CH_NUM];
static rec_t recorder_state;
static _Atomic uint32_t event_count[EV_COUNT],phase,accepted,rejected,ui_runs,core_runs;
static _Atomic int32_t event_result[EV_COUNT];
static uint32_t card_busy_until,owning_hal_calls,bad_owner_calls,dma_starts,verified_frames,close_failures;
static bool sd_irq,in_irq,busy_injected,busy_armed=true;
static uint32_t requested_busy_ms=250u;
static storage_sd_stats_t final_stats,fault_stats;
static struct {SD_HandleTypeDef *handle;uint8_t *data;uint32_t lba,count,due;bool pending,writing;} transfer;
static void check_owner(void) {
 if(!revd_storage_is_owner())bad_owner_calls++;
 REQUIRE(revd_storage_is_owner());owning_hal_calls++;
}
uint32_t __get_IPSR(void) {return in_irq?1u:0u;}
uint32_t HAL_GetTick(void) {return (uint32_t)xTaskGetTickCount();}
void HAL_GPIO_Init(GPIO_TypeDef *g,const GPIO_InitTypeDef *p) {(void)g;(void)p;check_owner();}
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *g,uint16_t pin) {(void)g;(void)pin;check_owner();return GPIO_PIN_RESET;}
void HAL_NVIC_SetPriority(int i,uint32_t p,uint32_t s) {(void)i;(void)p;(void)s;check_owner();}
void HAL_NVIC_EnableIRQ(int i) {(void)i;check_owner();sd_irq=true;}
void HAL_NVIC_DisableIRQ(int i) {(void)i;check_owner();sd_irq=false;}
void HAL_NVIC_ClearPendingIRQ(int i) {(void)i;check_owner();}
void test_sd_force_reset(void) {check_owner();transfer.pending=false;}
HAL_StatusTypeDef HAL_SD_Init(SD_HandleTypeDef *h) {(void)h;check_owner();return HAL_OK;}
HAL_StatusTypeDef HAL_SD_DeInit(SD_HandleTypeDef *h) {(void)h;check_owner();return HAL_OK;}
HAL_StatusTypeDef HAL_SD_ConfigWideBusOperation(SD_HandleTypeDef *h,uint32_t w) {(void)h;(void)w;check_owner();return HAL_OK;}
HAL_StatusTypeDef HAL_SD_GetCardInfo(const SD_HandleTypeDef *h,HAL_SD_CardInfoTypeDef *p) {(void)h;check_owner();p->LogBlockSize=512;p->LogBlockNbr=CARD_BLOCKS;return HAL_OK;}
HAL_SD_CardStateTypeDef HAL_SD_GetCardState(SD_HandleTypeDef *h) {(void)h;check_owner();return (int32_t)(HAL_GetTick()-card_busy_until)<0?0u:HAL_SD_CARD_TRANSFER;}
static HAL_StatusTypeDef begin(SD_HandleTypeDef *h,uint8_t *p,uint32_t lba,uint32_t n,bool writing) {
 check_owner();REQUIRE(!transfer.pending&&n&&n<=8u&&lba<CARD_BLOCKS&&n<=CARD_BLOCKS-lba&&((uintptr_t)p&31u)==0);
 transfer.handle=h;transfer.data=p;transfer.lba=lba;transfer.count=n;transfer.writing=writing;transfer.pending=true;transfer.due=HAL_GetTick()+1u;dma_starts++;return HAL_OK;
}
HAL_StatusTypeDef HAL_SD_ReadBlocks_DMA(SD_HandleTypeDef *h,uint8_t *p,uint32_t lba,uint32_t n) {return begin(h,p,lba,n,false);}
HAL_StatusTypeDef HAL_SD_WriteBlocks_DMA(SD_HandleTypeDef *h,const uint8_t *p,uint32_t lba,uint32_t n) {return begin(h,(uint8_t *)(uintptr_t)p,lba,n,true);}
HAL_StatusTypeDef HAL_SD_Abort_IT(SD_HandleTypeDef *h) {(void)h;check_owner();transfer.pending=false;return HAL_OK;}
void HAL_SD_IRQHandler(SD_HandleTypeDef *h) {
 REQUIRE(transfer.pending&&h==transfer.handle&&revd_storage_is_owner());transfer.pending=false;
 uint8_t *sector=card+(size_t)transfer.lba*512u;size_t bytes=(size_t)transfer.count*512u;
 if(transfer.writing) {
  memcpy(sector,transfer.data,bytes);
  if(busy_armed&&!busy_injected&&atomic_load(&accepted)>=100u) {busy_injected=true;card_busy_until=HAL_GetTick()+requested_busy_ms;}
  else card_busy_until=HAL_GetTick()+1u;
  in_irq=true;HAL_SD_TxCpltCallback(h);in_irq=false;
 } else {memcpy(transfer.data,sector,bytes);in_irq=true;HAL_SD_RxCpltCallback(h);in_irq=false;}
}
extern void SDMMC1_IRQHandler(void);
void revd_rtos_notify_storage(void) {if(storage_task_handle)xTaskNotifyGive(storage_task_handle);}
void revd_rtos_notify_storage_from_isr(void) {
 /* Test invokes the HAL IRQ edge on its owning POSIX thread. It does not
  * emulate a Cortex-M interrupt stack; use the real task notification API. */
 if(storage_task_handle)xTaskNotifyGive(storage_task_handle);
}
uint32_t revd_rtos_time_us(void) {
 struct timespec now;REQUIRE(clock_gettime(CLOCK_MONOTONIC,&now)==0);
 return (uint32_t)((uint64_t)now.tv_sec*1000000u+(uint64_t)now.tv_nsec/1000u);
}
void revd_rtos_wait_io(void) {
 check_owner();(void)ulTaskNotifyTake(pdTRUE,1);
 if(transfer.pending&&sd_irq&&(int32_t)(HAL_GetTick()-transfer.due)>=0)SDMMC1_IRQHandler();
}
void revd_io_wait_poll(void) {check_owner();taskYIELD();}
bool evt_post4(ev_type_t e,uint8_t ch,src_t s,int32_t a,int32_t b) {
 (void)ch;(void)s;(void)b;atomic_store(&event_result[e],a);atomic_fetch_add(&event_count[e],1);return true;
}
void evt_msg(msg_t m,uint8_t ch,int32_t b) {(void)m;(void)ch;(void)b;}
const player_t *play_get(int ch) {return &players[ch];}
const rec_t *rec_get(void) {return &recorder_state;}
void revd_storage_file_removed(int index) {(void)index;}
static void await_event(ev_type_t e,uint32_t previous) {
 TickType_t deadline=xTaskGetTickCount()+pdMS_TO_TICKS(10000);
 while(atomic_load(&event_count[e])==previous) {REQUIRE((int32_t)(deadline-xTaskGetTickCount())>0);vTaskDelay(1);}
}
static void await_not_busy(void) {
 TickType_t deadline=xTaskGetTickCount()+pdMS_TO_TICKS(10000);
 while(port_sd_busy()) {REQUIRE((int32_t)(deadline-xTaskGetTickCount())>0);vTaskDelay(1);}
}
static can_frame_t make_frame(uint32_t seq) {
 can_frame_t f={.ts_us=(uint64_t)HAL_GetTick()*1000u+seq,.id=0x100u+(seq&1u),.ch=seq&1u,.len=64,.flags=CANF_FD|CANF_BRS};
 for(unsigned i=0;i<64;i++)f.data[i]=(uint8_t)(seq*13u+i*7u);
 for(unsigned i=0;i<4;i++)f.data[i]=(uint8_t)(seq>>(8u*i));return f;
}
static void storage_main(void *arg) {(void)arg;revd_storage_owner_init();for(;;){revd_storage_owner_step();(void)ulTaskNotifyTake(pdTRUE,1);}}
static void ui_main(void *arg) {
 (void)arg;
 for(;;) {
  storage_sd_stats_t stats;storage_sd_stats(&stats);(void)port_sd_present();(void)port_sd_free_kb();(void)port_sd_total_kb();(void)port_sd_next_session();(void)storage_sd_raw_ready();
  int n=port_sd_file_count();if(n){file_info_t info;REQUIRE(port_sd_file_info(0,&info));}
  atomic_fetch_add(&ui_runs,1);vTaskDelay(1);
 }
}
static void usb_main(void *arg) {
 (void)arg;while(atomic_load(&phase)<1u)vTaskDelay(1);
 REQUIRE(!storage_sd_media_give()); /* Still mounted: never steals a live volume. */
 atomic_store(&phase,2u);while(atomic_load(&phase)<3u)vTaskDelay(1);
 REQUIRE(storage_sd_media_give());REQUIRE(storage_sd_raw_ready());
 uint8_t original[512],pattern[512],readback[512];memset(pattern,0xa7,sizeof pattern);
 uint32_t lba=storage_sd_raw_block_count()-1u;
 REQUIRE(storage_sd_raw_read(lba,original,1));REQUIRE(storage_sd_raw_write(lba,pattern,1));REQUIRE(storage_sd_raw_sync());
 REQUIRE(storage_sd_raw_read(lba,readback,1)&&!memcmp(pattern,readback,512));
 REQUIRE(storage_sd_raw_write(lba,original,1));REQUIRE(storage_sd_raw_sync());
 storage_sd_media_reclaim();REQUIRE(!storage_sd_raw_ready());atomic_store(&phase,4u);
 for(;;)vTaskDelay(100);
}
static void core_main(void *arg) {
 (void)arg;cfg_defaults();revd_storage_publish_context();
 while(!revd_storage_owner_ready())vTaskDelay(1);REQUIRE(port_sd_present());
 /* Direct disk/FatFs access from core is rejected before touching HAL. */
 REQUIRE(!storage_sd_disk_ready()&&disk_status(0)==STA_NOINIT);
 uint8_t denied[512];taskENTER_CRITICAL();in_irq=true;
 REQUIRE(!storage_sd_raw_read(0,denied,1));can_frame_t irq_frame=make_frame(0);REQUIRE(!port_sd_record_frame(&irq_frame));
 in_irq=false;taskEXIT_CRITICAL();
 uint32_t before=atomic_load(&event_count[EV_SD_FORMAT_DONE]);port_sd_format();await_event(EV_SD_FORMAT_DONE,before);REQUIRE(atomic_load(&event_result[EV_SD_FORMAT_DONE])==SD_OK);
 recorder_state.state=REC_PREPARING;revd_storage_publish_context();
 uint32_t unavailable_frames;uint64_t unavailable_duration;TickType_t guard_at=xTaskGetTickCount();
 REQUIRE(!port_sd_replay_info(0,0,&unavailable_frames,&unavailable_duration));REQUIRE((TickType_t)(xTaskGetTickCount()-guard_at)<=1u);
 before=atomic_load(&event_count[EV_SD_PREALLOC_DONE]);port_sd_prealloc(1,1,64,500000,2000000,true);await_event(EV_SD_PREALLOC_DONE,before);REQUIRE(atomic_load(&event_result[EV_SD_PREALLOC_DONE])==SD_OK);
 unsigned seg=1;
 for(uint32_t seq=0;seq<1200u;seq++) {
  if(seq&&seq%300u==0)port_sd_next_segment(++seg);
  can_frame_t frame=make_frame(seq);
  if(port_sd_record_frame(&frame))atomic_fetch_add(&accepted,1);else atomic_fetch_add(&rejected,1);
  atomic_fetch_add(&core_runs,1);
  if(seq&1u)vTaskDelay(1);
 }
 before=atomic_load(&event_count[EV_SD_CLOSE_DONE]);port_sd_close();can_frame_t late=make_frame(1200u);REQUIRE(!port_sd_record_frame(&late));await_event(EV_SD_CLOSE_DONE,before);
 recorder_state.state=REC_IDLE;revd_storage_publish_context();
 REQUIRE(atomic_load(&event_result[EV_SD_CLOSE_DONE])==SD_OK);REQUIRE(accepted==1200u&&!rejected&&port_sd_file_count()==4);
 REQUIRE(busy_injected&&ui_runs>100u&&core_runs==1200u);
 for(int file=0;file<4;file++)for(uint8_t ch=0;ch<2;ch++) {
  players[ch].state=PL_PLAYING;players[ch].file=file;revd_storage_publish_context();
  uint32_t count;uint64_t duration;REQUIRE(port_sd_replay_info(file,ch,&count,&duration)&&count==150u);
  for(uint32_t ordinal=0;ordinal<count;ordinal++) {
   can_frame_t actual;uint32_t seq=(uint32_t)file*300u+ordinal*2u+ch;
   can_frame_t expected=make_frame(seq);REQUIRE(port_sd_read_frame(file,ch,ordinal,&actual));
   REQUIRE(actual.ch==ch&&actual.id==expected.id&&actual.len==64&&actual.flags==expected.flags&&!memcmp(actual.data,expected.data,64));verified_frames++;
  }
  players[ch].state=PL_IDLE;revd_storage_publish_context();
 }
 atomic_store(&phase,1u);while(atomic_load(&phase)<2u)vTaskDelay(1);
 port_sd_unmount();await_not_busy();atomic_store(&phase,3u);while(atomic_load(&phase)<4u)vTaskDelay(1);
 before=atomic_load(&event_count[EV_SD_MOUNT_DONE]);port_sd_mount();await_event(EV_SD_MOUNT_DONE,before);REQUIRE(atomic_load(&event_result[EV_SD_MOUNT_DONE])==SD_OK&&port_sd_file_count()==4);
 before=atomic_load(&event_count[EV_SD_PREALLOC_DONE]);port_sd_prealloc(2,1,256,500000,2000000,true);await_event(EV_SD_PREALLOC_DONE,before);REQUIRE(atomic_load(&event_result[EV_SD_PREALLOC_DONE])==SD_OK);
 uint32_t rejects=0;
 for(uint32_t seq=0;seq<1029u;seq++) {can_frame_t frame=make_frame(seq);if(!port_sd_record_frame(&frame))rejects++;}
 REQUIRE(rejects==5u);before=atomic_load(&event_count[EV_SD_CLOSE_DONE]);port_sd_close();await_event(EV_SD_CLOSE_DONE,before);if(atomic_load(&event_result[EV_SD_CLOSE_DONE])!=SD_OK)close_failures++;
 storage_sd_stats(&final_stats);REQUIRE(final_stats.queue_overflows==5u&&final_stats.enqueued_frames==2224u&&final_stats.encoded_frames==2224u&&!final_stats.queued_frames&&!final_stats.discarded_frames&&!final_stats.dma_errors);
 REQUIRE(!bad_owner_calls&&atomic_load(&event_count[EV_SD_WRITE_ERR])==1u);
 REQUIRE(port_sd_file_count()==5);
 for(uint8_t ch=0;ch<2;ch++) {
  players[ch].state=PL_PLAYING;players[ch].file=4;revd_storage_publish_context();
  uint32_t count;uint64_t duration;REQUIRE(port_sd_replay_info(4,ch,&count,&duration)&&count==512u);
  for(uint32_t ordinal=0;ordinal<count;ordinal++) {
   can_frame_t actual,expected=make_frame(ordinal*2u+ch);
   REQUIRE(port_sd_read_frame(4,ch,ordinal,&actual));
   REQUIRE(actual.ch==ch&&actual.len==64&&!memcmp(actual.data,expected.data,64));verified_frames++;
  }
  players[ch].state=PL_IDLE;revd_storage_publish_context();
 }
 REQUIRE(verified_frames==2224u);
 /* A second, explicitly destructive-to-RAM-log fault must be reported. It is
  * never counted as zero loss: the 2.5 s programming tail exceeds 2 s limit. */
 busy_armed=false;
 before=atomic_load(&event_count[EV_SD_PREALLOC_DONE]);port_sd_prealloc(3,1,64,500000,2000000,true);await_event(EV_SD_PREALLOC_DONE,before);
 REQUIRE(atomic_load(&event_result[EV_SD_PREALLOC_DONE])==SD_OK);
 requested_busy_ms=2500u;busy_injected=false;busy_armed=true;
 for(uint32_t seq=0;seq<100u;seq++){can_frame_t frame=make_frame(seq);REQUIRE(port_sd_record_frame(&frame));}
 before=atomic_load(&event_count[EV_SD_CLOSE_DONE]);port_sd_close();await_event(EV_SD_CLOSE_DONE,before);
 REQUIRE(atomic_load(&event_result[EV_SD_CLOSE_DONE])!=SD_OK);close_failures++;
 storage_sd_stats(&fault_stats);REQUIRE(fault_stats.card_busy_timeouts==1u&&fault_stats.dma_errors==1u&&fault_stats.discarded_frames>0u);
 vTaskEndScheduler();abort();
}
int main(void) {
 revd_storage_owner_bootstrap();REQUIRE(!revd_storage_owner_ready());
 can_frame_t early={0};REQUIRE(!port_sd_record_frame(&early));
 storage_task_handle=xTaskCreateStatic(storage_main,"storage",8192,NULL,2,stacks[0],&tcbs[0]);
 REQUIRE(storage_task_handle);
 REQUIRE(xTaskCreateStatic(core_main,"core",8192,NULL,4,stacks[1],&tcbs[1]));
 REQUIRE(xTaskCreateStatic(ui_main,"ui",8192,NULL,1,stacks[2],&tcbs[2]));
 REQUIRE(xTaskCreateStatic(usb_main,"usb",8192,NULL,3,stacks[3],&tcbs[3]));
 vTaskStartScheduler();
 printf("{\"kernel\":\"FreeRTOS V11.3.2\",\"port\":\"official GCC/POSIX\",\"physical_hardware_tested\":false,\"production_sd_fatfs\":true,\"injected_long_busy_ms\":250,\"recorded_and_verified\":%u,\"accepted\":%u,\"overflow_rejections\":%u,\"queue_peak\":%u,\"ui_iterations\":%u,\"core_iterations\":%u,\"hal_owner_calls\":%u,\"hal_wrong_owner\":%u,\"dma_transfers\":%u,\"detected_close_failures\":%u,\"timeout_fault_injected_busy_ms\":2500,\"timeout_fault_card_busy_timeouts\":%u,\"timeout_fault_dma_errors\":%u,\"timeout_fault_discarded_frames\":%u,\"msc_stack_buffer_roundtrip\":true,\"pass\":true}\n",verified_frames,final_stats.enqueued_frames,final_stats.queue_overflows,final_stats.queue_high_water,(unsigned)ui_runs,(unsigned)core_runs,owning_hal_calls,bad_owner_calls,dma_starts,close_failures,fault_stats.card_busy_timeouts,fault_stats.dma_errors,fault_stats.discarded_frames);
 return 0;
}
