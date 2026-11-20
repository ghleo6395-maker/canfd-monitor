/* SPDX-License-Identifier: MIT
 * Rev D SDMMC1/FatFs storage. ST HAL and FatFs provide the card/filesystem;
 * candump framing reuses services/logfmt's can-utils-derived BSD codec.
 * Main-loop context only. SDMMC internal DMA, cooperative FatFs waits and a
 * bounded recorder queue keep card latency out of per-frame ingestion.
 */
#include "storage_sd.h"
#include "stm32h7xx_hal.h"
#include "ff.h"
#include "services/core/port.h"
#include "services/core/app_evt.h"
#include "services/logfmt/candump.h"
#include "services/player/play_sm.h"
#include "services/config/cfg.h"
#include "services/device/file_port.h"
#include "send_plan_json.h"

#ifdef REVD_FREERTOS
#define REVD_STORAGE_OWNER_IMPL
#include "rtos/storage_owner_driver.h"
#include "rtos/storage_owner.h"
#include "rtos/runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#define CAPTURE_LOCK() taskENTER_CRITICAL()
#define CAPTURE_UNLOCK() taskEXIT_CRITICAL()
#else
#define CAPTURE_LOCK() ((void)0)
#define CAPTURE_UNLOCK() ((void)0)
#endif
#include <stdatomic.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define SD_TIMEOUT_MS 2000u
#define SD_DEBOUNCE_MS 30u
#define SD_SYNC_MS 1000u
#define SD_ERROR_REPEAT_MS 200u
#define RECORD_BUFFER_SIZE 4096u
#define RECORD_QUEUE_FRAMES REVD_SD_RECORD_QUEUE_FRAMES
#define RECORD_TASK_FRAMES 64u
#define SD_COMMANDS 16u
#define SD_FILE_MAX 128u
#define SD_PATH_SIZE 96u
#define SD_DIR "0:/CANLOG"

typedef struct {
    file_info_t info;
    char path[SD_PATH_SIZE];
    uint32_t count[CH_NUM];
    uint64_t base_us, last_us[CH_NUM];
    bool scanned, valid;
    uint32_t token, session, file_id;
} sd_file_t;

typedef struct {
    FIL file;
    uint8_t buffer[512];
    UINT pos, used;
    bool open;
} text_reader_t;

typedef struct {
    text_reader_t text;
    int index;
    uint32_t next, cached_ordinal;
    can_frame_t cached;
    bool cached_valid;
} replay_reader_t;

static SD_HandleTypeDef sd;
static FATFS fs;
static bool initialized, mounted, msc_owner;
static _Atomic bool io_busy;
static bool detect_raw, detect_stable;
static uint32_t detect_since;
static uint32_t block_count, free_kb, total_kb;
static uint32_t session_max;
static sd_file_t files[SD_FILE_MAX];
static unsigned nfiles;
static uint32_t file_serial;
static char record_prefix[FILE_PORT_REC_PREFIX_MAX_BYTES+1u];
static char recording_directory[SD_PATH_SIZE];
static uint32_t recording_token,media_generation,recording_media_generation;
static bool plans_refresh_pending;
/* A full catalog reserves one unpinned cache slot for the current session.
 * Eviction changes metadata only, never files on media; stable tokens reject
 * stale UI requests. Further segments reuse that session's representative. */
static sd_file_t unlisted_recording;
static replay_reader_t replay[CH_NUM];

static FIL recording;
static _Atomic bool recording_open;
static bool recording_fault, full_reported, error_reported;
static int recording_index = -1;
static uint32_t recording_session, recording_segment_kb;
static uint32_t recording_nbr, recording_dbr;
static bool recording_fd;
static uint32_t sync_at, error_at;
static uint8_t record_buffer[RECORD_BUFFER_SIZE] __attribute__((aligned(32)));
static UINT record_used;
static uint64_t recorded_bytes;
/* Always stage DMA through AXI SRAM: FatFs windows, stack buffers and USB
 * buffers may otherwise reside in DTCM, which SDMMC IDMA cannot access.
 * The board linker places ordinary .bss in AXI SRAM; D-cache remains off. */
static uint8_t dma_buffer[4096] __attribute__((aligned(32)));
static volatile uint8_t dma_state;
enum { DMA_IDLE, DMA_ACTIVE, DMA_DONE, DMA_ERROR };
static storage_sd_stats_t stats;
/* CPU encoding reads this queue; SDMMC IDMA only sees dma_buffer in AXI. */
#ifdef CANFD_REVD_TARGET
static can_frame_t frame_queue[RECORD_QUEUE_FRAMES]
    __attribute__((section(".revd_dtcm_bss"), aligned(32)));
#else
static can_frame_t frame_queue[RECORD_QUEUE_FRAMES];
#endif
static _Atomic uint32_t frame_in, frame_out;
static _Atomic bool accepting_frames, close_pending, api_error;
static _Atomic bool task_active;
static uint32_t close_fence;
static unsigned close_command_fence;
static _Atomic bool overflow_pending, deferred_write_error;
static bool overflow_reported;
typedef enum { CMD_NOP, CMD_MOUNT, CMD_UNMOUNT, CMD_PREALLOC, CMD_ROTATE,
               CMD_REINIT, CMD_DELETE, CMD_PROTECT, CMD_FORMAT } command_type_t;
typedef struct {
    command_type_t type;
    uint32_t fence, a, b, c, d, e;
    bool flag;
} sd_command_t;
static sd_command_t commands[SD_COMMANDS];
static _Atomic unsigned command_in, command_out;
static void post(ev_type_t type,int32_t result);
static void post_message(msg_t message,int32_t detail);
static void files_on_mount(void);
static void files_on_unmount(void);
static void files_task(void);
static int prefix_now(const char *name);
static int rename_now(uint32_t token,const char *name);
static bool session_number(const char *name,uint32_t *number);
static uint32_t session_identity(const char *directory,uint32_t session);
static FRESULT ensure_session(const char *directory,uint32_t session);
static void bind_legacy_session(sd_file_t *entry);

/* Completion delivery is back-pressured, never dropped when the application
 * queue is full. Repeated identical errors are one outstanding notification;
 * accepted storage commands wait behind that notification before more I/O. */
#define SD_PENDING_EVENTS 64u
typedef struct {ev_type_t type;int32_t result,detail;} pending_event_t;
static pending_event_t pending_events[SD_PENDING_EVENTS];
static unsigned pending_head,pending_count;
static bool pending_fallback[EV_COUNT];
static int32_t fallback_result[EV_COUNT],fallback_detail[EV_COUNT];
static bool flush_events(void)
{
    while(pending_count){
        pending_event_t event=pending_events[pending_head];
        if(!evt_post4(event.type,0,SRC_SYS,event.result,event.detail))return false;
        pending_head=(pending_head+1u)%SD_PENDING_EVENTS;pending_count--;
    }
    for(unsigned i=0;i<EV_COUNT;i++)if(pending_fallback[i]){
        if(!evt_post4((ev_type_t)i,0,SRC_SYS,fallback_result[i],fallback_detail[i]))return false;
        pending_fallback[i]=false;
    }
    return true;
}

/* Provided by the board integration. It must service CAN ingress and DMA/USB
 * completion and enqueue capture frames, but never dispatch app events, reenter FatFs or this task. */
extern void revd_io_wait_poll(void);
#ifndef REVD_FREERTOS
static void cooperate(void)
{
    stats.cooperative_polls++;
    revd_io_wait_poll();
}
#endif
static bool producer_context(void)
{
#ifdef REVD_FREERTOS
    return __get_IPSR() == 0u && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        revd_storage_owner_ready();
#else
#ifdef CANFD_REVD_TARGET
    return __get_IPSR()==0u;
#else
    return true;
#endif
#endif
}
static bool command_push(sd_command_t command)
{
    if (!producer_context()) return false;
    CAPTURE_LOCK();
    unsigned in = atomic_load_explicit(&command_in, memory_order_relaxed);
    unsigned out = atomic_load_explicit(&command_out, memory_order_acquire);
    if (in - out == SD_COMMANDS) {
        stats.command_overflows++;
        CAPTURE_UNLOCK();
        return false;
    }
    command.fence = atomic_load_explicit(&frame_in, memory_order_acquire);
    commands[in % SD_COMMANDS] = command;
    atomic_store_explicit(&command_in, in + 1u, memory_order_release);
    uint32_t queued = in + 1u - out;
    if (queued > stats.command_high_water) stats.command_high_water = queued;
    CAPTURE_UNLOCK();
#ifdef REVD_FREERTOS
    revd_rtos_notify_storage();
#endif
    return true;
}

