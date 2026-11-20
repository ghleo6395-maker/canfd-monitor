#include "services/power/power_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/player/play_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/tx_sched/tx_sm.h"

#define BOOT_HOLD_MS      1000u
#define BOOT_COMBO_MS     2000u   /* 加上前面的 1 s，共 3 s */
#define START_SETTLE_MS   50u
#define SHUT_POLL_MS      50u
#define SHUT_SERVICE_MAX  3000u   /* 等录制关文件最多 3 s */
#define BL_OFF_HOLD_MS    2000u   /* bootloader 里长按 POWER 关机 */
#define AUTO_OFF_CD_S     30u
#define LOW_WARN_MV       3400u
#define LOW_CLEAR_MV      3500u
#define LOW_SHUT_MV       3300u

static power_t p;
static uint32_t shutdown_started_ms;
#ifdef CANFD_REVE_TARGET
static uint8_t low_samples;
#endif

static const char *const st_name[PWR_STATE_NUM] = { "关机", "开机确认", "开机组合键", "启动中", "运行", "关机中", "升级模式" };
const char *power_state_name(pwr_state_t s) { return s < PWR_STATE_NUM ? st_name[s] : "?"; }
const power_t *power_get(void) { return &p; }
bool power_running(void) { return p.state == PWR_RUNNING; }

void power_init(void)
{
    p = (power_t){ .state = PWR_OFF, .batt_mv = 3900 };
#ifdef CANFD_REVE_TARGET
    low_samples=0;
#endif
}

static void restart_idle_timers(void)
{
    const cfg_t *c = cfg_get();
    if (c->screen_off_s && p.screen_on)
        tmr_start(TMR_SCREEN_OFF, c->screen_off_s * 1000u);
    else
        tmr_stop(TMR_SCREEN_OFF);
    if (c->auto_off_min)
        tmr_start(TMR_AUTO_OFF, c->auto_off_min * 60000u);
    else
        tmr_stop(TMR_AUTO_OFF);
}

static void screen(bool on)
{
    p.screen_on = on;
    port_backlight(on ? cfg_get()->backlight_pct : 0);
    if (on)
        restart_idle_timers();
    else
        tmr_stop(TMR_SCREEN_OFF);
}

void power_wake(void)
{
    if (p.state == PWR_RUNNING && !p.screen_on)
        screen(true);
}

void power_activity(void)
{
    if (p.state != PWR_RUNNING)
        return;
    if (p.countdown_s) {
        p.countdown_s = 0;              /* 任意键取消自动关机 */
        tmr_stop(TMR_AUTO_OFF_CD);
    }
    restart_idle_timers();
}

/* 自动关机豁免（F8.2）：录制、周期发送、回放在跑，接着外部电源，或连着上位机 */
static bool auto_off_exempt(void)
{
    return rec_get()->state != REC_IDLE || tx_any_active() || play_any_active() || p.ext_power || p.host_connected;
}

static void begin_shutdown(shutdown_reason_t why)
{
    if (p.state != PWR_RUNNING && p.state != PWR_STARTING)
        return;
    p.state = PWR_SHUTDOWN;
    p.reason = why;
    p.countdown_s = 0;
    p.shstep = SH_WAIT_SERVICES;
    p.shwait_ms = 0;
    shutdown_started_ms = app_now_ms();
    tmr_stop(TMR_SCREEN_OFF);
    tmr_stop(TMR_AUTO_OFF);
    tmr_stop(TMR_AUTO_OFF_CD);
    evt_post4(EV_SYS_STOP_ALL, 0, SRC_SYS, why, 0);
    tmr_start(TMR_SHUTDOWN, SHUT_POLL_MS);
}

static void cut_power(void)
{
    port_backlight(0);
    port_5v(false);
    port_pwr_hold(false);
    p.state = PWR_OFF;
    p.screen_on = false;
    p.locked = false;
    tmr_stop(TMR_BOOT);
    tmr_stop(TMR_BOOT_COMBO);
}

