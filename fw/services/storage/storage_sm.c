#include "services/storage/storage_sm.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/player/play_sm.h"
#include "services/recorder/rec_sm.h"

static storage_t s;

static const char *const st_name[ST_STATE_NUM] = { "未插卡", "挂载中", "就绪", "U 盘模式", "格式化中", "异常" };
const char *storage_state_name(storage_state_t st) { return st < ST_STATE_NUM ? st_name[st] : "?"; }
const storage_t *storage_get(void) { return &s; }

void storage_init(void)
{
    /* 上电后由平台重报 USB_ATTACH；完整模拟重置不能保留旧环境。 */
    s = (storage_t){ .state = ST_NO_CARD };
}

static void mount(void)
{
    s.state = ST_MOUNTING;
    port_sd_mount();
}

static bool busy_with_files(void)
{
    return rec_get()->state != REC_IDLE || play_any_active();
}

static msg_t unusable_reason(void)
{
    switch (s.state) {
    case ST_NO_CARD:  return MSG_NO_SD;
    case ST_MSC:      return MSG_SD_IN_MSC;
    case ST_ERROR:    return s.last_err == SD_ERR_WP ? MSG_SD_WP : MSG_SD_UNSUPPORTED;
    default:          return MSG_SD_ERROR;
    }
}

bool storage_writable(msg_t *why)
{
    if (s.state == ST_READY)
        return true;
    *why = unusable_reason();
    return false;
}

bool storage_readable(msg_t *why)
{
    return storage_writable(why);
}

static void leave_msc(void)
{
    port_usb_msc(false);
    evt_msg(MSG_MSC_OFF, 0, 0);
    if (port_sd_present())
        mount();
    else
        s.state = ST_NO_CARD;
}

void storage_handle(const app_evt_t *e)
{
    switch (e->type) {
    case EV_SYS_STARTED:
        if (port_sd_present())
            mount();
        break;
    case EV_SYS_STOP_ALL:
        if (s.state == ST_MSC)
            port_usb_msc(false);
        break;
    case EV_SD_INSERT:
        if (s.state == ST_NO_CARD)
            mount();
        break;
    case EV_SD_REMOVE:
        if (s.state == ST_MSC)
            port_usb_msc(false);
        if (s.state != ST_NO_CARD) {
            bool was_ready = s.state == ST_READY;
            s.state = ST_NO_CARD;
            evt_post4(EV_NTF_STORAGE_LOST, 0, SRC_SYS, 0, 0);
            if (was_ready && !busy_with_files())
                evt_msg(MSG_SD_REMOVED, 0, 0);
        }
        break;
    case EV_SD_MOUNT_DONE:
        if (s.state != ST_MOUNTING)
            break;
        if (e->a == SD_OK || e->a == SD_OK_REPAIRED) {
            s.state = ST_READY;
            if (e->a == SD_OK_REPAIRED)
                evt_msg(MSG_SD_REPAIRED, 0, 0);   /* 上次录制没关好，已截断到最后一个有效块 */
        } else {
            s.state = ST_ERROR;
            s.last_err = (sd_result_t)e->a;
            evt_msg(e->a == SD_ERR_WP ? MSG_SD_WP : MSG_SD_UNSUPPORTED, 0, 0);
        }
        break;
    case EV_USB_ATTACH:
        s.usb_attached = true;
        break;
    case EV_USB_DETACH:
        s.usb_attached = false;
        if (s.state == ST_MSC)
            leave_msc();
        break;
    case EV_USB_EJECT:
        if (s.state == ST_MSC)
            leave_msc();
        break;
    case EV_REQ_MSC:
        if (e->a == 0) {
            if (s.state == ST_MSC)
                leave_msc();
            break;
        }
        if (s.state == ST_MSC)
            break;
        if (!s.usb_attached) {
            evt_msg(MSG_MSC_NO_USB, 0, 0);
            break;
        }
        if (s.state != ST_READY) {
            evt_msg(unusable_reason(), 0, 0);
            break;
        }
        if (busy_with_files()) {
            evt_msg(MSG_MSC_DENY, 0, 0);          /* 录制 / 回放中不能交出 SD 卡（需求 3.4） */
            break;
        }
        if (e->b == 0) {
            evt_ask(ASK_MSC, 0, 0, (src_t)e->src);
            break;
        }
        port_sd_unmount();
        port_usb_msc(true);
        s.state = ST_MSC;
        evt_post4(EV_NTF_STORAGE_LOST, 0, SRC_SYS, 0, 0);
        evt_msg(MSG_MSC_ON, 0, 0);
        break;
    case EV_REQ_FORMAT:
        if (s.state != ST_READY && s.state != ST_ERROR) {
            evt_msg(unusable_reason(), 0, 0);
            break;
        }
        if (s.state == ST_ERROR && s.last_err == SD_ERR_WP) {
            evt_msg(MSG_SD_WP, 0, 0);             /* 写保护开关拨着，格式化也没用 */
            break;
        }
        if (busy_with_files()) {
            evt_msg(MSG_FORMAT_DENY, 0, 0);
            break;
        }
        if (e->b == 0) {
            evt_ask(ASK_FORMAT, 0, 0, (src_t)e->src);
            break;
        }
        s.state = ST_FORMATTING;
        port_sd_format();
        break;
    case EV_SD_FORMAT_DONE:
        if (s.state == ST_FORMATTING) {
            s.last_err = (sd_result_t)e->a;
            s.state = e->a == SD_OK ? ST_READY : ST_ERROR;
            evt_msg(e->a == SD_OK ? MSG_FORMAT_DONE : MSG_SD_ERROR, 0, e->a);
        }
        break;
    case EV_REQ_PROTECT:
        if (s.state == ST_READY)
            port_sd_protect_toggle(e->a);
        break;
    default:
        break;
    }
}