static void command_failed(ev_type_t event)
{
    api_error = true;
    post(event, SD_ERR_IO);
}

void storage_sd_stats(storage_sd_stats_t *out)
{
    if (!out) return;
    CAPTURE_LOCK();
    *out = stats;
    out->queued_frames = atomic_load_explicit(&frame_in, memory_order_acquire) -
                         atomic_load_explicit(&frame_out, memory_order_acquire);
    CAPTURE_UNLOCK();
}

static void post_fields(ev_type_t type,int32_t result,int32_t detail)
{
#ifdef REVD_FREERTOS
    revd_storage_owner_publish_snapshot();
#endif
    if(!pending_count&&evt_post4(type,0,SRC_SYS,result,detail))return;
    /* Error/recovery edges delimit a retry streak. Coalescing an error across
     * an intervening recovery would publish a healthy state after a new fault. */
    unsigned first=(type==EV_SD_WRITE_ERR||type==EV_SD_WRITE_RECOVERED)?
        (pending_count?pending_count-1u:0u):0u;
    for(unsigned i=first;i<pending_count;i++){
        const pending_event_t *event=&pending_events[(pending_head+i)%SD_PENDING_EVENTS];
        if(event->type==type&&event->result==result&&event->detail==detail){stats.event_coalesced++;return;}
    }
    stats.event_deferred++;
    if(pending_count<SD_PENDING_EVENTS){
        pending_events[(pending_head+pending_count)%SD_PENDING_EVENTS]=(pending_event_t){type,result,detail};
        pending_count++;
        if(pending_count>stats.event_high_water)stats.event_high_water=pending_count;
    }else{pending_fallback[type]=true;fallback_result[type]=result;fallback_detail[type]=detail;stats.event_fallback++;}
}
static void post(ev_type_t type,int32_t result){post_fields(type,result,0);}
static void post_message(msg_t message,int32_t detail){post_fields(EV_NTF_MSG,(int32_t)message,detail);}

static sd_result_t result_code(FRESULT result)
{
    switch (result) {
    case FR_OK: return SD_OK;
    case FR_WRITE_PROTECTED: return SD_ERR_WP;
    case FR_NO_FILESYSTEM: case FR_INVALID_NAME: case FR_INVALID_PARAMETER:
        return SD_ERR_UNSUPPORTED;
    case FR_DENIED: return SD_ERR_FULL;
    default: return SD_ERR_IO;
    }
}

bool port_sd_present(void)
{
    return HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_4) == GPIO_PIN_RESET;
}

#ifdef CANFD_REVE_TARGET
static void sd_rail_off(void)
{
    GPIO_InitTypeDef g = {.Mode = GPIO_MODE_INPUT, .Pull = GPIO_NOPULL};
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2; HAL_GPIO_Init(GPIOD, &g);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_9, GPIO_PIN_RESET);
}

static void sd_rail_on(void)
{
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_9, GPIO_PIN_SET);
    HAL_Delay(5); /* > TPS22919 rise time, before any card signal is driven. */
    GPIO_InitTypeDef g = {.Mode = GPIO_MODE_AF_PP, .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_VERY_HIGH, .Alternate = GPIO_AF12_SDMMC1};
    /* External CMD/DAT pullups are on +3V3_SD, not the unswitched rail. */
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &g);
    g.Pin = GPIO_PIN_2; HAL_GPIO_Init(GPIOD, &g);
}
#endif

void storage_sd_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_SDMMC1_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
#ifndef CANFD_REVE_TARGET
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDMMC1;
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11;
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = GPIO_PIN_12;
    gpio.Pull = GPIO_NOPULL; /* SD clock must not be pulled high in reset. */
    HAL_GPIO_Init(GPIOC, &gpio);
    gpio.Pin = GPIO_PIN_2;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOD, &gpio);
#else
    sd_rail_off();
#endif
    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOD, &gpio);
    detect_raw = detect_stable = port_sd_present();
    detect_since = HAL_GetTick();
    HAL_NVIC_SetPriority(SDMMC1_IRQn, 5, 0);
    HAL_NVIC_DisableIRQ(SDMMC1_IRQn);
    HAL_NVIC_ClearPendingIRQ(SDMMC1_IRQn);
    for (int ch = 0; ch < CH_NUM; ch++) replay[ch].index = -1;
}

static void maximum(uint32_t *value, uint32_t candidate)
{ if (candidate > *value) *value = candidate; }

static bool wait_transfer(void)
{
    uint32_t start = HAL_GetTick();
#ifdef REVD_FREERTOS
    uint32_t busy_started_us = revd_rtos_time_us();
#endif
    for (;;) {
#ifndef REVD_FREERTOS
        cooperate();
#endif
        uint32_t elapsed = (uint32_t)(HAL_GetTick() - start);
        maximum(&stats.max_card_busy_ms, elapsed);
        if (!port_sd_present()) return false;
        HAL_SD_CardStateTypeDef state = HAL_SD_GetCardState(&sd);
        if (state == HAL_SD_CARD_TRANSFER) return true;
        if (state == HAL_SD_CARD_ERROR || state == HAL_SD_CARD_DISCONNECTED)
            return false;
        if (elapsed >= SD_TIMEOUT_MS) {
            stats.card_busy_timeouts++;
            return false;
        }
#ifdef REVD_FREERTOS
        /* Normal programming tails must not add a mandatory 1 ms sleep before
         * every ready check. Spend ONE bounded 200 us poll budget, then block
         * only this owner while core/USB/UI continue independently. */
        stats.cooperative_polls++;
        if ((uint32_t)(revd_rtos_time_us() - busy_started_us) < 200u) {
            taskYIELD();
        } else {
            vTaskDelay(1u);
        }
#endif
    }
}

/* A timeout can leave a late DATAEND in the peripheral/NVIC even after
 * Abort_IT. Quiesce both before another transaction may use this handle/buffer.
 * Completion callbacks do not carry a transaction generation, so merely clearing
 * the software done flag is insufficient. Recovery requires card_init(). */
static void card_quiesce(void)
{
    HAL_NVIC_DisableIRQ(SDMMC1_IRQn);
    dma_state = DMA_IDLE;
    if (sd.Instance == SDMMC1) {
        (void)HAL_SD_Abort_IT(&sd);
        (void)HAL_SD_DeInit(&sd);
    }
    __HAL_RCC_SDMMC1_FORCE_RESET();
    __DMB();
    __HAL_RCC_SDMMC1_RELEASE_RESET();
    HAL_NVIC_ClearPendingIRQ(SDMMC1_IRQn);
#ifdef CANFD_REVE_TARGET
    sd_rail_off();
#endif
    initialized = false;
    block_count = 0;
    CAPTURE_LOCK();
    accepting_frames = false;
    CAPTURE_UNLOCK();
}

static bool player_any_active(void);
static bool card_init(void)
{
    if (!port_sd_present() || io_busy) return false;
    if (initialized) return true;
#ifdef CANFD_REVE_TARGET
    /* A fresh rail cycle cannot preserve old FAT/MSC handles. A failed active
     * session must close/invalidate first; never hide loss by remounting it. */
    if (recording_open || msc_owner || player_any_active()) return false;
    sd_rail_off();
    HAL_Delay(10); /* QOD discharges the local 10uF rail before reset. */
    sd_rail_on();
#endif
    HAL_NVIC_DisableIRQ(SDMMC1_IRQn);
    HAL_NVIC_ClearPendingIRQ(SDMMC1_IRQn);
    memset(&sd, 0, sizeof sd);
    sd.Instance = SDMMC1;
    sd.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    sd.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    sd.Init.BusWide = SDMMC_BUS_WIDE_1B;
    sd.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    /* Kernel=100 MHz; 100/(2*4)=12.5 MHz, conservative normal-speed SD.
     * HAL performs identification at <=400 kHz before applying this divider. */
    sd.Init.ClockDiv = 4;
    io_busy = true;
    HAL_StatusTypeDef status = HAL_SD_Init(&sd);
    if (status == HAL_OK)
        status = HAL_SD_ConfigWideBusOperation(&sd, SDMMC_BUS_WIDE_4B);
    HAL_SD_CardInfoTypeDef card = {0};
    if (status == HAL_OK) status = HAL_SD_GetCardInfo(&sd, &card);
    initialized = status == HAL_OK && card.LogBlockSize == 512u && card.LogBlockNbr;
    block_count = initialized ? card.LogBlockNbr : 0;
    if (initialized) {
        dma_state = DMA_IDLE;
        HAL_NVIC_ClearPendingIRQ(SDMMC1_IRQn);
        HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
    }
    io_busy = false;
    return initialized;
}

