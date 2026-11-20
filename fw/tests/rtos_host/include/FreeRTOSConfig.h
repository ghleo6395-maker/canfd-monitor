/* SPDX-License-Identifier: MIT */
#ifndef REVD_HOST_FREERTOS_CONFIG_H
#define REVD_HOST_FREERTOS_CONFIG_H
#include <stdlib.h>
#include <stdint.h>
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configCPU_CLOCK_HZ 1000000000u
#define configTICK_RATE_HZ 1000u
#define configMAX_PRIORITIES 8
#define configMINIMAL_STACK_SIZE 8192u
#define configMAX_TASK_NAME_LEN 24
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configUSE_TASK_NOTIFICATIONS 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 2
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 1
#define configQUEUE_REGISTRY_SIZE 0
#define configUSE_QUEUE_SETS 0
#define configUSE_TIMERS 0
#define configUSE_TRACE_FACILITY 1
#define configGENERATE_RUN_TIME_STATS 0
#define configCHECK_FOR_STACK_OVERFLOW 0
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_NEWLIB_REENTRANT 0
#define configUSE_APPLICATION_TASK_TAG 0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelayUntil 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_uxTaskGetStackHighWaterMark2 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_vTaskEndScheduler 1
#define configASSERT(condition) do { if (!(condition)) abort(); } while (0)
#endif
