/* 文件回放状态机（需求 4.4），每个目标通道一个实例。
 *
 *          REQ_PLAY_OPEN                   所有检查都通过
 *   IDLE ────────────────▶ CHECKING ─────────────────────▶ PLAYING ◀──暂停/继续──▶ PAUSED（可单步）
 *    ▲   格式认不出：拒绝     │ 逐条问用户：                     │ 放完（循环则从头再来）
 *    │                      │  通道多于 2 个 / 速率不同 /      │ 停止 / 拔卡 / 关机
 *    │◀── 用户取消 ──────────┘  需要切到正常模式               ▼
 *    └──────────────────────────────────────────────────── 回到 IDLE
 *   SD 读不过来（例如同时在录制）时整条时间轴顺延，保持帧间隔（F4.5）。 */
#ifndef PLAY_SM_H
#define PLAY_SM_H

#include "services/core/app_types.h"

typedef enum { PL_IDLE, PL_CHECKING, PL_PLAYING, PL_PAUSED, PL_STATE_NUM } play_state_t;

typedef struct {
    play_state_t state;
    int          file;
    uint8_t      pending;        /* 还没问的检查项（位） */
    ask_t        asking;         /* 正在等用户回答的问题 */
    uint32_t     vt_ms;          /* 回放进度（文件时间） */
    uint8_t      vt_remainder;   /* 倍速换算的十分之一微秒余数 */
    uint32_t     duration_ms;
    uint64_t     vt_us, duration_us; /* actual imported relative timestamps */
    uint32_t     frames_total, frames_sent, fd_skipped, error_skipped;
    uint32_t     shifts;         /* 断流顺延次数 */
    uint32_t     loops_done;
    bool         file_fd;
    uint32_t     next_frame;
    uint32_t     shift_acc_ms;
} player_t;

void            play_init(void);
void            play_handle(const app_evt_t *e);
void            play_tick(uint32_t dt_ms);
const player_t *play_get(int ch);
bool            play_any_active(void);
/* Storage deletion must skip in-use files and notify every compacted index. */
bool            play_file_in_use(int idx);
void            play_file_removed(int idx);
const char     *play_state_name(play_state_t s);
const char     *play_speed_name(uint8_t idx);

#endif
