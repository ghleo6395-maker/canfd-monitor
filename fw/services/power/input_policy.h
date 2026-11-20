/* SPDX-License-Identifier: MIT */
#ifndef CANFD_INPUT_POLICY_H
#define CANFD_INPUT_POLICY_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { INPUT_NONE, INPUT_USB_100, INPUT_USB_500, INPUT_TYPEC, INPUT_EXTERNAL, INPUT_SUSPEND } input_mode_t;
typedef struct { bool qualifying; uint32_t since; } input_policy_t;
typedef struct {
    bool vbus, external, configured, suspended, cc_valid;
    uint16_t cc1_mv, cc2_mv;
} input_sample_t;
input_mode_t input_policy_step(input_policy_t *p, const input_sample_t *s, uint32_t now);
/* Called even if foreground I/O stalls. Only USB modes need freshness. */
input_mode_t input_policy_expire(input_mode_t mode, bool vbus, bool external,
                                 bool suspended, uint32_t age_ms);
#endif
