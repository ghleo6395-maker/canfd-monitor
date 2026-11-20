/* SPDX-License-Identifier: MIT
 * Rev D single-core M7, static kernel objects, same 1 ms HAL time base. */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#include <stdint.h>
#include "stm32h743xx.h"

extern uint32_t SystemCoreClock;
void revd_rtos_assert_failed(const char *file, uint32_t line);

#define configUSE_PREEMPTION                     1
#define configUSE_TIME_SLICING                   1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1
#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                       1000u
#define configMAX_PRIORITIES                     7
#define configMINIMAL_STACK_SIZE                 256u
#define configMAX_TASK_NAME_LEN                  12
#define configTICK_TYPE_WIDTH_IN_BITS            TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                   1
#define configUSE_TICKLESS_IDLE                  0
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         0
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES               0
#define configUSE_COUNTING_SEMAPHORES             1
#define configQUEUE_REGISTRY_SIZE                0
#define configUSE_QUEUE_SETS                     0
#define configUSE_TASK_NOTIFICATIONS             1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    2 /* index0 runtime/DMA, index1 storage RPC */
#define configUSE_TIMERS                         0
#define configUSE_TRACE_FACILITY                 0
#define configGENERATE_RUN_TIME_STATS            0
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             0
/* Each task gets its own errno/newlib state; allocation remains forbidden in
 * kernel/runtime paths. syscalls.c heap locking covers any libc allocation. */
#define configUSE_NEWLIB_REENTRANT               1
#define configUSE_APPLICATION_TASK_TAG           0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  0
#define configPRIO_BITS                          __NVIC_PRIO_BITS
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY  15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY          (15u << (8u - __NVIC_PRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     (5u << (8u - __NVIC_PRIO_BITS))
#define configASSERT(x) do { if (!(x)) revd_rtos_assert_failed(__FILE__, __LINE__); } while (0)
#define configCHECK_HANDLER_INSTALLATION         1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_xTaskGetCurrentTaskHandle         1
#define INCLUDE_xTaskGetSchedulerState             1
#define INCLUDE_uxTaskGetStackHighWaterMark         1
#define INCLUDE_uxTaskGetStackHighWaterMark2        1
#define INCLUDE_eTaskGetState                     1
#define INCLUDE_vTaskDelete                       0

/* Direct vector aliases retain the vendor startup and its early PE2 hold. */
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
/* SysTick remains a thin product wrapper, forwarding only after scheduler starts. */
#endif