void SDMMC1_IRQHandler(void) { HAL_SD_IRQHandler(&sd); }
void HAL_SD_TxCpltCallback(SD_HandleTypeDef *handle)
{ if (handle == &sd && dma_state == DMA_ACTIVE) {
    dma_state = DMA_DONE;
#ifdef REVD_FREERTOS
    revd_rtos_notify_storage_from_isr();
#endif
} }
void HAL_SD_RxCpltCallback(SD_HandleTypeDef *handle)
{ if (handle == &sd && dma_state == DMA_ACTIVE) {
    dma_state = DMA_DONE;
#ifdef REVD_FREERTOS
    revd_rtos_notify_storage_from_isr();
#endif
} }
void HAL_SD_ErrorCallback(SD_HandleTypeDef *handle)
{ if (handle == &sd && dma_state != DMA_IDLE) {
    dma_state = DMA_ERROR;
#ifdef REVD_FREERTOS
    revd_rtos_notify_storage_from_isr();
#endif
} }

static bool dma_wait(void)
{
    uint32_t start = HAL_GetTick();
    while (dma_state == DMA_ACTIVE && port_sd_present() &&
           (uint32_t)(HAL_GetTick() - start) < SD_TIMEOUT_MS) {
#ifdef REVD_FREERTOS
        stats.cooperative_polls++;
        revd_rtos_wait_io(); /* DATAEND notification can wake before one tick. */
#else
        cooperate();
#endif
    }
    maximum(&stats.max_dma_wait_ms, (uint32_t)(HAL_GetTick() - start));
    bool done = dma_state == DMA_DONE;
    if (!done) {
        if (dma_state == DMA_ACTIVE) stats.dma_timeouts++;
        stats.dma_errors++;
        card_quiesce();
    }
    dma_state = DMA_IDLE;
    /* DATAEND is not the end of the card's internal programming. */
    if (done && !wait_transfer()) {
        stats.dma_errors++;
        card_quiesce();
        return false;
    }
    return done;
}

static bool transfer(uint32_t lba, uint8_t *read_data,
                     const uint8_t *write_data, uint32_t count)
{
    if (!initialized || !port_sd_present() || io_busy || !count ||
        lba >= block_count || count > block_count - lba ||
        (!read_data && !write_data)) return false;
    io_busy = true;
    bool ok = wait_transfer();
    if (!ok) { stats.dma_errors++; card_quiesce(); }
    uint32_t offset = 0;
    while (ok && offset < count) {
        uint32_t chunk = count - offset;
        if (chunk > sizeof dma_buffer / 512u) chunk = sizeof dma_buffer / 512u;
        const uint8_t *source = write_data ? write_data + offset * 512u : NULL;
        uint8_t *target = read_data ? read_data + offset * 512u : NULL;
        if (source) memcpy(dma_buffer, source, chunk * 512u);
        __DMB();
        dma_state = DMA_ACTIVE;
        HAL_StatusTypeDef status = source
            ? HAL_SD_WriteBlocks_DMA(&sd, dma_buffer, lba + offset, chunk)
            : HAL_SD_ReadBlocks_DMA(&sd, dma_buffer, lba + offset, chunk);
        stats.dma_transfers++;
        if (status != HAL_OK) {
            dma_state = DMA_IDLE;
            stats.dma_errors++;
            card_quiesce();
            ok = false;
        } else ok = dma_wait();
        __DMB();
        if (ok && target) memcpy(target, dma_buffer, chunk * 512u);
        offset += chunk;
    }
    io_busy = false;
    return ok;
}

static bool disk_owner(void)
{
#ifdef REVD_FREERTOS
    return revd_storage_is_owner();
#else
    return true;
#endif
}
bool storage_sd_disk_init(void) { return disk_owner() && !msc_owner && card_init(); }
bool storage_sd_disk_ready(void) { return disk_owner() && !msc_owner && initialized && port_sd_present(); }
uint32_t storage_sd_disk_block_count(void) { return storage_sd_disk_ready() ? block_count : 0; }
bool storage_sd_disk_read(uint32_t lba, uint8_t *data, uint32_t count)
{ return disk_owner() && !msc_owner && transfer(lba, data, NULL, count); }
bool storage_sd_disk_write(uint32_t lba, const uint8_t *data, uint32_t count)
{ return disk_owner() && !msc_owner && transfer(lba, NULL, data, count); }
bool storage_sd_disk_sync(void)
{ return storage_sd_disk_ready() && !io_busy && wait_transfer(); }
bool storage_sd_raw_ready(void) { return msc_owner && initialized && port_sd_present(); }
bool storage_sd_raw_sync(void)
{ return storage_sd_raw_ready() && !io_busy && wait_transfer(); }
uint32_t storage_sd_raw_block_count(void) { return storage_sd_raw_ready() ? block_count : 0; }
bool storage_sd_raw_read(uint32_t lba, uint8_t *data, uint32_t count)
{ return msc_owner && transfer(lba, data, NULL, count); }
bool storage_sd_raw_write(uint32_t lba, const uint8_t *data, uint32_t count)
{ return msc_owner && transfer(lba, NULL, data, count); }

static bool player_any_active(void)
{
#ifdef REVD_FREERTOS
    return revd_storage_play_any_active();
#else
    return play_any_active();
#endif
}
static bool player_file_in_use(int index)
{
#ifdef REVD_FREERTOS
    return revd_storage_play_file_in_use(index);
#else
    return play_file_in_use(index);
#endif
}
static bool player_is_idle(unsigned ch)
{
#ifdef REVD_FREERTOS
    return revd_storage_player_idle(ch);
#else
    return play_get((int)ch)->state == PL_IDLE;
#endif
}
static void reader_close(text_reader_t *reader)
{
    if (reader->open) (void)f_close(&reader->file);
    reader->open = false;
    reader->pos = reader->used = 0;
}

static void replay_close_all(void)
{
    for (int ch = 0; ch < CH_NUM; ch++) {
        reader_close(&replay[ch].text);
        replay[ch].index = -1;
        replay[ch].cached_valid = false;
    }
}

static FRESULT reader_open(text_reader_t *reader, const char *path)
{
    reader_close(reader);
    FRESULT result = f_open(&reader->file, path, FA_READ);
    reader->open = result == FR_OK;
    return result;
}

/* Returns 1=line, 0=EOF, -1=I/O error, -2=overlong line. An incomplete final
 * line is parsed normally; invalid/torn bytes are never silently played. */
static int reader_line(text_reader_t *reader, char *line, size_t *length)
{
    size_t used = 0;
    if (!reader->open) return -1;
    for (;;) {
        if (reader->pos == reader->used) {
            FRESULT result = f_read(&reader->file, reader->buffer,
                                   sizeof reader->buffer, &reader->used);
            reader->pos = 0;
            if (result != FR_OK) return -1;
            if (!reader->used) {
                *length = used;
                line[used] = 0;
                return used ? 1 : 0;
            }
        }
        char c = (char)reader->buffer[reader->pos++];
        if (used == LOGFMT_MAX_LINE) return -2;
        line[used++] = c;
        if (c == '\n') {
            line[used] = 0;
            *length = used;
            return 1;
        }
    }
}

static void update_space(void)
{
    DWORD clusters;
    FATFS *volume;
    if (!mounted || msc_owner || f_getfree("0:", &clusters, &volume) != FR_OK) {
        free_kb = 0;
        return;
    }
    uint64_t free_sectors = (uint64_t)clusters * volume->csize;
    uint64_t total_sectors = (uint64_t)(volume->n_fatent - 2u) * volume->csize;
    free_kb = (uint32_t)(free_sectors / 2u);
    total_kb = (uint32_t)(total_sectors / 2u);
}

static bool suffix_is(const char *name, const char *extension)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    while (*dot && *extension)
        if (toupper((unsigned char)*dot++) != toupper((unsigned char)*extension++))
            return false;
    return !*dot && !*extension;
}

static bool join_path(char *out, size_t size, const char *directory, const char *name)
{
    int length = snprintf(out, size, "%s/%s", directory, name);
    return length > 0 && (size_t)length < size;
}

