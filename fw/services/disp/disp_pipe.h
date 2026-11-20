/* 显示管线（框架设计 7 节第 4 条，需求 F5.1 / F5.2）：
 *  - 每一帧先按显示筛选条件过滤，再算"与同 ID 上一帧的间隔"，写进显示环；
 *  - 暂停时显示环冻结，可往回翻（显示环 1024 条，满足"至少 1000 条"）；
 *  - 同时维护按 ID 汇总表（每个 (通道, ID, 格式, 方向) 一行，表满后合并成"其他"）。
 * 筛选在这里对所有帧做，界面只按固定频率取快照，所以罕见 ID 不会被抽样漏掉。 */
#ifndef DISP_PIPE_H
#define DISP_PIPE_H

#include "services/core/app_types.h"

#define DISP_ROWS  1024
#define DISP_IDS   64     /* 当前聚合键容量；超出后计入 others。 */
/* Device and simulator expose the same complete CAN FD payload. The board
 * implementation stores history tails in CPU-only SRAM4, preserving all
 * existing CAN/SD DMA queue capacities. */
#define DISP_DATA 64

typedef enum { DF_CH_ALL, DF_CH_1, DF_CH_2, DF_CH_NUM } disp_ch_filter_t;
typedef enum { DF_T_ALL, DF_T_FD, DF_T_ERR, DF_T_TX, DF_T_NUM } disp_type_filter_t;
typedef enum { DF_DIR_ALL, DF_DIR_RX, DF_DIR_TX, DF_DIR_NUM } disp_direction_filter_t;
typedef enum { DF_FMT_ALL, DF_FMT_STD, DF_FMT_EXT, DF_FMT_NUM } disp_format_filter_t;

typedef struct {
    disp_ch_filter_t channel;
    disp_type_filter_t type;
    uint32_t id, mask;       /* (frame.id & mask) == (id & mask); mask=0 disables */
    disp_direction_filter_t direction;
    disp_format_filter_t format;
} disp_filter_t;

/* Display-only: independent recorder/ring readers always retain original frames. */
bool     disp_filter_set_ex(const disp_filter_t *filter);
void     disp_filter_get_ex(disp_filter_t *out);

typedef struct {
    uint64_t ts_us;
    uint32_t id;
    uint32_t dt_same_us;   /* 与同 ID 上一帧的间隔，0 = 第一次出现 */
    uint8_t  ch, flags, len;
    uint8_t  data[DISP_DATA];
} disp_row_t;

typedef struct {
    uint32_t id;
    uint8_t  ch, flags, len;
    uint8_t  data[DISP_DATA];
    uint32_t count;
    uint64_t last_ts;
    uint32_t per_avg_us, per_min_us, per_max_us;
    uint16_t chg_mask;     /* 前 16 字节里最近变化过的字节 */
    uint64_t chg_ts;
#if DISP_DATA > 16
    uint64_t changed_full; /* Full-payload change highlighting. */
#endif
} disp_id_t;

void     disp_init(void);
void     disp_tick(void);
void     disp_set_filter(disp_ch_filter_t ch, disp_type_filter_t t);
void     disp_get_filter(disp_ch_filter_t *ch, disp_type_filter_t *t);
void     disp_pause(bool on);
bool     disp_paused(void);
uint64_t disp_view_time_us(void); /* Frozen ID rows age at their pause timestamp. */
uint32_t disp_rows_available(void);
bool     disp_row(uint32_t back, disp_row_t *out);   /* back=0 是最新一行 */
bool     disp_row_seq(uint32_t back, uint32_t *sequence);
bool     disp_row_back_for_seq(uint32_t sequence, uint32_t *back);
bool     disp_id_byte_changed(const disp_id_t *entry, unsigned byte);
uint32_t disp_hidden_while_paused(void);
uint32_t disp_sampled_n(void);                       /* 1 = 没抽样；N = 屏幕每 N 帧显示 1 帧 */
int      disp_id_count(void);
const disp_id_t *disp_id_at(int i);
uint32_t disp_id_others(void);
void     disp_clear_ids(void);

#endif
