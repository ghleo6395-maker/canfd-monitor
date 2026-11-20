/* SPDX-License-Identifier: MIT
 * Runs the pinned upstream FreeRTOS POSIX port, not a replacement scheduler. */
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
static StaticTask_t tcbs[2];
static StackType_t stacks[2][8192];
static TaskHandle_t controller, worker;
static volatile unsigned requests, completions, worker_waits;
static void worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        worker_waits++;
        vTaskDelay(1);
        completions++;
        xTaskNotifyGive(controller);
    }
}
static void controller_main(void *arg)
{
    (void)arg;
    for (unsigned n=0;n<20;n++) {
        requests++;
        xTaskNotifyGive(worker);
        configASSERT(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)));
        configASSERT(requests==completions);
    }
    vTaskEndScheduler();
    abort();
}
int main(void)
{
    controller=xTaskCreateStatic(controller_main,"controller",8192,0,3,stacks[0],&tcbs[0]);
    worker=xTaskCreateStatic(worker_main,"worker",8192,0,2,stacks[1],&tcbs[1]);
    configASSERT(controller&&worker);
    vTaskStartScheduler();
    printf("{\"kernel\":\"FreeRTOS V11.3.2\",\"port\":\"upstream GCC/POSIX\",\"physical_hardware_tested\":false,\"requests\":%u,\"completed\":%u,\"notification_waits\":%u,\"pass\":%s}\n",requests,completions,worker_waits,requests==20&&completions==20&&worker_waits==20?"true":"false");
    return requests==20&&completions==20&&worker_waits==20?0:1;
}
