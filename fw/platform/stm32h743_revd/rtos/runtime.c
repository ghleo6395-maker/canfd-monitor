/* SPDX-License-Identifier: MIT */
#include "runtime.h"
#include "storage_owner.h"
#include "board.h"

#define DTCM_STACK __attribute__((section(".revd_dtcm_bss"), aligned(8)))
static StackType_t core_stack[8192u / sizeof(StackType_t)] DTCM_STACK;
static StackType_t storage_stack[12288u / sizeof(StackType_t)] DTCM_STACK;
static StackType_t usb_stack[6144u / sizeof(StackType_t)] DTCM_STACK;
static StackType_t ui_stack[16384u / sizeof(StackType_t)] __attribute__((aligned(8)));
static StackType_t idle_stack[configMINIMAL_STACK_SIZE] __attribute__((aligned(8)));
static StaticTask_t task_tcb[REVD_RTOS_TASK_COUNT], idle_tcb;
static TaskHandle_t handles[REVD_RTOS_TASK_COUNT];
static revd_rtos_task_metrics_t metrics[REVD_RTOS_TASK_COUNT];
static volatile bool running;
static struct {uint32_t started,last;bool valid;} command_wait[REVD_RTOS_TASK_COUNT];
volatile const char *revd_rtos_assert_file;
volatile uint32_t revd_rtos_assert_line;

void revd_rtos_assert_failed(const char *file, uint32_t line)
{
    revd_rtos_assert_file=file;
    revd_rtos_assert_line=line;
    __disable_irq();
    board_fatal(0x40u);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    revd_rtos_assert_file=name;
    __disable_irq();
    board_fatal(0x41u);
}
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                  configSTACK_DEPTH_TYPE *words)
{
    *tcb=&idle_tcb;*stack=idle_stack;*words=configMINIMAL_STACK_SIZE;
}
bool revd_rtos_running(void) { return running; }
uint32_t revd_rtos_time_us(void) { return TIM2->CNT; }
TaskHandle_t revd_rtos_task_handle(revd_rtos_task_id_t id)
{ return (unsigned)id<REVD_RTOS_TASK_COUNT ? handles[id] : NULL; }
bool revd_rtos_task_metrics(revd_rtos_task_id_t id,revd_rtos_task_metrics_t *out)
{
    if ((unsigned)id>=REVD_RTOS_TASK_COUNT || !out || !handles[id]) return false;
    taskENTER_CRITICAL();
    *out=metrics[id];
    taskEXIT_CRITICAL();
    /* This scans a different task's unused stack pattern; no other task writes
     * into free stack while this reader has preempted it on this single core. */
    out->stack_min_free_bytes=(uint32_t)uxTaskGetStackHighWaterMark(handles[id])*sizeof(StackType_t);
    return true;
}
static void notify_task(revd_rtos_task_id_t id)
{
    if (running && handles[id]) xTaskNotifyGive(handles[id]);
}
void revd_rtos_notify_core(void) { notify_task(REVD_RTOS_CORE); }
void revd_rtos_notify_storage(void) { notify_task(REVD_RTOS_STORAGE); }
void revd_rtos_notify_usb(void) { notify_task(REVD_RTOS_USB); }
void revd_rtos_notify_ui(void) { notify_task(REVD_RTOS_UI); }
static void notify_from_isr(revd_rtos_task_id_t id)
{
    if (running && handles[id]) {
        BaseType_t wake=pdFALSE;
        vTaskNotifyGiveFromISR(handles[id],&wake);
        portYIELD_FROM_ISR(wake);
    }
}
void revd_rtos_notify_storage_from_isr(void) { notify_from_isr(REVD_RTOS_STORAGE); }
void revd_rtos_notify_usb_from_isr(void) { notify_from_isr(REVD_RTOS_USB); }
void revd_rtos_notify_ui_from_isr(void) { notify_from_isr(REVD_RTOS_UI); }
void revd_rtos_notify_core_from_isr_deferred(void)
{
    /* Safe at CAN priority 4: this performs no kernel call or shared mutation. */
    if (running) NVIC_SetPendingIRQ(TIM7_IRQn);
}
void TIM7_IRQHandler(void) { notify_from_isr(REVD_RTOS_CORE); }
void revd_rtos_wait_io(void)
{
    if (running && __get_IPSR()==0u) (void)ulTaskNotifyTake(pdTRUE,1u);
}
void revd_rtos_poll_command_wait(void)
{
    if (!running || __get_IPSR()!=0u) return;
    TaskHandle_t current=xTaskGetCurrentTaskHandle();
    unsigned id=0;
    while (id<REVD_RTOS_TASK_COUNT && handles[id]!=current) id++;
    if (id==REVD_RTOS_TASK_COUNT) {vTaskDelay(1u);return;}
    uint32_t now=revd_rtos_time_us();
    /* LL's existing hook has no command begin/end notification. Reset after
     * an idle gap, otherwise cap the cumulative continuous polling window.
     * No shared task state or critical section is needed: one writer per id. */
    if (!command_wait[id].valid || (uint32_t)(now-command_wait[id].last)>=200u) {
        command_wait[id].valid=true;command_wait[id].started=now;
    }
    command_wait[id].last=now;
    if ((uint32_t)(now-command_wait[id].started)<200u) {taskYIELD();}
    else {command_wait[id].valid=false;vTaskDelay(1u);}
}