static void describe_file(sd_file_t *entry, const char *path, const FILINFO *fatinfo)
{
    memset(entry, 0, sizeof *entry);
    snprintf(entry->path, sizeof entry->path, "%s", path);
    entry->token=++file_serial;if(!entry->token)entry->token=++file_serial;
    /* Session tokens can be shared by several segments; retain the original
     * per-file serial for queued operations on one exact catalog entry. */
    entry->file_id=entry->token;
    const char *display=path+3;
    if(!strncmp(path,SD_DIR "/",sizeof(SD_DIR))){
        const char *directory=path+sizeof(SD_DIR);const char *slash=strchr(directory,'/');
        if(slash){char component[64];size_t bytes=(size_t)(slash-directory);
            if(bytes<sizeof component){memcpy(component,directory,bytes);component[bytes]=0;uint32_t session;
                if(session_number(component,&session)){
                    char full[SD_PATH_SIZE];size_t size=(size_t)(slash-path);memcpy(full,path,size);full[size]=0;
                    uint32_t token=session_identity(full,session);
                    if(token){entry->session=session;entry->token=token;entry->info.is_session=true;
                        if(component[8]=='_')display=directory+9;
                    }
                }
            }
        }
    }
    /* A named session is represented by its complete label; segment files
     * remain separately indexed in the existing replay catalog. */
    size_t length=strlen(display);const char *slash=strchr(display,'/');
    if(entry->session&&display!=path+3&&slash)length=(size_t)(slash-display);
    if(length>47){length=46;while(length&&((unsigned char)display[length]&0xC0u)==0x80u)length--;}
    memcpy(entry->info.name,display,length);entry->info.name[length]=0;
    entry->info.size_kb = (uint32_t)(((uint64_t)fatinfo->fsize + 1023u) / 1024u);
    entry->info.protected_ = (fatinfo->fattrib & AM_RDO) != 0;
    entry->info.kind = FK_CANDUMP;
    entry->info.imported = true;
}

static bool add_file(const char *path, const FILINFO *fatinfo)
{
    if (nfiles == SD_FILE_MAX) return false;
    describe_file(&files[nfiles++], path, fatinfo);
    return true;
}

static sd_file_t *current_recording(void)
{
    return recording_index >= 0 ? &files[recording_index] : &unlisted_recording;
}

static bool session_number(const char *name, uint32_t *number)
{
    if (strlen(name) < 8 || (name[8]&&name[8]!='_') || toupper((unsigned char)name[0]) != 'S') return false;
    uint32_t value = 0;
    for (int i = 1; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
        value = value * 10u + (uint32_t)(name[i] - '0');
    }
    *number = value;
    return true;
}

static FRESULT enumerate(const char *directory, bool sessions)
{
    DIR dir;
    FILINFO info;
    FRESULT result = f_opendir(&dir, directory);
    if (result != FR_OK) return result;
    for (;;) {
        result = f_readdir(&dir, &info);
        if (result != FR_OK || !info.fname[0]) break;
        if (info.fattrib & (AM_HID | AM_SYS)) continue;
        char path[SD_PATH_SIZE];
        if (!join_path(path, sizeof path, directory, info.fname)) continue;
        if (info.fattrib & AM_DIR) {
            uint32_t session;
            if (sessions && session_number(info.fname, &session)) {
                if (session > session_max) session_max = session;
                result = enumerate(path, false);
                if (result != FR_OK) break;
            }
        } else if (suffix_is(info.fname, ".LOG") || suffix_is(info.fname, ".TXT")) {
            if (!add_file(path, &info)) {
                /* Listing is bounded; signal the limit, never overwrite entries. */
                if (!stats.catalog_limited) evt_msg(MSG_SD_ERROR, 0, (int32_t)SD_FILE_MAX);
                stats.catalog_limited = 1;
                stats.catalog_omitted++;
                /* Continue bounded directory enumeration so omitted counts and
                 * the largest session number remain accurate on remount. */
            }
        }
    }
    FRESULT close_result = f_closedir(&dir);
    return result == FR_OK ? close_result : result;
}

static bool scan_file(unsigned index)
{
    sd_file_t *entry = &files[index];
    if (entry->scanned) return entry->valid;
    entry->scanned = true;
    entry->valid = false;
    /* A failed close leaves capture-time counts for bytes that may never have
     * reached media. Rebuild every payload-derived field from the file. */
    memset(entry->count, 0, sizeof entry->count);
    memset(entry->last_us, 0, sizeof entry->last_us);
    entry->base_us = 0;
    entry->info.frames = entry->info.sample_frames = entry->info.duration_ms = 0;
    entry->info.channels = 0;
    entry->info.nbr = entry->info.dbr = 0;
    entry->info.fd = entry->info.sample_available = false;
    entry->info.imported = true;
    entry->info.kind = FK_UNKNOWN;
    text_reader_t reader = {0};
    if (reader_open(&reader, entry->path) != FR_OK) return false;
    char line[LOGFMT_MAX_LINE + 1];
    size_t length;
    bool first = true;
    bool first_line = true;
    uint64_t previous = 0, last = 0;
    uint32_t total = 0;
    int status;
    while ((status = reader_line(&reader, line, &length)) == 1) {
        if (first_line && !strncmp(line, "# REVD1 ", 8)) {
            unsigned nbr, dbr, fd;
            if (sscanf(line + 8, "nbr=%u dbr=%u fd=%u", &nbr, &dbr, &fd) == 3 && fd <= 1) {
                entry->info.nbr = nbr;
                entry->info.dbr = dbr;
                entry->info.fd = fd != 0;
                entry->info.imported = false;
                bind_legacy_session(entry);
            }
        }
        first_line = false;
        can_frame_t frame;
        logfmt_result_t parsed = logfmt_parse_line(line, length, &frame);
        if (parsed == LOGFMT_SKIP) continue;
        if (parsed != LOGFMT_OK || (!first && frame.ts_us < previous) || total == UINT32_MAX) {
            status = -2;
            break;
        }
        if (first) entry->base_us = frame.ts_us;
        first = false;
        previous = last = frame.ts_us;
        total++;
        if (frame.ch < CH_NUM) {
            entry->count[frame.ch]++;
            entry->last_us[frame.ch] = frame.ts_us;
        }
        if (entry->info.channels <= frame.ch) entry->info.channels = frame.ch + 1u;
        if (frame.flags & CANF_FD) entry->info.fd = true;
    }
    reader_close(&reader);
    entry->info.frames = total;
    uint64_t duration_ms = first ? 0 : (last - entry->base_us + 999u) / 1000u;
    entry->info.duration_ms = duration_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_ms;
    entry->valid = status == 0 && total > 0;
    entry->info.sample_available = entry->valid;
    entry->info.sample_frames = total;
    entry->info.kind = entry->valid ? FK_CANDUMP : FK_UNKNOWN;
    return entry->valid;
}

static void mount_now(void)
{
    if (msc_owner || recording_open) {
        post(EV_SD_MOUNT_DONE, SD_ERR_IO);
        return;
    }
    replay_close_all();
    FRESULT result = f_mount(&fs, "0:", 1);
    mounted = result == FR_OK;
    nfiles = 0;
    media_generation++;
    stats.catalog_limited = stats.catalog_omitted = 0;
    session_max = 0;
    total_kb = free_kb = 0;
    if (mounted) {
        result = f_mkdir(SD_DIR);
        if (result == FR_EXIST) result = FR_OK;
        if (result == FR_OK) result = enumerate("0:", false);
        if (result == FR_OK) result = enumerate(SD_DIR, true);
        if (result == FR_OK) files_on_mount();
        update_space();
    }
    if (result != FR_OK && mounted) {
        (void)f_mount(NULL, "0:", 0);
        mounted = false;
    }
    post(EV_SD_MOUNT_DONE, result_code(result));
}

static void unmount_now(void)
{
    if (recording_open || player_any_active()) return;
    replay_close_all();
    if (mounted) (void)f_mount(NULL, "0:", 0);
    mounted = false;
    media_generation++;
    files_on_unmount();
    nfiles = 0;
    stats.catalog_limited = stats.catalog_omitted = 0;
}

bool storage_sd_media_give(void)
{
    if (mounted || recording_open || close_pending || command_in != command_out ||
        frame_in != frame_out || player_any_active() || io_busy || !card_init())
        return false;
    replay_close_all();
    if (!wait_transfer()) return false;
    msc_owner = true;
    return true;
}

void storage_sd_media_reclaim(void)
{
    /* USB reset/suspend/detach also invokes port_usb_msc(false) while local
     * recording is active. A no-op reclaim must preserve the local volume. */
    if (!msc_owner) return;
    msc_owner = false;
    /* Host may have changed the partition table/FAT; no stale cached sectors
     * survive the next f_mount. Do not format or mount from a USB callback. */
    mounted = false;
    media_generation++;
    files_on_unmount();
    nfiles = 0;
    stats.catalog_limited = stats.catalog_omitted = 0;
}

static void record_error(void)
{
    uint32_t now = HAL_GetTick();
    bool report = !error_reported || (uint32_t)(now - error_at) >= SD_ERROR_REPEAT_MS;
    recording_fault = true;
    /* Sticky FatFs errors can be observed again during a close/retry in the
     * same task. One physical failure must not consume all retries at once. */
    if (report) {
        error_reported = true;
        error_at = now;
        post(EV_SD_WRITE_ERR, 1);
    }
}

