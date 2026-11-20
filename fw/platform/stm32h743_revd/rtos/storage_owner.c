/* SPDX-License-Identifier: MIT
 * FreeRTOS SD owner: bounded command/frame ingestion stays non-blocking;
 * all card/filesystem/replay/MSC operations execute on one storage task.
 */
#include "storage_owner.h"
#include "stm32h7xx_hal.h"
#include "storage_owner_driver.h"
#include "runtime.h"
#include "services/player/play_sm.h"
#include "services/recorder/rec_sm.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdatomic.h>
#include <string.h>

#define RPC_DEPTH 8u
#define CATALOG_MAX 128u
#define RPC_NOTIFICATION_INDEX 1u
#if configTASK_NOTIFICATION_ARRAY_ENTRIES < 2
#error Storage RPC needs task notification index 1; runtime DMA uses index 0.
#endif
#ifdef CANFD_REVD_TARGET
#define OWNER_RAM __attribute__((section(".revd_d2_bss"),aligned(32)))
#else
#define OWNER_RAM
#endif

typedef enum { RPC_GIVE, RPC_RECLAIM, RPC_SYNC, RPC_READ, RPC_WRITE,
               RPC_REPLAY_INFO, RPC_REPLAY_FRAME, RPC_FILE_INFO } rpc_op_t;
typedef struct {
    bool ok;
    uint32_t frames;
    uint64_t duration_us;
} rpc_reply_t;
typedef struct {
    rpc_op_t op;
    int index;
    uint32_t a, b;
    void *buffer;
    rpc_reply_t *reply;
    TaskHandle_t waiter;
} rpc_t;
typedef struct {
    file_info_t files[CATALOG_MAX];
    int nfiles;
    bool present, busy, raw_ready;
    uint32_t free_kb,total_kb,next_session,raw_blocks;
    storage_sd_stats_t stats;
} snapshot_t;
typedef struct {cfg_t cfg;play_state_t state[CH_NUM];int file[CH_NUM];bool recording;} context_t;
static snapshot_t snapshots[2] OWNER_RAM;
static context_t context OWNER_RAM;
static unsigned active_snapshot;
static _Atomic bool ready, rpc_active;
static _Atomic(TaskHandle_t) owner;
static StaticQueue_t rpc_queue_control;
static uint8_t rpc_queue_bytes[RPC_DEPTH * sizeof(rpc_t)] OWNER_RAM;
static QueueHandle_t rpc_queue;
static bool scan_queued[CATALOG_MAX];

static bool task_context(void)
{
    return __get_IPSR() == 0u && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}
