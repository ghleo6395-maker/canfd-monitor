/* 全局类型：事件、按键、通道模式、提示码、确认码。纯 C，不依赖 HAL。 */
#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CH_NUM 2

typedef enum {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_OK, KEY_BACK, KEY_REC, KEY_POWER, KEY_NUM
} key_id_t;

typedef enum { GEST_SHORT, GEST_LONG, GEST_REPEAT } gesture_t;
typedef enum { COMBO_LOCK = 1 } combo_t;

/* 请求来源：屏幕、上位机、命令行、系统内部 */
typedef enum { SRC_SYS, SRC_UI, SRC_HOST, SRC_CLI } src_t;

typedef enum {
    CAN_MODE_LISTEN,      /* 只听：不回 ACK，不发任何位 */
    CAN_MODE_RESTRICTED,  /* 受限：只回 ACK，不发数据帧 */
    CAN_MODE_NORMAL,      /* 正常：可以发送 */
} can_mode_t;

/* 帧标志 */
#define CANF_EXT 0x01u
#define CANF_FD  0x02u
#define CANF_BRS 0x04u
#define CANF_ESI 0x08u
#define CANF_RTR 0x10u
#define CANF_TX  0x20u   /* 本机发送的回显 */
#define CANF_ERR 0x40u   /* 错误事件记录，不是真实报文 */

typedef struct {
    uint64_t ts_us;
    uint32_t id;
    uint8_t  ch;
    uint8_t  flags;
    uint8_t  len;        /* 数据字节数；RTR 帧存请求的 DLC */
    uint8_t  data[64];
} can_frame_t;