static bool flush_record(bool sync)
{
    bool recovering = recording_fault;
    if (!recording_open || !mounted || msc_owner || !port_sd_present()) return false;
    if (record_used) {
        UINT written = 0;
        uint32_t started = HAL_GetTick();
        stats.write_calls++;
        FRESULT result = f_write(&recording, record_buffer, record_used, &written);
        maximum(&stats.max_write_ms, (uint32_t)(HAL_GetTick() - started));
        /* A short successful write means full media; retain only the unwritten
         * suffix so deleting an old segment can retry without duplicating data. */
        if (written > record_used) written = record_used;
        record_used -= written;
        recorded_bytes += written;
        stats.written_bytes += written;
        if (record_used) memmove(record_buffer, record_buffer + written, record_used);
        if (result != FR_OK) {
            record_error();
            return false;
        }
        if (record_used) {
            if (!full_reported) post(EV_SD_FULL, 0);
            full_reported = true;
            return false;
        }
    }
    if (sync) {
        uint32_t started = HAL_GetTick();
        stats.sync_calls++;
        FRESULT result = f_sync(&recording);
        maximum(&stats.max_sync_ms, (uint32_t)(HAL_GetTick() - started));
        if (result != FR_OK) {
            record_error();
            return false;
        }
        sync_at = HAL_GetTick();
        update_space();
    }
    recording_fault = false;
    if (recovering) post(EV_SD_WRITE_RECOVERED, SD_OK);
    return true;
}

static FRESULT close_record(void)
{
    if (!recording_open) {
        bool failed = recording_fault;
        recording_fault = full_reported = false;
        record_used = 0;
        return failed ? FR_DISK_ERR : FR_OK;
    }
    bool flushed = flush_record(false);
    FRESULT result = f_close(&recording);
    recording_open = false;
    stats.segments_closed++;
    {
        sd_file_t *entry = current_recording();
        entry->info.writing = false;
        entry->info.size_kb = (uint32_t)((recorded_bytes + 1023u) / 1024u);
        /* Encoder accepted every cached record. Avoid rereading a potentially
         * huge just-closed session from the UI's metadata query. */
        entry->scanned = flushed && result==FR_OK;
        entry->valid = entry->scanned && entry->valid && entry->info.frames>0;
        entry->info.sample_available=entry->scanned;
    }
    recording_index = -1;
    record_used = 0;
    recording_fault = full_reported = false;
    update_space();
    return !flushed && result == FR_OK ? FR_DISK_ERR : result;
}

static sd_result_t open_segment(uint32_t segment)
{
    if (!mounted || msc_owner || !port_sd_present() || recording_open ||
        !recording_session || recording_session > 9999999u || !segment ||
        segment > 99999999u || !recording_segment_kb ||
        recording_segment_kb > UINT32_MAX / 1024u)
        return SD_ERR_IO;
    char path[SD_PATH_SIZE];
    FRESULT result=ensure_session(recording_directory,recording_session);
    if(result!=FR_OK)return result_code(result);
    int path_length=snprintf(path,sizeof path,"%s/%08lu.LOG",recording_directory,(unsigned long)segment);
    if(path_length<0||(size_t)path_length>=sizeof path)return SD_ERR_IO;
    result = f_open(&recording, path, FA_CREATE_NEW | FA_WRITE);
    if (result != FR_OK) return result_code(result);
    /* opt=0 prepares the next contiguous extent while leaving logical EOF at
     * zero. No uninitialized future payload is ever advertised as valid log. */
    result = f_expand(&recording, (FSIZE_t)recording_segment_kb * 1024u, 0);
    if (result != FR_OK) {
        (void)f_close(&recording);
        (void)f_unlink(path);
        update_space();
        return result == FR_DENIED ? (free_kb < recording_segment_kb ? SD_ERR_FULL : SD_ERR_FRAG)
                                   : result_code(result);
    }
    recording_open = true;
    CAPTURE_LOCK();
    atomic_store_explicit(&accepting_frames,
        !atomic_load_explicit(&close_pending, memory_order_acquire), memory_order_release);
    CAPTURE_UNLOCK();
    recording_fault = full_reported = false;
    record_used = 0;
    recorded_bytes = 0;
    sync_at = HAL_GetTick();
    FILINFO fileinfo = {0};
    if (add_file(path, &fileinfo)) recording_index = (int)nfiles - 1;
    else {
        int replace=-1;
        for(unsigned i=0;i<nfiles;i++)if(!files[i].info.writing&&!player_file_in_use((int)i)){
            replace=(int)i;
            if(files[i].token==recording_token&&files[i].session==recording_session)break;
        }
        if(replace>=0){
            for(int ch=0;ch<CH_NUM;ch++)if(replay[ch].index==replace){reader_close(&replay[ch].text);replay[ch].index=-1;replay[ch].cached_valid=false;}
            recording_index=replace;
            describe_file(&files[replace],path,&fileinfo);
        }else{
            recording_index = -1;
            describe_file(&unlisted_recording, path, &fileinfo);
        }
        stats.catalog_limited = 1;
        stats.catalog_omitted++;
    }
    stats.segments_opened++;
    sd_file_t *entry = current_recording();
    entry->info.imported = false;
    entry->info.is_session=true;
    entry->session=recording_session;entry->token=recording_token;
    entry->scanned=true;entry->valid=true;
    entry->info.writing = true;
    entry->info.nbr = recording_nbr;
    entry->info.dbr = recording_dbr;
    entry->info.fd = recording_fd;
    int header = snprintf((char *)record_buffer, sizeof record_buffer,
                          "# REVD1 nbr=%lu dbr=%lu fd=%u\n",
                          (unsigned long)recording_nbr, (unsigned long)recording_dbr,
                          recording_fd ? 1u : 0u);
    if (header < 0 || (unsigned)header >= sizeof record_buffer) {
        record_error();
        (void)close_record();
        return SD_ERR_IO;
    }
    record_used = (UINT)header;
    if (!flush_record(true)) {
        (void)close_record();
        return SD_ERR_IO;
    }
    if (recording_session > session_max) session_max = recording_session;
    return SD_OK;
}

void port_sd_prealloc(uint32_t session, uint32_t segment, uint32_t segment_kb,
                      uint32_t nbr, uint32_t dbr, bool fd)
{
    if (recording_open || close_pending || !command_push((sd_command_t){
        .type = CMD_PREALLOC, .a = session, .b = segment, .c = segment_kb,
        .d = nbr, .e = dbr, .flag = fd })) command_failed(EV_SD_PREALLOC_DONE);
}

static bool next_segment_now(uint32_t segment)
{
    /* Keep the ROTATE command and its frame fence while a full card is being
     * reclaimed. The old segment was already closed on the first attempt. */
    if (full_reported) return false;
    if (recording_open && close_record() != FR_OK) {
        record_error();
        return true;
    }
    sd_result_t result = open_segment(segment);
    if (result == SD_ERR_FULL) {
        full_reported = true;
        post(EV_SD_FULL, 0);
        return false;
    }
    if (result != SD_OK) record_error();
    return true;
}

bool port_sd_record_frame(const can_frame_t *frame)
{
    /* A short CPU-only publication window serializes close/removal with the
     * producer. The release store occurs AFTER copying all 80 payload bytes. */
    if (!frame || !producer_context()) return false;
    CAPTURE_LOCK();
    if (!atomic_load_explicit(&accepting_frames, memory_order_acquire)
#ifndef REVD_FREERTOS
        || !mounted || msc_owner || !initialized || !detect_stable
#endif
       ) { CAPTURE_UNLOCK(); return false; }
    uint32_t in = atomic_load_explicit(&frame_in, memory_order_relaxed);
    uint32_t out = atomic_load_explicit(&frame_out, memory_order_acquire);
    if (in - out == RECORD_QUEUE_FRAMES) {
        stats.queue_overflows++;
        atomic_store_explicit(&overflow_pending, true, memory_order_release);
        CAPTURE_UNLOCK();
        return false;
    }
    frame_queue[in % RECORD_QUEUE_FRAMES] = *frame;
    atomic_store_explicit(&frame_in, in + 1u, memory_order_release);
    stats.enqueued_frames++;
    uint32_t queued = in + 1u - out;
    if (queued > stats.queue_high_water) stats.queue_high_water = queued;
    CAPTURE_UNLOCK();
#ifdef REVD_FREERTOS
    revd_rtos_notify_storage();
#endif
    return true;
}

/* At most 64 encoded frames and one f_write per ordinary task iteration.
 * FatFs is synchronous, so each disk wait cooperatively services real-time I/O.
 * A command fence preserves exact segment/stop ordering across queued frames. */
