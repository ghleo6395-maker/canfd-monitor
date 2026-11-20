/* 配置参数。只放 PERSIST 级参数（会写进 SD 卡上的 config.ini）。
 * SESSION 级状态（工作模式、后台任务是否在跑）不在这里，开机一律回到只听模式。
 * 模拟器里这份数据在"断电"后保留，相当于 config.ini。 */
#ifndef CFG_H
#define CFG_H

#include "services/core/app_types.h"

#define REC_SEG_OPTIONS 5

typedef struct {
    uint32_t nbr[CH_NUM];         /* 标称速率 */
    uint32_t dbr[CH_NUM];         /* 数据段速率 */
    bool     fd[CH_NUM];
    bool     rate_saved[CH_NUM];  /* 保存过速率才会开机自动监听 */
    bool     busoff_auto;         /* Bus-Off 自动恢复（默认手动） */
    uint16_t screen_off_s;        /* 0 = 不自动熄屏 */
    uint16_t auto_off_min;        /* 0 = 不自动关机 */
    uint8_t  rec_ch_mask;         /* bit0 CAN1, bit1 CAN2 */
    uint8_t  rec_seg_idx;         /* 分段大小选项 */
    bool     rec_full_delete;     /* 卡满时删除最旧会话（默认停止） */
    uint8_t  play_speed_idx;      /* 0.5x / 1x / 2x / 尽快 */
    bool     play_loop;
    uint8_t  backlight_pct;
} cfg_t;

const cfg_t *cfg_get(void);
cfg_t       *cfg_mut(void);
void         cfg_defaults(void);
uint32_t     cfg_rec_seg_kb(void);
void         cfg_rec_seg_override(uint32_t kib); /* Session-only bench; 0 restores config. */
const char  *cfg_rec_seg_name(uint8_t idx);

#endif
