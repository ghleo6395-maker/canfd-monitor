#include "services/frame_ring/frame_ring.h"

#define MAX_LOSSLESS 4

#if defined(CANFD_REVD_TARGET) && (defined(__arm__) || defined(__thumb__))
#define FRAME_RING_RAM __attribute__((section(".revd_d2_bss"),aligned(32)))
#else
#define FRAME_RING_RAM
#endif
static can_frame_t    ring[RING_LEN] FRAME_RING_RAM;
static uint32_t       head, dropped;
static ring_reader_t *lossless[MAX_LOSSLESS];
static int            n_lossless;

void ring_init(void)
{
    head = dropped = 0;
    n_lossless = 0;
}

uint32_t ring_level(void)
{
    uint32_t worst = 0;
    for (int i = 0; i < n_lossless; i++) {
        uint32_t lag = head - lossless[i]->tail;
        if (lag > worst)
            worst = lag;
    }
    return worst;
}

bool ring_push(const can_frame_t *f)
{
    if (ring_level() >= RING_LEN) {
        dropped++;
        return false;
    }
    ring[head % RING_LEN] = *f;
    head++;
    return true;
}

void ring_reader_attach(ring_reader_t *r, bool lossless_)
{
    r->tail = head;
    r->lossless = lossless_;
    if (lossless_ && n_lossless < MAX_LOSSLESS)
        lossless[n_lossless++] = r;
}

const can_frame_t *ring_peek(ring_reader_t *r)
{
    if (!r->lossless && head - r->tail > RING_LEN)
        r->tail = head - RING_LEN;           /* 只取最新：落后太多就跳过 */
    if (r->tail == head)
        return NULL;
    return &ring[r->tail % RING_LEN];
}

void ring_advance(ring_reader_t *r)
{
    if (r->tail != head)
        r->tail++;
}

uint32_t ring_dropped(void)
{
    return dropped;
}