static void drain_frames(uint32_t fence)
{
    bool wrote = false;
    for (unsigned budget = 0; budget < RECORD_TASK_FRAMES && frame_out != fence;
         budget++) {
        if (recording_fault || !recording_open) return;
        const can_frame_t *frame = &frame_queue[frame_out % RECORD_QUEUE_FRAMES];
        char line[LOGFMT_MAX_LINE + 1];
        int length = logfmt_format_line(frame, line, sizeof line);
        if (length < 0) { record_error(); return; }
        if ((UINT)length > sizeof record_buffer - record_used) {
            if (wrote || !flush_record(false)) return;
            wrote = true;
        }
        memcpy(record_buffer + record_used, line, (size_t)length);
        record_used += (UINT)length;
        stats.encoded_frames++;
        {
            sd_file_t *entry=current_recording();
            file_info_t *info = &entry->info;
            if(!info->frames)entry->base_us=frame->ts_us;
            else if(frame->ts_us<(entry->last_us[0]>entry->last_us[1]?entry->last_us[0]:entry->last_us[1]))entry->valid=false;
            info->frames++;
            info->sample_frames++;
            if(frame->ch<CH_NUM){entry->count[frame->ch]++;entry->last_us[frame->ch]=frame->ts_us;}
            uint64_t duration=frame->ts_us>=entry->base_us?(frame->ts_us-entry->base_us+999u)/1000u:0;
            if(duration>info->duration_ms)info->duration_ms=duration>UINT32_MAX?UINT32_MAX:(uint32_t)duration;
            if (info->channels <= frame->ch) info->channels = frame->ch + 1u;
            if (frame->flags & CANF_FD) info->fd = true;
        }
        /* Do not release the slot until every access to frame is complete. */
        atomic_fetch_add_explicit(&frame_out, 1u, memory_order_release);
    }
    if (!wrote && record_used >= sizeof record_buffer - LOGFMT_MAX_LINE)
        (void)flush_record(false);
}

void port_sd_write(uint32_t bytes)
{
    /* Existing recorder calls this for accounting after record_frame. Real I/O
     * uses the actual candump byte count, never this binary-format estimate. */
    (void)bytes;
}

static void reinit_now(void)
{
    /* No power switch exists on Rev D. Resetting/re-mounting underneath an open
     * FIL would invalidate its FAT chain and hide loss. Retry only when the
     * controller/card is back in transfer state; FatFs sticky errors propagate
     * again and rec_sm stops after its three-error policy. */
    if (recording_open && storage_sd_disk_sync() && !f_error(&recording))
        (void)flush_record(true);
}

void port_sd_close(void)
{
    if (!producer_context()) return;
    CAPTURE_LOCK();
    atomic_store_explicit(&accepting_frames, false, memory_order_release);
    close_fence = atomic_load_explicit(&frame_in, memory_order_acquire);
    close_command_fence = atomic_load_explicit(&command_in, memory_order_acquire);
    atomic_store_explicit(&close_pending, true, memory_order_release);
    CAPTURE_UNLOCK();
#ifdef REVD_FREERTOS
    revd_rtos_notify_storage();
#endif
}

bool port_sd_replay_info(int index, uint8_t source, uint32_t *frames, uint64_t *duration_us)
{
    if (!frames || !duration_us || source >= CH_NUM || !mounted || msc_owner ||
        index < 0 || (unsigned)index >= nfiles || files[index].info.writing ||
        (!files[index].scanned && recording_open) ||
        !scan_file((unsigned)index) || !files[index].count[source]) return false;
    *frames = files[index].count[source];
    *duration_us = files[index].last_us[source] - files[index].base_us;
    return true;
}

bool port_sd_read_frame(int index, uint8_t source, uint32_t ordinal, can_frame_t *out)
{
    if (!out || source >= CH_NUM || !mounted || msc_owner || index < 0 ||
        (unsigned)index >= nfiles || files[index].info.writing ||
        (!files[index].scanned && recording_open) ||
        !scan_file((unsigned)index) || ordinal >= files[index].count[source]) return false;
    replay_reader_t *reader = &replay[source];
    if (reader->index == index && reader->cached_valid && reader->cached_ordinal == ordinal) {
        *out = reader->cached;
        return true;
    }
    if (reader->index != index || !reader->text.open || ordinal < reader->next) {
        if (reader_open(&reader->text, files[index].path) != FR_OK) return false;
        reader->index = index;
        reader->next = 0;
        reader->cached_valid = false;
    }
    char line[LOGFMT_MAX_LINE + 1];
    size_t length;
    while (reader_line(&reader->text, line, &length) == 1) {
        can_frame_t frame;
        logfmt_result_t parsed = logfmt_parse_line(line, length, &frame);
        if (parsed == LOGFMT_SKIP) continue;
        if (parsed != LOGFMT_OK) return false;
        if (frame.ch != source) continue;
        uint32_t current = reader->next++;
        if (current != ordinal) continue;
        frame.ts_us -= files[index].base_us;
        reader->cached = frame;
        reader->cached_ordinal = ordinal;
        reader->cached_valid = true;
        *out = frame;
        return true;
    }
    return false;
}

/* The visible catalog is only a 128-entry cache. Ring reclamation must walk
 * the actual session directories, including segments evicted from that cache.
 * One FILINFO is reused across both directory levels; no recursive tree walk. */
static void reclaim_poll(void)
{
#ifndef REVD_FREERTOS
    cooperate();
#else
    taskYIELD();
#endif
}
static bool device_segment_name(const char *name)
{
    if (strlen(name) != 12 || !suffix_is(name, ".LOG")) return false;
    bool nonzero = false;
    for (unsigned i = 0; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
        if (name[i] != '0') nonzero = true;
    }
    return nonzero;
}
static bool reclaim_in_use(const char *path)
{
    if (recording_open && !strcmp(path, current_recording()->path)) return true;
    for (unsigned i = 0; i < nfiles; i++)
        if (!strcmp(path, files[i].path) &&
            (files[i].info.writing || files[i].info.protected_ || player_file_in_use((int)i)))
            return true;
    return false;
}
static FRESULT device_segment_owned(const char *path, bool *owned)
{
    *owned = false;
    FIL input;
    FRESULT result = f_open(&input, path, FA_READ);
    if (result != FR_OK) return result;
    char header[8];
    UINT count = 0;
    result = f_read(&input, header, sizeof header, &count);
    FRESULT closed = f_close(&input);
    if (result == FR_OK) result = closed;
    if (result == FR_OK) *owned = count == sizeof header && !memcmp(header, "# REVD1 ", sizeof header);
    return result;
}
/* Keep the DELETE command queued until this bounded discovery completes.
 * That also excludes rename/MSC/format mutations between scan steps. */