/* ---- 事件 ---- */
typedef enum {
    EV_NONE = 0,
    /* 原始输入（驱动 / 模拟器） */
    EV_KEY_RAW,          /* ch=key, a=1 按下 / 0 松开 */
    EV_TOUCH_ITEM,       /* a=条目序号 */
    EV_TOUCH_BACK,       /* 点标题栏的返回 */
    EV_TOUCH_DLG,        /* a=0 确认 / 1 取消；菜单对话框里 a=选项序号 */
    /* 按键手势（keys 模块生成） */
    EV_KEY,              /* ch=key, a=gesture_t */
    EV_COMBO,            /* a=combo_t */
    /* 定时器 */
    EV_TIMER,            /* a=timer_id_t */
    /* 系统生命周期 */
    EV_SYS_STARTED,      /* 开机完成；a=1 进工程菜单 */
    EV_SYS_STOP_ALL,     /* 关机前：停止录制、发送、回放 */
    /* 平台 → 核心（硬件事件） */
    EV_BATT,             /* a=电池 mV, b=外部供电 */
    EV_SD_INSERT,
    EV_SD_REMOVE,
    EV_SD_MOUNT_DONE,    /* a=sd_result_t */
    EV_SD_PREALLOC_DONE, /* a=sd_result_t */
    EV_SD_CLOSE_DONE,
    EV_SD_WRITE_ERR,     /* a=1 I/O failure; a=2 irreversible SD queue overflow */
    EV_SD_FULL,
    EV_SD_DELETE_DONE,   /* a=1 删掉了 / 0 没有可删的 */
    EV_SD_FORMAT_DONE,
    EV_USB_ATTACH,
    EV_USB_DETACH,
    EV_USB_EJECT,        /* 电脑弹出了 U 盘 */
    EV_HOST_LINK,        /* a=1 上位机连上 / 0 断开 */
    EV_CAN_STATUS,       /* ch, a=TEC, b=REC | (busoff << 16) */
    EV_CAN_TX_DONE,      /* ch */
    EV_CAN_PROBE_RESULT, /* ch, a=有效帧数, b=错误数 | (见到 FD 帧 << 16) */
    EV_CAN_DROPS,        /* ch, a=count; b=0/1 hardware lost latch (lower bound), 2 IRQ queue frames, 3 ring frames, 4 TX echo frames */
    /* 请求（屏幕 / 上位机 / 命令行 → 服务），src 标明来源 */
    EV_REQ_REC_START,
    EV_REQ_REC_STOP,     /* b=已确认 */
    EV_REQ_REC_MARK,
    EV_REQ_CH_MODE,      /* ch, a=can_mode_t, b=0 未确认 / 1 已确认 / 2 发送流程里已确认 */
    EV_REQ_CH_RATE,      /* ch, a=标称速率, b=数据段速率 */
    EV_REQ_CH_FD,        /* ch, a=开/关 */
    EV_REQ_CH_RECOVER,   /* ch */
    EV_REQ_AUTOBAUD,     /* ch, a=can_mode_t, b=已确认 */
    EV_REQ_TX_SEND,      /* ch, a=条目, b=已确认首次发送 */
    EV_REQ_TX_CYC,       /* ch, a=条目, b: bit0=开/关 bit1=已确认 */
    EV_REQ_TX_STOP_ALL,
    EV_REQ_PLAY_OPEN,    /* ch=目标通道, a=文件序号 */
    EV_REQ_PLAY_ANSWER,  /* ch, a=ask_t, b=1 是 / 0 否 */
    EV_REQ_PLAY_CTRL,    /* ch, a=play_ctl_t */
    EV_REQ_MSC,          /* a=1 进入 U 盘模式 / 0 收回, b=已确认 */
    EV_REQ_FORMAT,       /* b=已确认 */
    EV_REQ_PROTECT,      /* a=文件序号 */
    EV_REQ_SHUTDOWN,
    /* 服务之间的通知 */
    EV_NTF_CH_CFG_CHANGED, /* ch：速率或模式变了（录制要切段，周期发送要检查） */
    EV_NTF_STORAGE_LOST,   /* SD 不可用了（拔卡 / 进 U 盘模式） */
    EV_NTF_MSG,          /* ch, a=msg_t, b=参数：给界面的提示 */
    EV_NTF_ASK,          /* ch, a=ask_t, b=参数, src=原请求来源：需要用户在设备上确认 */
    /* Append extensions here to preserve existing simulator/API event numbers. */
    EV_REQ_REC_ROTATE,   /* Explicit bench fence; recorder also requires enabled bench. */
    EV_NTF_FILE_REMOVED, /* SD owner notifies core to compact player indexes. */
    EV_SD_WRITE_RECOVERED, /* Platform confirmed that a failed recording write recovered. */
    EV_COUNT
} ev_type_t;

typedef struct {
    uint16_t type;
    uint8_t  ch;
    uint8_t  src;
    int32_t  a;
    int32_t  b;
    uint32_t generation; /* CAN STATUS/PROBE_RESULT: controller incarnation; other events use zero. */
} app_evt_t;

