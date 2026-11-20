/* SPDX-License-Identifier: MIT
 * Actual pinned FreeRTOS GCC/POSIX tasks + deterministic virtual peripheral
 * time. This is a scheduling/ownership model, NOT STM32 performance evidence.
 * Production driver bodies and all 11 scenario checks come from one source.
 */
#include "FreeRTOS.h"
#include "task.h"
static void kernel_model_step(void);
static void kernel_report_extra(void);
#define STRESS_MAIN_STEP() kernel_model_step()
#define STRESS_REPORT_EXTRA() kernel_report_extra()
#define main pipeline_model_main
#define revd_io_wait_poll unused_cooperative_wait
#define stress_f_write original_stress_f_write
#define stress_f_sync original_stress_f_sync
#define stress_f_close original_stress_f_close
#define evt_post4 original_evt_post4
#include "../stress/pipeline_stress.c"
#undef main
#undef revd_io_wait_poll
#undef stress_f_write
#undef stress_f_sync
#undef stress_f_close
#undef evt_post4

enum { CORE_TASK, UI_TASK, STORAGE_TASK, TASKS };
static StaticTask_t controller_tcb, worker_tcbs[TASKS];
static StackType_t controller_stack[8192], worker_stacks[TASKS][8192];
static TaskHandle_t controller_handle, workers[TASKS];
static uint64_t wake_at[TASKS], last_core_at, max_core_gap;
static unsigned task_runs[TASKS], wait_switches, can_during_sd_wait, ui_during_sd_wait;
static unsigned ff_depth, max_ff_depth, owner_errors, sd_wait_depth;
static uint64_t previous_error_at, minimum_error_interval=UINT64_MAX;
static uint64_t busy_generation, short_poll_started;
static bool tick_only_policy;
static int active_task=-1, model_argc, model_result;
static char **model_argv;
static bool started, scheduler_control;

bool evt_post4(ev_type_t e,uint8_t ch,src_t src,int32_t a,int32_t b)
{
    if(e==EV_SD_WRITE_ERR&&a==1) {
        if(previous_error_at&&clock_us-previous_error_at<minimum_error_interval)minimum_error_interval=clock_us-previous_error_at;
        previous_error_at=clock_us;
    }
    return original_evt_post4(e,ch,src,a,b);
}

static void worker_block(uint64_t delay_us)
{
    configASSERT(active_task>=0 && active_task<TASKS);
    int mine=active_task;
    wake_at[mine]=clock_us+delay_us;
    xTaskNotifyGive(controller_handle);
    /* Actual kernel notification parks the current task on its own pthread
     * stack. No nested call to core/LVGL is made from the FatFs call stack. */
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    configASSERT(active_task==mine);
}

void revd_io_wait_poll(void)
{
    if (!started || !scheduler_control) { unused_cooperative_wait(); return; }
    if (active_task!=STORAGE_TASK) {owner_errors++;return;}
    wait_switches++;sd_wait_depth++;
    /* A pending IDMA completion is an interrupt notification, not a mandatory
     * 1 ms timeout. Card programming has no completion IRQ here, so it retains
     * the 1 ms poll timeout. Peripheral timing inputs are unchanged. */
    if(sd_hw.pending) {
        uint64_t wait_us=1000u;
        if(sd_hw.due>clock_us&&sd_hw.due-clock_us<wait_us)wait_us=sd_hw.due-clock_us;
        worker_block(wait_us);
    } else if(tick_only_policy) worker_block(1000u);
    else {
        /* Mirror the production phase policy: one 200 us bounded short-poll
         * budget per programming interval; then block for a 1 ms poll. The
         * 50 us CPU cost matches the unchanged cooperative scenario input. */
        if(busy_generation!=program_busy_until) {
            busy_generation=program_busy_until;short_poll_started=clock_us;
        }
        if(clock_us<program_busy_until&&clock_us-short_poll_started>=200u)worker_block(1000u);
        else {advance(50u);worker_block(0u);}
    }
    sd_wait_depth--;
}

static void ff_enter(void)
{
    if (scheduler_control && active_task!=STORAGE_TASK) owner_errors++;
    ff_depth++;if(ff_depth>max_ff_depth)max_ff_depth=ff_depth;
    if(ff_depth>1)owner_errors++;
}
FRESULT stress_f_write(FIL *f,const void *p,UINT bytes,UINT *written)
{ff_enter();FRESULT result=original_stress_f_write(f,p,bytes,written);ff_depth--;return result;}
FRESULT stress_f_sync(FIL *f)
{ff_enter();FRESULT result=original_stress_f_sync(f);ff_depth--;return result;}
FRESULT stress_f_close(FIL *f)
{ff_enter();FRESULT result=original_stress_f_close(f);ff_depth--;return result;}