static void enter_starting(bool eng)
{
    p.state = PWR_STARTING;
    p.eng_boot = eng;
    port_chg_hi(p.ext_power);           /* 先定充电电流档，再开 5V（框架 8.7 第 4、5 步） */
    port_5v(true);
    tmr_start(TMR_START, START_SETTLE_MS);
}

bool power_raw_key(key_id_t k, bool down)
{
    switch (p.state) {
    case PWR_OFF:
        if (k == KEY_POWER && down) {
            port_pwr_hold(true);        /* Reset_Handler 最前面就拉高 PE2 */
            p.state = PWR_BOOT_HOLD;
            p.boot_count++;
            tmr_start(TMR_BOOT, BOOT_HOLD_MS);
        }
        return true;
    case PWR_BOOT_HOLD:
        if (k == KEY_POWER && !down) {
            bool combo = port_key_down(KEY_OK) || port_key_down(KEY_BACK) || port_key_down(KEY_REC);
            if (!combo) {
                p.reason = SHUT_MISFIRE;   /* 按住不满 1 s：误触，直接断电 */
                cut_power();
            }
        }
        return true;
    case PWR_BOOT_COMBO:
        if ((k == KEY_BACK || k == KEY_REC) && !down) {
            tmr_stop(TMR_BOOT_COMBO);
            enter_starting(false);
        }
        return true;
    case PWR_STARTING:
        return true;
    case PWR_BOOTLOADER:
        if (k == KEY_POWER) {
            if (down)
                tmr_start(TMR_BOOT, BL_OFF_HOLD_MS);
            else
                tmr_stop(TMR_BOOT);
        }
        return true;
    case PWR_SHUTDOWN:
        return true;                    /* 关机流程里只关心 POWER 什么时候松开（读电平） */
    case PWR_RUNNING:
    default:
        return false;
    }
}

static void on_timer(int id)
{
    switch (id) {
    case TMR_BOOT:
        if (p.state == PWR_BOOT_HOLD) {
#ifdef CANFD_REVE_TARGET
            /* BACK at power-on opens maintenance without the deleted OK
             * switch. Hardware BOOT0/SWD handles recovery; do not enter the
             * legacy placeholder bootloader that never actually loads ROM. */
            enter_starting(port_key_down(KEY_BACK));
#else
            if (port_key_down(KEY_BACK) && port_key_down(KEY_REC)) {
                p.state = PWR_BOOT_COMBO;
                tmr_start(TMR_BOOT_COMBO, BOOT_COMBO_MS);
            } else {
                enter_starting(port_key_down(KEY_OK) && port_key_down(KEY_BACK));
            }
#endif
        } else if (p.state == PWR_BOOTLOADER) {
            p.reason = SHUT_USER;
            cut_power();
        }
        break;
    case TMR_BOOT_COMBO:
        if (p.state == PWR_BOOT_COMBO) {
            p.state = PWR_BOOTLOADER;   /* 升级模式：不开 5V，只等 SD / USB 升级 */
            port_5v(false);
        }
        break;
    case TMR_START:
        if (p.state != PWR_STARTING)
            break;
        p.state = PWR_RUNNING;
        p.no_screen = !port_screen_present();
        p.low_warned = false;
        screen(true);
        evt_post4(EV_SYS_STARTED, 0, SRC_SYS, p.eng_boot, 0);
        if (port_chip_rev() == 'Y')
            evt_msg(MSG_CHIP_REV_Y, 0, 0);
        break;
    case TMR_SCREEN_OFF:
        if (p.state == PWR_RUNNING && p.screen_on)
            screen(false);
        break;
    case TMR_AUTO_OFF:
        if (p.state != PWR_RUNNING)
            break;
        if (auto_off_exempt()) {
            restart_idle_timers();
            break;
        }
        p.countdown_s = AUTO_OFF_CD_S;
        if (!p.screen_on)
            screen(true);               /* 倒计时要让人看得见 */
        tmr_start(TMR_AUTO_OFF_CD, 1000);
        break;
    case TMR_AUTO_OFF_CD:
        if (p.state != PWR_RUNNING || !p.countdown_s)
            break;
        if (auto_off_exempt()) {        /* 倒计时期间开始了录制等 */
            p.countdown_s = 0;
            restart_idle_timers();
            break;
        }
        if (--p.countdown_s == 0)
            begin_shutdown(SHUT_AUTO_OFF);
        else
            tmr_start(TMR_AUTO_OFF_CD, 1000);
        break;
    case TMR_SHUTDOWN:
        if (p.state != PWR_SHUTDOWN)
            break;
        /* SD work may delay a 50 ms timer by seconds. Count actual elapsed
         * time, including uint32_t clock wrap, instead of poll deliveries. */
        p.shwait_ms = app_now_ms() - shutdown_started_ms;
        if (p.shstep == SH_WAIT_SERVICES) {
            if (rec_get()->state != REC_IDLE && p.shwait_ms < SHUT_SERVICE_MAX) {
                tmr_start(TMR_SHUTDOWN, SHUT_POLL_MS);   /* 等录制把文件关好 */
                break;
            }
            for (int ch = 0; ch < CH_NUM; ch++)
                port_can_standby(ch);
            port_backlight(0);          /* 先停屏幕和触摸，再关 5V */
            port_5v(false);
            p.screen_on = false;
            p.shstep = SH_WAIT_KEY;
        }
        if (port_key_down(KEY_POWER)) {
            tmr_start(TMR_SHUTDOWN, SHUT_POLL_MS);       /* 按着 POWER 时硬件一直拉着 PWR_GATE */
            break;
        }
        cut_power();
        break;
    default:
        break;
    }
}