/* ---- 给界面的提示码（文字在界面层，服务层只发码） ---- */
typedef enum {
    MSG_NONE,
    MSG_LOCKED, MSG_UNLOCKED, MSG_LOCKED_HINT, MSG_LOW_BATT, MSG_CHIP_REV_Y,
    MSG_NO_SD, MSG_SD_IN_MSC, MSG_SD_ERROR, MSG_SD_REPAIRED, MSG_SD_WP, MSG_SD_UNSUPPORTED,
    MSG_SD_FRAG, MSG_SD_FULL_NOREC, MSG_SD_REMOVED,
    MSG_REC_STARTED, MSG_REC_STOPPED, MSG_REC_MARK, MSG_REC_SEG_CFG, MSG_REC_SD_REMOVED,
    MSG_REC_WRITE_RETRY, MSG_REC_WRITE_FAIL, MSG_REC_SD_FULL, MSG_REC_DELETED_OLDEST,
    MSG_CH_LOW_RATE, MSG_CH_MODE, MSG_CH_RATE, MSG_CH_BUSOFF, MSG_CH_RECOVERED, MSG_CH_BUSOFF_GIVEUP,
    MSG_AB_START, MSG_AB_OK, MSG_AB_NO_TRAFFIC, MSG_AB_FAIL, MSG_AB_DENY_REC, MSG_AB_ABORTED,
    MSG_TX_NOACK, MSG_TX_PAUSED, MSG_TX_LOAD_WARN, MSG_TX_LOAD_DENY, MSG_TX_DENY_BUSOFF,
    MSG_PLAY_STARTED, MSG_PLAY_DONE, MSG_PLAY_STOPPED, MSG_PLAY_SD_REMOVED, MSG_PLAY_BAD_FILE,
    MSG_PLAY_FD_SKIPPED, MSG_PLAY_BUSY, MSG_PLAY_PAUSED_MODE, MSG_PLAY_NOT_PLAYABLE,
    MSG_MSC_ON, MSG_MSC_OFF, MSG_MSC_DENY, MSG_MSC_NO_USB, MSG_FORMAT_DONE, MSG_FORMAT_DENY,
    MSG_HOST_CFG_CHANGED, MSG_HOST_DENIED, MSG_HOST_ON, MSG_HOST_OFF,
    MSG_BUS_ABNORMAL, MSG_CH_NO_RATE, MSG_FACTORY_RESET, MSG_SELFTEST_OK, MSG_REC_BUSY_CFG, MSG_TIME_SYNCED,
    MSG_COUNT
} msg_t;

/* ---- 需要用户在设备上确认的问题 ---- */
typedef enum {
    ASK_NONE,
    ASK_CH_NORMAL,       /* 把通道切到正常模式（会往总线发送） */
    ASK_TX_NORMAL,       /* 发送前：通道不在正常模式，切过去并发送？ b=条目 */
    ASK_CYC_NORMAL,      /* 启动周期发送前：同上 b=条目 */
    ASK_TX_FIRST,        /* 刚切到正常模式后的第一次发送 b=条目 | 0x100 表示周期发送 */
    ASK_AB_RESTRICTED,   /* 只听识别失败：用受限模式再试（会回 ACK）？ */
    ASK_PLAY_CHMAP,      /* 文件通道多于 2 个，多出来的忽略？ b=文件通道数 */
    ASK_PLAY_RATE,       /* 文件速率和通道不同：套用文件参数？ b=文件标称速率 */
    ASK_PLAY_NORMAL,     /* 回放要把目标通道切到正常模式 */
    ASK_REC_STOP,        /* 上位机 / 命令行要求停止录制 */
    ASK_MSC,             /* 进入 U 盘模式 */
    ASK_FORMAT,          /* 格式化 SD 卡 */
    ASK_SEQ_NORMAL,      /* 主机模拟ID分组循环：切正常模式并启动，b=请求令牌 */
    ASK_SEQ_FIRST,       /* 主机模拟ID分组循环：首次发送确认，b=请求令牌 */
    ASK_PLAN_START,      /* 主机发送方案：所含通道统一模式/首次发送确认，b=请求令牌 */
    ASK_SCOPE_START,     /* 主机手动范围：所选通道统一模式/首次发送确认，b=请求令牌 */
    ASK_COUNT
} ask_t;

typedef enum {
    SD_OK, SD_OK_REPAIRED, SD_ERR_WP, SD_ERR_UNSUPPORTED, SD_ERR_IO, SD_ERR_FRAG, SD_ERR_FULL
} sd_result_t;

typedef enum {
    SHUT_NONE, SHUT_USER, SHUT_LOW_BATT, SHUT_AUTO_OFF, SHUT_MISFIRE, SHUT_HOST
} shutdown_reason_t;

typedef enum {
    PLAYCTL_PAUSE_TOGGLE, PLAYCTL_STOP, PLAYCTL_STEP, PLAYCTL_SPEED_NEXT, PLAYCTL_LOOP_TOGGLE
} play_ctl_t;

#endif
