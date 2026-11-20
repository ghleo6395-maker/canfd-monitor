/* SPDX-License-Identifier: MIT */
#include "can_ingress.h"

_Static_assert((CAN_INGRESS_LEN & (CAN_INGRESS_LEN - 1u)) == 0u, "power of two queue");
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "IRQ atomics must be lock-free");

void can_ingress_init(can_ingress_t *q)
{
    /* Caller must have stopped the producer and consumer before resetting. */
    atomic_init(&q->head, 0); atomic_init(&q->tail, 0);
    atomic_init(&q->dropped, 0); atomic_init(&q->high_water, 0);
}

bool can_ingress_push(can_ingress_t *q, const can_frame_t *frame)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    uint32_t level = head - tail;
    if (level >= CAN_INGRESS_LEN) {
        atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed);
        return false;
    }
    q->frames[head & (CAN_INGRESS_LEN - 1u)] = *frame;
    /* Release publishes the entire 80-byte frame after the copy, and acquire
     * on tail ensures the consumer has finished copying a reused slot. */
    atomic_store_explicit(&q->head, head + 1u, memory_order_release);
    if (++level > atomic_load_explicit(&q->high_water, memory_order_relaxed))
        atomic_store_explicit(&q->high_water, level, memory_order_relaxed);
    return true;
}

bool can_ingress_peek(can_ingress_t *q, can_frame_t *frame)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (head == tail) return false;
    *frame = q->frames[tail & (CAN_INGRESS_LEN - 1u)];
    return true;
}

void can_ingress_advance(can_ingress_t *q)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    atomic_store_explicit(&q->tail, tail + 1u, memory_order_release);
}

uint32_t can_ingress_level(const can_ingress_t *q)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return atomic_load_explicit(&q->head, memory_order_acquire) - tail;
}
uint32_t can_ingress_dropped(const can_ingress_t *q)
{
    return atomic_load_explicit(&q->dropped, memory_order_relaxed);
}
uint32_t can_ingress_high_water(const can_ingress_t *q)
{
    return atomic_load_explicit(&q->high_water, memory_order_relaxed);
}