static void on_batt(int32_t mv, bool ext)
{
    p.batt_mv = (uint16_t)mv;
    if (ext != p.ext_power) {
        p.ext_power = ext;
        if (p.state == PWR_RUNNING)
            port_chg_hi(ext);
    }
    if (p.state != PWR_RUNNING)
        return;
    if (mv >= (int32_t)LOW_CLEAR_MV)
        p.low_warned = false;
#ifdef CANFD_REVE_TARGET
    /* Weak external power may still drain the battery. Three consecutive
     * 1Hz low samples tolerate brief droop; <3.0V requests an immediate stop. */
    if(mv < (int32_t)LOW_SHUT_MV) { if(low_samples<3u) ++low_samples; }
    else low_samples=0;
    if (low_samples>=3u || mv<3000) {
#else
    if (!ext && mv < (int32_t)LOW_SHUT_MV) {
#endif
        begin_shutdown(SHUT_LOW_BATT);  /* 先停录制存文件，再断电（F9.2） */
        return;
    }
#ifdef CANFD_REVE_TARGET
    if (mv < (int32_t)LOW_WARN_MV && !p.low_warned) {
#else
    if (!ext && mv < (int32_t)LOW_WARN_MV && !p.low_warned) {
#endif
        p.low_warned = true;
        evt_msg(MSG_LOW_BATT, 0, mv);
    }
}

void power_handle(const app_evt_t *e)
{
    switch (e->type) {
    case EV_TIMER:
        on_timer(e->a);
        break;
    case EV_BATT:
        on_batt(e->a, e->b != 0);
        break;
    case EV_HOST_LINK:
        p.host_connected = e->a != 0;
        if (p.host_connected)
            power_activity();
        break;
    case EV_USB_DETACH:
        p.host_connected = false;
        break;
    case EV_KEY:
        if (p.state != PWR_RUNNING || e->ch != KEY_POWER)
            break;
        if (e->a == GEST_SHORT && p.screen_on)
            screen(false);
        else if (e->a == GEST_LONG)
            begin_shutdown(SHUT_USER);
        break;
    case EV_COMBO:
        if (p.state == PWR_RUNNING && e->a == COMBO_LOCK) {
            p.locked = !p.locked;
            evt_msg(p.locked ? MSG_LOCKED : MSG_UNLOCKED, 0, 0);
        }
        break;
    case EV_REQ_SHUTDOWN:
        begin_shutdown(SHUT_HOST);
        break;
    default:
        break;
    }
}
