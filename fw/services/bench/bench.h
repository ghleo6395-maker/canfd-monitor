/* SPDX-License-Identifier: MIT */
#ifndef BENCH_H
#define BENCH_H

#include "services/core/app_types.h"

typedef enum { BENCH_CONFIG, BENCH_ON, BENCH_OFF, BENCH_ROTATE } bench_action_t;
typedef struct {
    uint32_t segment_kib;       /* 64..65536; session-only override */
    uint32_t rotate_ms;         /* 0 or 250..3600000 */
    uint32_t refresh_ms;        /* 0 or 50..60000 */
} bench_config_t;
typedef struct {
    bool enabled;
    bench_config_t config;
    uint32_t diag_epoch;
    uint32_t rotations_requested, redraws_requested, request_rejected;
} bench_t;

/* Single application foreground owner. No ISR calls, no I/O, no traffic
 * generator, no recording auto-start and no confirmation bypass. */
void bench_init(void);
void bench_tick(void);
const bench_t *bench_get(void);
const char *bench_execute(bench_action_t action, const bench_config_t *config);

#endif