bool revd_storage_is_owner(void)
{ return task_context() && owner && xTaskGetCurrentTaskHandle() == owner; }
bool revd_storage_owner_ready(void)
{ return atomic_load_explicit(&ready,memory_order_acquire); }
void revd_storage_owner_bootstrap(void)
{
    /* Startup only, before scheduler and before any public operation. */
    memset(snapshots,0,sizeof snapshots);
    memset(&context,0,sizeof context);
    memset(scan_queued,0,sizeof scan_queued);
    active_snapshot=0;owner=NULL;
    atomic_store_explicit(&ready,false,memory_order_relaxed);
    rpc_queue=xQueueCreateStatic(RPC_DEPTH,sizeof(rpc_t),rpc_queue_bytes,&rpc_queue_control);
    configASSERT(rpc_queue);
}
void revd_storage_publish_context(void)
{
    context_t copy={.cfg=*cfg_get(),.recording=rec_get()->state!=REC_IDLE};
    for(unsigned ch=0;ch<CH_NUM;ch++) {
        const player_t *player=play_get((int)ch);
        copy.state[ch]=player->state;copy.file[ch]=player->file;
    }
    taskENTER_CRITICAL();context=copy;taskEXIT_CRITICAL();
}
void revd_storage_config_snapshot(cfg_t *out)
{ if(out) {taskENTER_CRITICAL();*out=context.cfg;taskEXIT_CRITICAL();} }
bool revd_storage_play_any_active(void)
{
    bool active=false;taskENTER_CRITICAL();
    for(unsigned ch=0;ch<CH_NUM;ch++)active|=context.state[ch]!=PL_IDLE;
    taskEXIT_CRITICAL();return active;
}
bool revd_storage_play_file_in_use(int index)
{
    bool used=false;taskENTER_CRITICAL();
    for(unsigned ch=0;ch<CH_NUM;ch++)used|=context.state[ch]!=PL_IDLE&&context.file[ch]==index;
    taskEXIT_CRITICAL();return used;
}
bool revd_storage_player_idle(unsigned ch)
{
    if(ch>=CH_NUM)return true;
    taskENTER_CRITICAL();bool idle=context.state[ch]==PL_IDLE;taskEXIT_CRITICAL();return idle;
}
static bool recording_priority(void)
{
    taskENTER_CRITICAL();bool active=context.recording;taskEXIT_CRITICAL();
    return active||storage_sd_recording_active();
}
void revd_storage_owner_publish_snapshot(void)
{
    configASSERT(revd_storage_is_owner());
    /* Build inactive bank without masking interrupts. Readers copy only a
     * single selected file/header under the short critical publication lock. */
    unsigned next=active_snapshot^1u;snapshot_t *copy=&snapshots[next];
    copy->nfiles=owner_port_sd_file_count();
    if(copy->nfiles>(int)CATALOG_MAX)copy->nfiles=CATALOG_MAX;
    for(int i=0;i<copy->nfiles;i++)(void)storage_sd_peek_file_info(i,&copy->files[i]);
    copy->present=owner_port_sd_present();copy->busy=owner_port_sd_busy()||recording_priority();
    copy->free_kb=owner_port_sd_free_kb();copy->total_kb=owner_port_sd_total_kb();
    copy->next_session=owner_port_sd_next_session();copy->raw_ready=owner_storage_sd_raw_ready();
    copy->raw_blocks=owner_storage_sd_raw_block_count();
    owner_storage_sd_stats(&copy->stats);
    taskENTER_CRITICAL();active_snapshot=next;taskEXIT_CRITICAL();
}
void revd_storage_owner_init(void)
{
    configASSERT(task_context()&&rpc_queue&&!owner);
    owner=xTaskGetCurrentTaskHandle();storage_sd_init();revd_storage_owner_publish_snapshot();
    atomic_store_explicit(&ready,true,memory_order_release);
}
static rpc_reply_t execute_rpc(const rpc_t *rpc)
{
    rpc_reply_t reply={0};configASSERT(revd_storage_is_owner());
    atomic_store_explicit(&rpc_active,true,memory_order_release);
    switch(rpc->op) {
    case RPC_GIVE: reply.ok=owner_storage_sd_media_give();break;
    case RPC_RECLAIM: owner_storage_sd_media_reclaim();reply.ok=true;break;
    case RPC_SYNC: reply.ok=owner_storage_sd_raw_sync();break;
    case RPC_READ: reply.ok=owner_storage_sd_raw_read(rpc->a,rpc->buffer,rpc->b);break;
    case RPC_WRITE: reply.ok=owner_storage_sd_raw_write(rpc->a,rpc->buffer,rpc->b);break;
    case RPC_REPLAY_INFO:
        reply.ok=owner_port_sd_replay_info(rpc->index,(uint8_t)rpc->a,&reply.frames,&reply.duration_us);break;
    case RPC_REPLAY_FRAME:
        reply.ok=owner_port_sd_read_frame(rpc->index,(uint8_t)rpc->a,rpc->b,rpc->buffer);break;
    case RPC_FILE_INFO: {
        file_info_t info;
        if(!recording_priority())reply.ok=owner_port_sd_file_info(rpc->index,&info);
        if(rpc->index>=0&&rpc->index<(int)CATALOG_MAX) {
            taskENTER_CRITICAL();scan_queued[rpc->index]=false;taskEXIT_CRITICAL();
        }
        break;
    }
    }
    atomic_store_explicit(&rpc_active,false,memory_order_release);
    return reply;
}
void revd_storage_owner_step(void)
{
    configASSERT(revd_storage_is_owner());
    storage_sd_task();
    rpc_t rpc;
    if(xQueueReceive(rpc_queue,&rpc,0)==pdPASS) {
        rpc_reply_t result=execute_rpc(&rpc);
        /* Publish completion state before waking a higher-priority caller. */
        revd_storage_owner_publish_snapshot();
        if(rpc.reply)*rpc.reply=result;
        if(rpc.waiter)xTaskNotifyGiveIndexed(rpc.waiter,RPC_NOTIFICATION_INDEX);
    } else revd_storage_owner_publish_snapshot();
}
static bool rpc_call(rpc_t rpc,rpc_reply_t *result)
{
    if(!result||!task_context()||!revd_storage_owner_ready()||!rpc_queue)return false;
    if(revd_storage_is_owner()) {*result=execute_rpc(&rpc);revd_storage_owner_publish_snapshot();return true;}
    rpc_reply_t reply={0};rpc.reply=&reply;rpc.waiter=xTaskGetCurrentTaskHandle();
    /* Index 1 is dedicated to synchronous RPC. There is one outstanding call
     * per task, so stale completions cannot be confused with another request. */
    (void)ulTaskNotifyTakeIndexed(RPC_NOTIFICATION_INDEX,pdTRUE,0);
    /* RPC callers are allowed to block. Wait for bounded queue capacity rather
     * than misreport an imported log as invalid or silently drop void reclaim.
     * This path is never used by per-frame capture or command submission. */
    if(xQueueSend(rpc_queue,&rpc,portMAX_DELAY)!=pdPASS)return false;
    revd_rtos_notify_storage();
    /* Never time out after enqueue: reply and I/O buffers may be on caller's
     * stack and must remain alive until the owner has completely finished.
     * DMA/card waits themselves are bounded by production driver timeouts. */
    while(!ulTaskNotifyTakeIndexed(RPC_NOTIFICATION_INDEX,pdTRUE,portMAX_DELAY)) { }
    *result=reply;return true;
}
bool port_sd_present(void)
{ taskENTER_CRITICAL();bool v=snapshots[active_snapshot].present;taskEXIT_CRITICAL();return v; }
int port_sd_file_count(void)
{ taskENTER_CRITICAL();int n=snapshots[active_snapshot].nfiles;taskEXIT_CRITICAL();return n; }
bool port_sd_file_info(int index,file_info_t *out)
{
    if(!out||index<0||index>=(int)CATALOG_MAX)return false;
    taskENTER_CRITICAL();snapshot_t *copy=&snapshots[active_snapshot];
    bool valid=index<copy->nfiles;if(valid)*out=copy->files[index];
    bool request=valid&&!scan_queued[index]&&!copy->busy;
    if(request)scan_queued[index]=true;
    taskEXIT_CRITICAL();
    /* Query returns cached data immediately. A bounded best-effort request
     * lets the owner fill lazy imported metadata without blocking core/UI. */
    if(request&&rpc_queue&&task_context()) {
        rpc_t rpc={.op=RPC_FILE_INFO,.index=index};
        if(xQueueSend(rpc_queue,&rpc,0)!=pdPASS) {
            taskENTER_CRITICAL();scan_queued[index]=false;taskEXIT_CRITICAL();
        } else revd_rtos_notify_storage();
    }
    return valid;
}
#define CACHED_U32(name,field) uint32_t name(void) { \
    taskENTER_CRITICAL();uint32_t value=snapshots[active_snapshot].field; \
    taskEXIT_CRITICAL();return value; }
