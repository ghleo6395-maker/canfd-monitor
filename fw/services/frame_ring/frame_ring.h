/* 广播式环形缓冲：只有接收中断（模拟器里是 env_sim）写入，多个读者各自读。
 * 读者分两种（框架设计第 7 节）：
 *  - 无损读者（录制、统计）：生产者根据它们的进度判断有没有空间，满了就丢新帧并计数；
 *  - 只取最新的读者（显示）：落后太多就跳到最新位置，不会挡住生产者。 */
#ifndef FRAME_RING_H
#define FRAME_RING_H

#include "services/core/app_types.h"

#define RING_LEN 512

typedef struct {
    uint32_t tail;
    bool     lossless;
} ring_reader_t;

void               ring_init(void);
bool               ring_push(const can_frame_t *f);
void               ring_reader_attach(ring_reader_t *r, bool lossless);
const can_frame_t *ring_peek(ring_reader_t *r);
void               ring_advance(ring_reader_t *r);
uint32_t           ring_dropped(void);
uint32_t           ring_level(void);   /* 最慢的无损读者落后多少帧 */

#endif
