#include "services/input/keys.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/board_profile.h"

#define REPEAT_DELAY_MS  500
#define REPEAT_PERIOD_MS 150
#define COMBO_LOCK_MS    2000

typedef struct {
    bool     down;
    bool     consumed;   /* 这一下用于唤醒，不产生手势 */
    bool     multi;      /* 按住期间有别的键也按下了 */
    bool     long_fired;
    uint32_t t_down;
    uint32_t t_next_rep;
} kstate_t;

static kstate_t ks[KEY_NUM];
static bool     power_armed;
static bool     combo_fired;

static const uint16_t long_ms[KEY_NUM] = {
    [KEY_OK] = 1000, [KEY_BACK] = 1000, [KEY_REC] = 2000, [KEY_POWER] = 2000,
};

static bool is_arrow(key_id_t k)
{
    return k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT;
}

static void gesture(key_id_t k, gesture_t g)
{
    if (k == KEY_POWER && !power_armed)
        return;
    evt_post4(EV_KEY, (uint8_t)k, SRC_UI, g, 0);
}

void keys_reset(void)
{
    for (int i = 0; i < KEY_NUM; i++)
        ks[i] = (kstate_t){ 0 };
    power_armed = false;
    combo_fired = false;
}

void keys_cancel(key_id_t key)
{
    if ((unsigned)key >= KEY_NUM) return;
    ks[key] = (kstate_t){0};
    if (key == CANFD_LOCK_KEY_A || key == CANFD_LOCK_KEY_B) combo_fired = false;
}

void keys_raw(key_id_t k, bool down, bool consume)
{
    kstate_t *s = &ks[k];
    uint32_t now = app_now_ms();

    if (down) {
        if (s->down)
            return;
        bool other = false;
        for (int i = 0; i < KEY_NUM; i++) {
            if (i != (int)k && ks[i].down) {
                ks[i].multi = true;
                other = true;
            }
        }
        *s = (kstate_t){ .down = true, .consumed = consume, .multi = other, .t_down = now,
                         .t_next_rep = now + REPEAT_DELAY_MS };
        return;
    }

    if (k == KEY_POWER && !power_armed) {
        power_armed = true;          /* 开机时按着的那一下松开了（按下发生在复位之前） */
        *s = (kstate_t){ 0 };
        return;
    }
    if (!s->down)
        return;                      /* 按下发生在开机之前的键（开机组合键），松开不算 */
    if (!s->consumed && !s->multi && !s->long_fired)
        gesture(k, GEST_SHORT);
    *s = (kstate_t){ 0 };
    if (k == CANFD_LOCK_KEY_A || k == CANFD_LOCK_KEY_B)
        combo_fired = false;
}

void keys_tick(void)
{
    uint32_t now = app_now_ms();
    int n_down = 0;
    for (int i = 0; i < KEY_NUM; i++)
        n_down += ks[i].down;

    for (int i = 0; i < KEY_NUM; i++) {
        kstate_t *s = &ks[i];
        if (!s->down || s->consumed || s->multi)
            continue;
        if (long_ms[i] && !s->long_fired && now - s->t_down >= long_ms[i]) {
            s->long_fired = true;
            gesture((key_id_t)i, GEST_LONG);
        }
        if (is_arrow((key_id_t)i) && (int32_t)(now - s->t_next_rep) >= 0) {
            s->t_next_rep = now + REPEAT_PERIOD_MS;
            s->long_fired = true;    /* 连发过就不再算短按 */
            gesture((key_id_t)i, GEST_REPEAT);
        }
    }

    /* Rev D: LEFT+RIGHT. Rev E: BACK+REC. A chord suppresses both
     * individual gestures; a wake-only press cannot lock the device. */
    kstate_t *l = &ks[CANFD_LOCK_KEY_A], *r = &ks[CANFD_LOCK_KEY_B];
    if (l->down && r->down && n_down == 2 && !l->consumed && !r->consumed) {
#ifdef CANFD_REVE_TARGET
        /* Elapsed unsigned durations remain valid across the tick rollover.
         * Each key must have been held for the full chord interval. */
        bool held = now - l->t_down >= COMBO_LOCK_MS &&
                    now - r->t_down >= COMBO_LOCK_MS;
        if (!combo_fired && held) {
#else
        uint32_t t0 = l->t_down > r->t_down ? l->t_down : r->t_down;   /* 后按下的那个键 */
        if (!combo_fired && now - t0 >= COMBO_LOCK_MS) {
#endif
            combo_fired = true;
            evt_post4(EV_COMBO, 0, SRC_UI, COMBO_LOCK, 0);
        }
    }
}

bool keys_is_down(key_id_t k)
{
    return ks[k].down;
}

bool keys_power_armed(void)
{
    return power_armed;
}