#define RECLAIM_ENTRIES_PER_TASK 8u
static struct {
    DIR root, session;
    char directory[SD_PATH_SIZE], oldest[SD_PATH_SIZE];
    bool active, root_open, session_open;
} reclaim;
static FRESULT reclaim_close(void)
{
    FRESULT result = FR_OK;
    if (reclaim.session_open) result = f_closedir(&reclaim.session);
    if (reclaim.root_open) {
        FRESULT closed = f_closedir(&reclaim.root);
        if (result == FR_OK) result = closed;
    }
    reclaim.active = reclaim.root_open = reclaim.session_open = false;
    return result;
}
static void reclaim_cancel(bool media_valid)
{
    if (media_valid) (void)reclaim_close();
    else reclaim.active = reclaim.root_open = reclaim.session_open = false;
}
static bool delete_oldest_now(void)
{
    FRESULT result = FR_OK;
    bool complete = false;
    if (!mounted || msc_owner || !port_sd_present()) {
        reclaim_cancel(false);
        post(EV_SD_DELETE_DONE, 0);
        return true;
    }
    if (!reclaim.active) {
        memset(&reclaim, 0, sizeof reclaim);
        result = f_opendir(&reclaim.root, SD_DIR);
        if (result == FR_OK) reclaim.active = reclaim.root_open = true;
    }
    FILINFO info;
    for (unsigned budget=0; result==FR_OK && budget<RECLAIM_ENTRIES_PER_TASK; budget++) {
        reclaim_poll();
        result = f_readdir(reclaim.session_open ? &reclaim.session : &reclaim.root, &info);
        if (result != FR_OK) break;
        if (reclaim.session_open) {
            if (!info.fname[0]) {
                result = f_closedir(&reclaim.session);
                reclaim.session_open = false;
                continue;
            }
            if (info.fattrib & (AM_DIR | AM_RDO | AM_HID | AM_SYS) ||
                !device_segment_name(info.fname)) continue;
            char path[SD_PATH_SIZE];
            if (!join_path(path, sizeof path, reclaim.directory, info.fname)) continue;
            if ((reclaim.oldest[0] && strcmp(path, reclaim.oldest)>=0) || reclaim_in_use(path)) continue;
            bool owned;
            result = device_segment_owned(path, &owned);
            if (result == FR_OK && owned) memcpy(reclaim.oldest, path, strlen(path)+1u);
            /* One candidate header read at most per task, even if filesystem
             * order repeatedly presents an earlier eligible segment. */
            break;
        }
        if (!info.fname[0]) { complete=true; break; }
        uint32_t session;
        if (!(info.fattrib & AM_DIR) || info.fattrib & (AM_RDO | AM_HID | AM_SYS) ||
            !session_number(info.fname, &session) || !session) continue;
        if (!join_path(reclaim.directory, sizeof reclaim.directory, SD_DIR, info.fname)) continue;
        result = f_opendir(&reclaim.session, reclaim.directory);
        if (result == FR_OK) reclaim.session_open = true;
    }
    if (result==FR_OK && !complete) return false;
    FRESULT closed = reclaim_close();
    if (result == FR_OK) result = closed;
    /* An incomplete scan cannot establish the oldest eligible segment. Check
     * occupancy again: playback may have started between bounded scan steps. */
    if (result!=FR_OK || !reclaim.oldest[0] || reclaim_in_use(reclaim.oldest)) {
        post(EV_SD_DELETE_DONE, 0);
        return true;
    }
    int cached = -1;
    for (unsigned i=0;i<nfiles;i++)
        if (!strcmp(files[i].path,reclaim.oldest)) { cached=(int)i;break; }
    replay_close_all();
    if (f_unlink(reclaim.oldest) != FR_OK) {
        post(EV_SD_DELETE_DONE, 0);
        return true;
    }
    if (cached >= 0) {
#ifndef REVD_FREERTOS
        play_file_removed(cached);
#endif
        for (unsigned i=(unsigned)cached+1u;i<nfiles;i++) files[i-1u]=files[i];
        nfiles--;
        if (recording_index > cached) recording_index--;
#ifdef REVD_FREERTOS
        revd_storage_owner_publish_snapshot();
        revd_storage_file_removed(cached);
#endif
    }
    full_reported = false;
    update_space();
    post(EV_SD_DELETE_DONE, 1);
    return true;
}

static void protect_now(uint32_t target)
{
    if (!mounted || msc_owner) return;
#ifdef REVD_FREERTOS
    /* Historical RTOS commands resolve the index only on the storage owner. */
    unsigned index=target;
    if(index>=nfiles||files[index].info.writing)return;
#else
    if(!target)return;
    unsigned index=0;
    while(index<nfiles&&files[index].file_id!=target)index++;
    if(index==nfiles||files[index].info.writing)return;
#endif
    bool protect = !files[index].info.protected_;
    FRESULT result = f_chmod(files[index].path, protect ? AM_RDO : 0, AM_RDO);
    if (result == FR_OK) files[index].info.protected_ = protect;
    else evt_msg(MSG_SD_ERROR, 0, (int32_t)result);
}

static void format_now(void)
{
    /* Only storage_sm's device-confirmed request reaches this function.
     * There is deliberately no auto-format fallback on mount failures. */
    if (msc_owner || recording_open || player_any_active() || !port_sd_present()) {
        post(EV_SD_FORMAT_DONE, SD_ERR_IO);
        return;
    }
    replay_close_all();
    files_on_unmount();
    (void)f_mount(NULL, "0:", 0);
    mounted = false;
    MKFS_PARM options = { .fmt = FM_FAT | FM_FAT32, .n_fat = 2,
                          .align = 0, .n_root = 0, .au_size = 0 };
    /* Reuse recording RAM; no recording is allowed while formatting. */
    FRESULT result = f_mkfs("0:", &options, record_buffer, sizeof record_buffer);
    if (result == FR_OK) result = f_mount(&fs, "0:", 1);
    mounted = result == FR_OK;
    if (mounted) {
        result = f_mkdir(SD_DIR);
        if (result == FR_EXIST) result = FR_OK;
    }
    nfiles = 0;
    stats.catalog_limited = stats.catalog_omitted = 0;
    session_max = 0;
    media_generation++;
    total_kb = free_kb = 0;
    if (result != FR_OK && mounted) {
        (void)f_mount(NULL, "0:", 0);
        mounted = false;
    }
    if (mounted) update_space();
    if(mounted)files_on_mount();
    post(EV_SD_FORMAT_DONE, result_code(result));
}

int port_sd_file_count(void) { return mounted && !msc_owner ? (int)nfiles : 0; }
bool port_sd_file_info(int index, file_info_t *out)
{
    if (!out || !mounted || msc_owner || index < 0 || (unsigned)index >= nfiles) return false;
    /* Cached metadata only. Historical payload counts remain explicitly
     * unindexed; opening a file list must not read every captured log. */
    *out = files[index].info;
    return true;
}
uint32_t port_sd_free_kb(void) { return mounted && !msc_owner ? free_kb : 0; }
uint32_t port_sd_total_kb(void) { return mounted && !msc_owner ? total_kb : 0; }
uint32_t port_sd_next_session(void) { return session_max < 9999999u ? session_max + 1u : 0; }
bool port_sd_busy(void)
{ return io_busy || full_reported || recording_fault || close_pending ||
         command_in != command_out || frame_in != frame_out; }

void port_sd_mount(void)
{ if (!command_push((sd_command_t){ .type = CMD_MOUNT })) command_failed(EV_SD_MOUNT_DONE); }
void port_sd_unmount(void)
{ if (!command_push((sd_command_t){ .type = CMD_UNMOUNT })) api_error = true; }
void port_sd_next_segment(uint32_t segment)
{
    /* This command producer runs only on core, which owns cfg. Capture the
     * same-event value here; the SD owner consumes descriptor fields only. */
    cfg_t copy = *cfg_get();
    const cfg_t *configuration = &copy;
    if (!command_push((sd_command_t){ .type = CMD_ROTATE, .a = segment,
        .d = configuration->nbr[0], .e = configuration->dbr[0],
        .flag = configuration->fd[0] })) {
        api_error = true;
        deferred_write_error = true;
    }
}
void port_sd_reinit(void)
{ if (!command_push((sd_command_t){ .type = CMD_REINIT })) {
    api_error = true;
    deferred_write_error = true;
} }
void port_sd_delete_oldest(void)
{ if (!command_push((sd_command_t){ .type = CMD_DELETE })) post(EV_SD_DELETE_DONE, 0); }
void port_sd_protect_toggle(int index)
{
#ifdef REVD_FREERTOS
    /* Do not read storage-owned catalog memory from core. Stable file identity
     * is applied to the maintained bare-metal path below; RTOS stays unchanged. */
    if(!command_push((sd_command_t){ .type=CMD_PROTECT, .a=(uint32_t)index }))api_error=true;
#else
    if(!mounted||msc_owner||index<0||(unsigned)index>=nfiles)return;
    if(!command_push((sd_command_t){ .type=CMD_PROTECT, .a=files[index].file_id }))api_error=true;
#endif
}
void port_sd_format(void)
{ if (!command_push((sd_command_t){ .type = CMD_FORMAT })) command_failed(EV_SD_FORMAT_DONE); }

static bool execute_command(const sd_command_t *command)
{
    switch (command->type) {
    case CMD_NOP: break;
    case CMD_MOUNT: mount_now(); break;
    case CMD_UNMOUNT: unmount_now(); break;
    case CMD_PREALLOC:
        if (recording_open) { post(EV_SD_PREALLOC_DONE, SD_ERR_IO); break; }
        overflow_reported = false;
        error_reported = false;
        atomic_store_explicit(&overflow_pending, false, memory_order_release);
        recording_session = command->a;
        recording_token=++file_serial;if(!recording_token)recording_token=++file_serial;
        recording_media_generation=media_generation;
        snprintf(recording_directory,sizeof recording_directory,SD_DIR "/S%07lu%s%s",
                 (unsigned long)recording_session,record_prefix[0]?"_":"",record_prefix);
        recording_segment_kb = command->c;
        recording_nbr = command->d;
        recording_dbr = command->e;
        recording_fd = command->flag;
        post(EV_SD_PREALLOC_DONE, open_segment(command->b));
        break;
    case CMD_ROTATE:
        if (recording_fault) record_error();
        else {
            recording_nbr = command->d;
            recording_dbr = command->e;
            recording_fd = command->flag;
            return next_segment_now(command->a);
        }
        break;
    case CMD_REINIT: reinit_now(); break;
    case CMD_DELETE:
        if (close_pending) { reclaim_cancel(true);post(EV_SD_DELETE_DONE,0); }
        else (void)delete_oldest_now();
        break;
    case CMD_PROTECT: protect_now(command->a); break;
    case CMD_FORMAT: format_now(); break;
    }
    return true;
}

