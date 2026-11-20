/* SPDX-License-Identifier: MIT */
#ifndef REVD_RTOS_RUNTIME_H
#define REVD_RTOS_RUNTIME_H
#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

typedef enum {
    REVD_RTOS_CORE, REVD_RTOS_STORAGE, REVD_RTOS_USB, REVD_RTOS_UI,
    REVD_RTOS_TASK_COUNT
} revd_rtos_task_id_t;
typedef struct {
    uint32_t runs;
    uint32_t max_step_us;          /* Wall time, includes blocking/preemption. */
    uint32_t stack_min_free_bytes; /* FreeRTOS stack high-water value * sizeof(StackType_t). */
    uint32_t stack_size_bytes;
} revd_rtos_task_metrics_t;

/* Called once after hardware clocks/GPIO, before any application work. */
void revd_rtos_start(void);
bool revd_rtos_running(void);
void revd_rtos_wait_io(void);       /* Current task blocks <=1 tick; never pumps another owner. */
uint32_t revd_rtos_time_us(void);   /* 1 MHz TIM2 low word; unsigned subtraction handles wrap. */
void revd_rtos_poll_command_wait(void); /* Fast LL response polls yield; bounded 200 us window then block. */
void revd_rtos_notify_core(void);
void revd_rtos_notify_storage(void);
void revd_rtos_notify_usb(void);
void revd_rtos_notify_ui(void);
/* CAN priority 4 uses only deferred NVIC pend; TIM7 priority 6 invokes the kernel. */
void revd_rtos_notify_core_from_isr_deferred(void);
void revd_rtos_notify_storage_from_isr(void);
void revd_rtos_notify_usb_from_isr(void);
void revd_rtos_notify_ui_from_isr(void);
TaskHandle_t revd_rtos_task_handle(revd_rtos_task_id_t id);
bool revd_rtos_task_metrics(revd_rtos_task_id_t id, revd_rtos_task_metrics_t *out);
void revd_rtos_assert_failed(const char *file, uint32_t line);

/* Product bridges: definitions must be strong in the final image. */
void revd_rtos_bridges_init(void);
void revd_core_boot(void);
void revd_core_step(void);
void revd_ui_step(void);
void revd_usb_transport_step(void);
#endif