static void step_start(revd_rtos_task_id_t id,uint32_t *started)
{ (void)id;*started=TIM2->CNT; }
static void step_end(revd_rtos_task_id_t id,uint32_t started)
{
    uint32_t elapsed=TIM2->CNT-started;
    taskENTER_CRITICAL();
    metrics[id].runs++;
    if (elapsed>metrics[id].max_step_us) metrics[id].max_step_us=elapsed;
    taskEXIT_CRITICAL();
}
/* Notifications provide prompt wake-up with a 1 ms fallback. A continuously
 * notified high-priority owner must still block: cap each burst at four steps
 * or one tick of CPU time, then guarantee a full tick for lower priorities. */
static void owner_wait(unsigned *burst,TickType_t *burst_started)
{
    TickType_t now=xTaskGetTickCount();
    if (++*burst>=4u || (TickType_t)(now-*burst_started)>=1u) {
        vTaskDelay(1u);*burst=0;*burst_started=xTaskGetTickCount();
    } else {
        (void)ulTaskNotifyTake(pdTRUE,1u);
        if (xTaskGetTickCount()!=now) {*burst=0;*burst_started=xTaskGetTickCount();}
    }
}
static void core_task(void *arg)
{
    (void)arg;running=true;
    revd_core_boot();
    unsigned burst=0;TickType_t started=xTaskGetTickCount();
    for (;;) {uint32_t at;step_start(REVD_RTOS_CORE,&at);revd_core_step();step_end(REVD_RTOS_CORE,at);owner_wait(&burst,&started);}
}
static void storage_task(void *arg)
{
    (void)arg;revd_storage_owner_init();
    unsigned burst=0;TickType_t started=xTaskGetTickCount();
    for (;;) {uint32_t at;step_start(REVD_RTOS_STORAGE,&at);revd_storage_owner_step();step_end(REVD_RTOS_STORAGE,at);owner_wait(&burst,&started);}
}
static void usb_task(void *arg)
{
    (void)arg;unsigned burst=0;TickType_t started=xTaskGetTickCount();
    for (;;) {uint32_t at;step_start(REVD_RTOS_USB,&at);revd_usb_transport_step();step_end(REVD_RTOS_USB,at);owner_wait(&burst,&started);}
}
static void ui_task(void *arg)
{
    (void)arg;unsigned burst=0;TickType_t started=xTaskGetTickCount();
    for (;;) {uint32_t at;step_start(REVD_RTOS_UI,&at);revd_ui_step();step_end(REVD_RTOS_UI,at);owner_wait(&burst,&started);}
}
void revd_rtos_start(void)
{
    NVIC_SetPriorityGrouping(3u); /* STM32 NVIC_PRIORITYGROUP_4: 4 preemption bits. */
    NVIC_SetPriority(TIM7_IRQn,6u);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    NVIC_EnableIRQ(TIM7_IRQn); /* Software-only deferred IRQ; no TIM7 clock/IRQ source. */
    revd_rtos_bridges_init();
    metrics[REVD_RTOS_CORE].stack_size_bytes=sizeof core_stack;
    metrics[REVD_RTOS_STORAGE].stack_size_bytes=sizeof storage_stack;
    metrics[REVD_RTOS_USB].stack_size_bytes=sizeof usb_stack;
    metrics[REVD_RTOS_UI].stack_size_bytes=sizeof ui_stack;
    handles[REVD_RTOS_CORE]=xTaskCreateStatic(core_task,"core",sizeof core_stack/sizeof *core_stack,NULL,5,core_stack,&task_tcb[REVD_RTOS_CORE]);
    handles[REVD_RTOS_STORAGE]=xTaskCreateStatic(storage_task,"storage",sizeof storage_stack/sizeof *storage_stack,NULL,4,storage_stack,&task_tcb[REVD_RTOS_STORAGE]);
    handles[REVD_RTOS_USB]=xTaskCreateStatic(usb_task,"usb",sizeof usb_stack/sizeof *usb_stack,NULL,3,usb_stack,&task_tcb[REVD_RTOS_USB]);
    handles[REVD_RTOS_UI]=xTaskCreateStatic(ui_task,"ui",sizeof ui_stack/sizeof *ui_stack,NULL,2,ui_stack,&task_tcb[REVD_RTOS_UI]);
    for (unsigned i=0;i<REVD_RTOS_TASK_COUNT;i++) configASSERT(handles[i]);
    vTaskStartScheduler();
    board_fatal(0x42u);
}
