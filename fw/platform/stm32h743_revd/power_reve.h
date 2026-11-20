/* SPDX-License-Identifier: MIT */
#ifndef CANFD_POWER_REVE_H
#define CANFD_POWER_REVE_H
#ifdef CANFD_REVE_TARGET
#include <stdbool.h>
#include <stdint.h>
#include "services/power/input_policy.h"
typedef struct {
    input_mode_t mode;
    uint16_t cc1_mv, cc2_mv, cell_mv;
    int32_t battery_ma; /* positive = charging, negative = discharging */
    bool monitor_valid, cc_valid;
    uint32_t stale_fallbacks, monitor_errors;
} reve_power_diag_t;
void reve_power_init(void);
void reve_power_task(void);
void reve_power_tick(void);
void reve_power_usb_configured(bool configured);
void reve_power_usb_suspend(bool suspended);
void reve_power_usb_reset(void);
bool reve_power_vbus(void);
const reve_power_diag_t *reve_power_diagnostics(void);
bool board_reve_adc_mv(uint32_t channel, uint16_t *mv);
#endif
#endif
