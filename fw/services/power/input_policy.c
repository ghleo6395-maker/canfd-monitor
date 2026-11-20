/* SPDX-License-Identifier: MIT */
#ifdef CANFD_REVE_TARGET
#include "services/power/input_policy.h"
input_mode_t input_policy_step(input_policy_t *p, const input_sample_t *s, uint32_t now)
{
    /* A conservative Type-C 1.5A/3A threshold accommodates the 100k sense
     * resistor/ADC error. Ambiguous or dual-active CC always falls back.
     * No USB PD or BC1.2 contract is inferred from VBUS/PGOOD. */
    uint16_t high=s->cc1_mv>s->cc2_mv?s->cc1_mv:s->cc2_mv;
    uint16_t low=s->cc1_mv<s->cc2_mv?s->cc1_mv:s->cc2_mv;
    bool capable=s->vbus && s->cc_valid && high>=800u && high<=2050u && low<200u;
    if(!capable || s->external) p->qualifying=false;
    if(!s->vbus && !s->external) return INPUT_NONE;
    if(s->external) return INPUT_EXTERNAL;
    if(capable) {
        if(!p->qualifying) { p->qualifying=true; p->since=now; }
        if((uint32_t)(now-p->since)>=150u) return INPUT_TYPEC;
    }
    if(s->suspended) return INPUT_SUSPEND;
    return s->configured?INPUT_USB_500:INPUT_USB_100;
}
input_mode_t input_policy_expire(input_mode_t mode, bool vbus, bool external,
                                 bool suspended, uint32_t age_ms)
{
    if(external) return INPUT_EXTERNAL;
    if(!vbus) return INPUT_NONE;
    if(mode==INPUT_EXTERNAL || mode==INPUT_NONE || age_ms>30u)
        return suspended?INPUT_SUSPEND:INPUT_USB_100;
    if(suspended && mode!=INPUT_TYPEC) return INPUT_SUSPEND;
    return mode;
}
#endif