CACHED_U32(port_sd_free_kb,free_kb)
CACHED_U32(port_sd_total_kb,total_kb)
CACHED_U32(port_sd_next_session,next_session)
CACHED_U32(storage_sd_raw_block_count,raw_blocks)
bool port_sd_busy(void)
{ taskENTER_CRITICAL();bool value=snapshots[active_snapshot].busy;taskEXIT_CRITICAL();return value||recording_priority()||storage_sd_queue_busy()||atomic_load_explicit(&rpc_active,memory_order_acquire); }
bool storage_sd_raw_ready(void)
{ taskENTER_CRITICAL();bool value=snapshots[active_snapshot].raw_ready;taskEXIT_CRITICAL();return value; }
void storage_sd_stats(storage_sd_stats_t *out)
{
    if(!out)return;
    taskENTER_CRITICAL();*out=snapshots[active_snapshot].stats;taskEXIT_CRITICAL();
    /* Producer counters stay live even while the owner is blocked on SD. */
    storage_sd_capture_stats(out);
}
bool storage_sd_media_give(void)
{ rpc_reply_t r;return rpc_call((rpc_t){.op=RPC_GIVE},&r)&&r.ok; }
void storage_sd_media_reclaim(void)
{ rpc_reply_t r;(void)rpc_call((rpc_t){.op=RPC_RECLAIM},&r); }
bool storage_sd_raw_sync(void)
{ rpc_reply_t r;return rpc_call((rpc_t){.op=RPC_SYNC},&r)&&r.ok; }
bool storage_sd_raw_read(uint32_t lba,uint8_t *data,uint32_t count)
{ rpc_reply_t r;return data&&count&&rpc_call((rpc_t){.op=RPC_READ,.a=lba,.b=count,.buffer=data},&r)&&r.ok; }
bool storage_sd_raw_write(uint32_t lba,const uint8_t *data,uint32_t count)
{ rpc_reply_t r;return data&&count&&rpc_call((rpc_t){.op=RPC_WRITE,.a=lba,.b=count,.buffer=(void *)(uintptr_t)data},&r)&&r.ok; }
bool port_sd_replay_info(int index,uint8_t source,uint32_t *frames,uint64_t *duration_us)
{
    if(!frames||!duration_us||recording_priority())return false;
    rpc_reply_t r;if(!rpc_call((rpc_t){.op=RPC_REPLAY_INFO,.index=index,.a=source},&r)||!r.ok)return false;
    *frames=r.frames;*duration_us=r.duration_us;return true;
}
bool port_sd_read_frame(int index,uint8_t source,uint32_t ordinal,can_frame_t *out)
{ rpc_reply_t r;return out&&!recording_priority()&&rpc_call((rpc_t){.op=RPC_REPLAY_FRAME,.index=index,.a=source,.b=ordinal,.buffer=out},&r)&&r.ok; }