static void core_cycle(void)
{
    if(sd_wait_depth)can_during_sd_wait++;
    if(last_core_at&&clock_us-last_core_at>max_core_gap)max_core_gap=clock_us-last_core_at;
    last_core_at=clock_us;
    advance(cpu_loop_us);capture();observe_control_probe();
    if(generating&&accepted[0]+accepted[1]>=next_rotate&&segment<max_segment&&!close_started) {
        port_sd_next_segment(++segment);next_rotate=accepted[0]+accepted[1]+rotate_frames;
    }
}
static void ui_cycle(void)
{
    if(sd_wait_depth)ui_during_sd_wait++;
    if(clock_us>=next_redraw_us) {
        next_redraw_us=clock_us+20000u;redraws++;char text[64];
        snprintf(text,sizeof text,"Dual CAN stress %lu",(unsigned long)redraws);
        lv_label_set_text(label,text);lv_obj_invalidate(lv_display_get_screen_active(lcd));
        lv_timer_ready(lv_display_get_refr_timer(lcd));
    }
    observe_ui_entry();display_running=true;display_task(HAL_GetTick());display_running=false;
}
static void worker_main(void *arg)
{
    int mine=(int)(intptr_t)arg;
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    for(;;) {
        configASSERT(active_task==mine);
        task_runs[mine]++;
        if(mine==CORE_TASK)core_cycle();
        else if(mine==UI_TASK)ui_cycle();
        else storage_sd_task();
        worker_block(1000u);
    }
}
static void kernel_model_step(void)
{
    if(!started) {
        started=true;
        for(unsigned n=0;n<TASKS;n++)wake_at[n]=clock_us;
    }
    scheduler_control=true;
    /* Highest priority ready worker; explicit virtual wake times keep the
     * scenario independent of host load. Kernel context switches are real. */
    int selected=-1;
    for(unsigned n=0;n<TASKS;n++)if(wake_at[n]<=clock_us){selected=(int)n;break;}
    if(selected<0) {
        uint64_t earliest=wake_at[0];
        for(unsigned n=1;n<TASKS;n++)if(wake_at[n]<earliest)earliest=wake_at[n];
        advance(earliest-clock_us);
        for(unsigned n=0;n<TASKS;n++)if(wake_at[n]<=clock_us){selected=(int)n;break;}
    }
    configASSERT(selected>=0);
    active_task=selected;
    xTaskNotifyGive(workers[selected]);
    configASSERT(ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(10000)));
    active_task=-1;
    scheduler_control=false;
}
static void kernel_report_extra(void)
{
    printf(",\"sd_wait_policy\":\"%s\",\"short_busy_budget_us\":200",tick_only_policy?"tick-only-regression":"bounded-poll-then-block");
    printf(",\"minimum_repeated_error_interval_us\":%" PRIu64,minimum_error_interval==UINT64_MAX?0:minimum_error_interval);
    printf(",\"scheduler_model\":\"upstream FreeRTOS V11.3.2 GCC/POSIX with virtual peripheral time\",\"physical_hardware_tested\":false,\"wrapper_ipc_modeled\":false,\"model_worker_period_us\":1000,\"task_runs\":[%u,%u,%u],\"storage_wait_switches\":%u,\"core_runs_during_sd_wait\":%u,\"ui_runs_during_sd_wait\":%u,\"max_core_service_gap_us\":%" PRIu64 ",\"max_fatfs_call_depth\":%u,\"owner_errors\":%u",task_runs[0],task_runs[1],task_runs[2],wait_switches,can_during_sd_wait,ui_during_sd_wait,max_core_gap,max_ff_depth,owner_errors);
}
static void controller_main(void *arg)
{
    (void)arg;
    model_result=pipeline_model_main(model_argc,model_argv);
    if(owner_errors||max_ff_depth>1||!task_runs[CORE_TASK]||!task_runs[UI_TASK]||!wait_switches)model_result=1;
    vTaskEndScheduler();abort();
}
int main(int argc,char **argv)
{
    for(int n=1;n+1<argc;n++)if(!strcmp(argv[n],"--model-wait-policy")) {
        REQUIRE(!strcmp(argv[n+1],"hybrid")||!strcmp(argv[n+1],"tick-only"));
        tick_only_policy=!strcmp(argv[n+1],"tick-only");
        memmove(&argv[n],&argv[n+2],(size_t)(argc-n-1)*sizeof argv[0]);argc-=2;break;
    }
    model_argc=argc;model_argv=argv;
    controller_handle=xTaskCreateStatic(controller_main,"model-controller",8192,0,6,controller_stack,&controller_tcb);
    for(unsigned n=0;n<TASKS;n++)workers[n]=xTaskCreateStatic(worker_main,n==0?"core":n==1?"ui":"storage",8192,(void *)(intptr_t)n,5-n,worker_stacks[n],&worker_tcbs[n]);
    configASSERT(controller_handle&&workers[0]&&workers[1]&&workers[2]);
    vTaskStartScheduler();
    return model_result;
}
