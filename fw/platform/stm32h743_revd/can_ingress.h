/* SPDX-License-Identifier: MIT */
#ifndef REVD_CAN_INGRESS_H
#define REVD_CAN_INGRESS_H

#include "services/core/app_types.h"
#include <stdatomic.h>

/* One IRQ producer and one foreground consumer, per FDCAN channel. Indices
 * grow monotonically; no critical section or interrupt masking is required.
 * The frame's ts_us contains TIM2's low 32 bits until foreground dispatch. */
#define CAN_INGRESS_LEN 512u
typedef struct {
    can_frame_t frames[CAN_INGRESS_LEN];
    _Atomic uint32_t head, tail, dropped, high_water;
} can_ingress_t;

void can_ingress_init(can_ingress_t *q);
bool can_ingress_push(can_ingress_t *q, const can_frame_t *frame);
bool can_ingress_peek(can_ingress_t *q, can_frame_t *frame);
void can_ingress_advance(can_ingress_t *q);
uint32_t can_ingress_level(const can_ingress_t *q);
uint32_t can_ingress_dropped(const can_ingress_t *q);
uint32_t can_ingress_high_water(const can_ingress_t *q);

/* EV_CAN_DROPS.b distinguishes real hardware-lost notifications (a lower bound,
 * because RF0L is a latched bit) from exact software frame-drop counts. */
enum { CAN_DROP_HW_LOST_EVENT = 1, CAN_DROP_INGRESS = 2, CAN_DROP_RING = 3, CAN_DROP_TX_ECHO = 4 };
typedef struct {
    uint32_t hw_lost_events, ingress_drops, ring_drops, irq_read_errors, tx_echo_drops;
    uint32_t queued, high_water;
    /* Foreground board_can_task wall time, including any ISR preemption.
     * Gap is entry-to-entry while this channel is continuously active.
     * Both channels see the same shared dispatcher duration when active. */
    uint32_t max_service_gap_us, max_service_us;
} board_can_metrics_t;
bool board_can_metrics(unsigned ch, board_can_metrics_t *out);

#endif
