/* 总线统计：帧数、帧率、负载率、错误、丢帧（需求 F6.3、F6.4）。环形缓冲的无损读者。 */
#ifndef STATS_H
#define STATS_H

#include "services/core/app_types.h"

typedef struct {
    uint32_t rx_total, tx_total, err_total, fd_total, ext_total;
    uint32_t hw_drops;        /* Hardware FIFO lost latch events: lower bound. */
    uint32_t ingress_drops, ring_drops, tx_echo_drops; /* Exact software frame losses. */
    uint32_t fps;             /* 最近 1 s */
    uint16_t load_permille;   /* 最近 1 s，千分比 */
    uint16_t load_peak;
    uint16_t err_rate;        /* 最近 1 s 错误次数 */
    uint16_t valid_last_s;    /* 最近 1 s 有效帧（用于"错误多、有效帧为 0"提示） */
    uint16_t load_hist[60];   /* 最近 60 s 负载曲线 */
    uint8_t  hist_pos;
} ch_stats_t;

void              stats_init(void);
void              stats_tick(void);
void              stats_handle(const app_evt_t *e);
const ch_stats_t *stats_get(int ch);
void              stats_clear(int ch);
uint32_t          stats_ring_drops(void);

#endif