void storage_sd_task(void)
{
    if (task_active || io_busy) return;
    task_active = true;
    uint32_t task_started = HAL_GetTick();
    uint32_t now = task_started;
    bool reclaim_serviced = false;
    CAPTURE_LOCK();
    bool closing = atomic_load_explicit(&close_pending, memory_order_acquire);
    uint32_t stop_fence = close_fence;
    unsigned stop_commands = close_command_fence;
    CAPTURE_UNLOCK();
    bool present = port_sd_present();
    if (present != detect_raw) {
        detect_raw = present;
        detect_since = now;
    }
    if (detect_raw != detect_stable && (uint32_t)(now - detect_since) >= SD_DEBOUNCE_MS) {
        detect_stable = detect_raw;
        if (!detect_stable) {
            /* Removed media cannot be flushed. Invalidate stale FILs before
             * notifying services; never execute close/write on a new card. */
            reclaim_cancel(false); /* Removed DIR handles must never be closed on a new card. */
            (void)f_mount(NULL, "0:", 0);
            mounted = initialized = false;
            msc_owner = false;
            recording_open = recording_fault = false;
            recording_index = -1;
            record_used = 0;
            CAPTURE_LOCK();
            atomic_store_explicit(&accepting_frames, false, memory_order_release);
            frame_out = frame_in;
            command_out = command_in;
            close_pending = false;
            CAPTURE_UNLOCK();
            /* This task captured the close fence before debouncing removal.
             * The old medium cannot be closed successfully after invalidation. */
            closing = false;
            dma_state = DMA_IDLE;
            for (int ch = 0; ch < CH_NUM; ch++) {
                replay[ch].text.open = false;
                replay[ch].index = -1;
                replay[ch].cached_valid = false;
            }
            nfiles = 0;
            media_generation++;
            files_on_unmount();
            stats.catalog_limited = stats.catalog_omitted = 0;
            total_kb = free_kb = block_count = 0;
            if (sd.Instance == SDMMC1) (void)HAL_SD_DeInit(&sd);
#ifdef CANFD_REVE_TARGET
            sd_rail_off();
#endif
            post(EV_SD_REMOVE, 0);
        } else post(EV_SD_INSERT, 0);
    }
    if (closing && reclaim.active) reclaim_cancel(true);
    if(!flush_events()){task_active=false;return;}
    /* Reclamation/retry must not sit behind frames that cannot be written to
     * a full card. They do not change recording's segment boundary. Leave a
     * no-op in place so sequence fences and a pending close stay meaningful. */
    if (full_reported) {
        for (unsigned sequence = command_out; sequence != command_in; sequence++) {
            sd_command_t *command = &commands[sequence % SD_COMMANDS];
            if (command->type == CMD_DELETE ||
                (command->type == CMD_REINIT && !reclaim.active)) {
                sd_command_t urgent = *command;
                execute_command(&urgent);
                if (urgent.type == CMD_DELETE) reclaim_serviced = true;
                if (urgent.type != CMD_DELETE || !reclaim.active) command->type = CMD_NOP;
                break;
            }
        }
        if (closing && full_reported) recording_fault = true;
    }
    if (closing && command_out == stop_commands) {
        if (!recording_fault) drain_frames(stop_fence);
    } else if (command_in != command_out) {
        sd_command_t command = commands[command_out % SD_COMMANDS];
        bool unfenced = command.type == CMD_NOP || command.type == CMD_DELETE ||
                        command.type == CMD_REINIT;
        /* An urgent DELETE may be scanning ahead of a fenced ROTATE on a
         * full card. Keep directory-changing commands behind its open DIRs;
         * STOP cancels those cursors above before draining the command fence. */
        bool scan_blocks = reclaim.active && !closing &&
                           command.type != CMD_NOP && command.type != CMD_DELETE;
        if (!scan_blocks && !recording_fault && !unfenced) drain_frames(command.fence);
        if (!scan_blocks && (frame_out == command.fence || recording_fault || unfenced)) {
            if (command.type == CMD_DELETE) {
                if (!reclaim_serviced) {
                    execute_command(&command);
                    if (!reclaim.active) command_out++;
                }
            } else {
                /* A full-card rotation remains pending until reclamation or
                 * STOP resolves it; later frames belong to its new segment. */
                if (command.type != CMD_ROTATE) command_out++;
                if (execute_command(&command) && command.type == CMD_ROTATE) command_out++;
            }
        }
    } else if (recording_open && !recording_fault) {
        drain_frames(frame_in);
        if ((uint32_t)(HAL_GetTick() - sync_at) >= SD_SYNC_MS) (void)flush_record(true);
    }
    if (closing && command_out == stop_commands &&
        (frame_out == stop_fence || recording_fault || !recording_open)) {
        bool pending_loss = frame_out != stop_fence;
        stats.discarded_frames += frame_in - frame_out;
        frame_out = frame_in;
        FRESULT closed = close_record();
        CAPTURE_LOCK();
        close_pending = false;
        CAPTURE_UNLOCK();
        bool command_error = atomic_exchange_explicit(&api_error, false, memory_order_acq_rel);
        post(EV_SD_CLOSE_DONE, pending_loss || command_error ? SD_ERR_IO : result_code(closed));
    }
    if (atomic_exchange_explicit(&deferred_write_error, false, memory_order_acq_rel)) {
        /* A lost segment command cannot be repaired by resetting the card. */
        recording_fault = true;
        error_at = now;
        post(EV_SD_WRITE_ERR, 2);
    }
    if (!overflow_reported && atomic_exchange_explicit(&overflow_pending, false, memory_order_acq_rel)) {
        overflow_reported = true;
        post(EV_SD_WRITE_ERR, 2); /* Exact loss counter is in storage_sd_stats. */
    }
    /* A FatFs wait may have advanced the clock past entry's now and a newly
     * recorded error_at. Re-read it here: subtracting stale now underflows and
     * spuriously emits a second error immediately after a long-busy timeout. */
    now = HAL_GetTick();
    if (recording_fault && (uint32_t)(now - error_at) >= SD_ERROR_REPEAT_MS) {
        error_at = now;
        error_reported = true;
        post(EV_SD_WRITE_ERR, 1);
    }
    /* Player stop has no platform close API. Retire idle readers here so
     * f_chmod/unlink and MSC handover never retain abandoned open handles. */
    for (int ch = 0; ch < CH_NUM; ch++) {
        if (player_is_idle((unsigned)ch) && replay[ch].text.open) {
            reader_close(&replay[ch].text);
            replay[ch].index = -1;
            replay[ch].cached_valid = false;
        }
    }
    files_task();
    (void)flush_events();
    maximum(&stats.max_task_ms, (uint32_t)(HAL_GetTick() - task_started));
    task_active = false;
}

#include "storage_files.inc"

#ifdef REVD_FREERTOS
bool storage_sd_peek_file_info(int index, file_info_t *out)
{
    if (!out || !mounted || msc_owner || index < 0 || (unsigned)index >= nfiles) return false;
    *out = files[index].info;
    return true;
}
bool storage_sd_recording_active(void)
{
    return atomic_load_explicit(&recording_open, memory_order_acquire) ||
        atomic_load_explicit(&accepting_frames, memory_order_acquire) ||
        atomic_load_explicit(&close_pending, memory_order_acquire);
}
bool storage_sd_queue_busy(void)
{
    return atomic_load_explicit(&task_active, memory_order_acquire) ||
        atomic_load_explicit(&io_busy, memory_order_acquire) ||
        atomic_load_explicit(&close_pending, memory_order_acquire) ||
        atomic_load_explicit(&command_in, memory_order_acquire) != atomic_load_explicit(&command_out, memory_order_acquire) ||
        atomic_load_explicit(&frame_in, memory_order_acquire) != atomic_load_explicit(&frame_out, memory_order_acquire);
}
void storage_sd_capture_stats(storage_sd_stats_t *out)
{
    CAPTURE_LOCK();
    out->queued_frames = atomic_load_explicit(&frame_in, memory_order_acquire) - atomic_load_explicit(&frame_out, memory_order_acquire);
    out->enqueued_frames = stats.enqueued_frames;
    out->queue_high_water = stats.queue_high_water;
    out->queue_overflows = stats.queue_overflows;
    out->command_high_water = stats.command_high_water;
    out->command_overflows = stats.command_overflows;
    CAPTURE_UNLOCK();
}
#endif
