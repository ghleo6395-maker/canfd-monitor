/* SPDX-License-Identifier: MIT */
#include "FreeRTOS.h"
#include "task.h"
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, configSTACK_DEPTH_TYPE *depth)
{
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
    *tcb=&idle_tcb; *stack=idle_stack; *depth=configMINIMAL_STACK_SIZE;
}
