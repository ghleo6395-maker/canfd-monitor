#include "services/sys/led.h"
#include "services/can_core/chan_sm.h"
#include "services/core/app_timer.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"

static bool     rec_on, stat_on;
static uint32_t flash_until;       /* 快闪 3 次结束时刻 */
static const char *stat_pat = "灭", *rec_pat = "灭";

void led_init(void)
{
    rec_on = stat_on = false;
    flash_until = 0;
}

void led_handle(const app_evt_t *e)
{
    if (e->type != EV_NTF_MSG)
        return;
    switch ((msg_t)e->a) {
    case MSG_NO_SD: case MSG_SD_IN_MSC: case MSG_SD_ERROR: case MSG_SD_WP:
    case MSG_SD_UNSUPPORTED: case MSG_SD_FRAG: case MSG_SD_FULL_NOREC:
        flash_until = app_now_ms() + 600;      /* 100 ms 亮 / 100 ms 灭 × 3 */
        break;
    default:
        break;
    }
}

void led_tick(void)
{
    uint32_t now = app_now_ms();
    const power_t *p = power_get();
    if (p->state == PWR_OFF) {
        rec_on = stat_on = false;
        stat_pat = rec_pat = "灭";
        return;
    }

    const rec_t *r = rec_get();
    if ((int32_t)(flash_until - now) > 0) {
        rec_on = ((flash_until - now) / 100u) % 2u == 1u;
        rec_pat = "快闪";
    } else if (r->state == REC_RUNNING) {
        if (r->drops) {
            rec_on = (now % 1000u) < 500u;
            rec_pat = "1Hz闪(有丢帧)";
        } else {
            rec_on = true;
            rec_pat = "常亮";
        }
    } else {
        rec_on = false;
        rec_pat = "灭";
    }

    bool err = false;
    for (int ch = 0; ch < CH_NUM; ch++) {
        const chan_t *c = chan_get(ch);
        if (c->errst == ERRST_PASSIVE || c->errst == ERRST_BUSOFF || c->state == CH_BUSOFF)
            err = true;
    }
    uint32_t ph = now % 2000u;
    if (p->batt_mv < 3400 && !p->ext_power) {
        stat_on = ph < 1000u;
        stat_pat = "慢闪(低电)";
    } else if (err) {
        stat_on = ph < 100u || (ph >= 200u && ph < 300u);
        stat_pat = "双闪(总线错误)";
    } else {
        stat_on = ph < 100u;
        stat_pat = "短闪(正常)";
    }
}

bool led_rec_on(void) { return rec_on; }
bool led_stat_on(void) { return stat_on; }
const char *led_stat_pattern(void) { return stat_pat; }
const char *led_rec_pattern(void) { return rec_pat; }
