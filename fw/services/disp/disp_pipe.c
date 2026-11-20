#include "services/disp/disp_pipe.h"
#include "services/core/app_timer.h"
#include "services/frame_ring/frame_ring.h"
#include "services/core/port.h"

#include <string.h>

#define UI_ROWS_PER_S 30u   /* 屏幕滚动视图每秒能让人看清的大约行数 */

#ifdef CANFD_REVD_TARGET
/* Keep the original 40 KiB AXI history prefix. Complete FD tails plus the two
 * ID tables fit in the H743's separate 64 KiB SRAM4. These are CPU-only view
 * data: never pass them to SDMMC/SPI DMA. */
#if defined(__arm__) || defined(__thumb__)
#define VIEW_RAM __attribute__((section(".revd_d3_bss"), aligned(32)))
#else
/* Target-logic tests on Mach-O use the normal host BSS; ELF verification
 * independently checks the physical ARM section addresses and sizes. */
#define VIEW_RAM __attribute__((aligned(32)))
#endif
typedef struct {
    uint64_t ts_us;
    uint32_t id, dt_same_us;
    uint8_t ch, flags, len, data[16];
} stored_row_t;
static stored_row_t rows[DISP_ROWS];
static uint8_t row_tail[DISP_ROWS][48] VIEW_RAM;
_Static_assert(sizeof row_tail + 2u * sizeof(disp_id_t) * DISP_IDS <= 65536u,
               "complete display history exceeds SRAM4");
#else
#define VIEW_RAM
static disp_row_t rows[DISP_ROWS];
#endif
static uint32_t      rows_head;          /* 已写入的总行数 */
static disp_id_t     ids[DISP_IDS] VIEW_RAM;
static int           n_ids;
static uint32_t      id_others;
#if DISP_DATA > 16
/* Pause freezes visible ID rows while recorder consumers continue. */
static disp_id_t frozen_ids[DISP_IDS] VIEW_RAM;
static int frozen_id_count;
static uint32_t frozen_others;
static uint64_t frozen_at_us;
#endif
static ring_reader_t rd;
static bool          paused;
static uint32_t      hidden;
static disp_filter_t filter;
static uint32_t      win_start, win_matched, sampled_n = 1;

void disp_init(void)
{
    rows_head = 0;
    n_ids = 0;
    id_others = 0;
    paused = false;
#if DISP_DATA > 16
    frozen_id_count=0; frozen_others=0;
#endif
    hidden = 0;
    memset(&filter, 0, sizeof filter);
    win_matched = 0;
    sampled_n = 1;
    win_start = app_now_ms();
    ring_reader_attach(&rd, false);
}

static bool match(const can_frame_t *f)
{
    if (filter.channel == DF_CH_1 && f->ch != 0)
        return false;
    if (filter.channel == DF_CH_2 && f->ch != 1)
        return false;
    if ((f->id & filter.mask) != (filter.id & filter.mask)) return false;
    if (filter.direction == DF_DIR_RX && (f->flags & CANF_TX)) return false;
    if (filter.direction == DF_DIR_TX && !(f->flags & CANF_TX)) return false;
    if (filter.format == DF_FMT_STD && (f->flags & CANF_EXT)) return false;
    if (filter.format == DF_FMT_EXT && !(f->flags & CANF_EXT)) return false;
    switch (filter.type) {
    case DF_T_FD:  return (f->flags & CANF_FD) != 0;
    case DF_T_ERR: return (f->flags & CANF_ERR) != 0;
    case DF_T_TX:  return (f->flags & CANF_TX) != 0;
    default:       return true;
    }
}

/* 按 ID 表：错误记录不进表；表满时计入"其他" */
static disp_id_t *id_update(const can_frame_t *f, uint32_t *dt_out)
{
    *dt_out = 0;
    if (f->flags & CANF_ERR)
        return NULL;
    for (int i = 0; i < n_ids; i++) {
        disp_id_t *d = &ids[i];
        if (d->id == f->id && d->ch == f->ch &&
            ((d->flags ^ f->flags) & (CANF_EXT | CANF_RTR | CANF_TX | CANF_FD)) == 0) {
            uint32_t dt = (uint32_t)(f->ts_us - d->last_ts);
            *dt_out = dt;
            d->per_avg_us = d->count == 1 ? dt : (uint32_t)(((uint64_t)d->per_avg_us * 7u + dt) / 8u);
            if (d->count == 1 || dt < d->per_min_us)
                d->per_min_us = dt;
            if (dt > d->per_max_us)
                d->per_max_us = dt;
            uint8_t n = f->len < DISP_DATA ? f->len : DISP_DATA;
            for (uint8_t b = 0; b < n; b++) {
                if (d->data[b] != f->data[b]) {
                    if (b < 16) d->chg_mask |= (uint16_t)(1u << b);
#if DISP_DATA > 16
                    d->changed_full |= UINT64_C(1) << b;
#endif
                    d->chg_ts = f->ts_us;
                }
            }
            memcpy(d->data, f->data, n);
            d->len = f->len;
            d->flags = f->flags;
            d->count++;
            d->last_ts = f->ts_us;
            return d;
        }
    }
    if (n_ids >= DISP_IDS) {
        id_others++;
        return NULL;
    }
    disp_id_t *d = &ids[n_ids++];
    memset(d, 0, sizeof *d);
    d->id = f->id;
    d->ch = f->ch;
    d->flags = f->flags;
    d->len = f->len;
    memcpy(d->data, f->data, f->len < DISP_DATA ? f->len : DISP_DATA);
    d->count = 1;
    d->last_ts = f->ts_us;
    return d;
}

void disp_tick(void)
{
    const can_frame_t *f;
    while ((f = ring_peek(&rd)) != NULL) {
        if (match(f)) {
            uint32_t dt;
            id_update(f, &dt);
            win_matched++;
            if (paused) {
                hidden++;
            } else {
#ifdef CANFD_REVD_TARGET
                stored_row_t *r = &rows[rows_head % DISP_ROWS];
#else
                disp_row_t *r = &rows[rows_head % DISP_ROWS];
#endif
                r->ts_us = f->ts_us;
                r->id = f->id;
                r->dt_same_us = dt;
                r->ch = f->ch;
                r->flags = f->flags;
                r->len = f->len;
                unsigned prefix = f->len < sizeof r->data ? f->len : sizeof r->data;
                memcpy(r->data, f->data, prefix);
#ifdef CANFD_REVD_TARGET
                unsigned full = f->len < DISP_DATA ? f->len : DISP_DATA;
                if (full > sizeof r->data)
                    memcpy(row_tail[rows_head % DISP_ROWS], f->data + sizeof r->data,
                           full - sizeof r->data);
#endif
                rows_head++;
            }
        }
        ring_advance(&rd);
    }
    /* 变化高亮 1.5 s 后渐隐 */
    uint64_t now_us = port_capture_time_us();
    for (int i = 0; i < n_ids; i++)
        if (now_us >= ids[i].chg_ts && now_us - ids[i].chg_ts > 1500000u) {
            ids[i].chg_mask = 0;
#if DISP_DATA > 16
            ids[i].changed_full = 0;
#endif
        }

    uint32_t now = app_now_ms();
    if (now - win_start >= 1000) {
        uint32_t per_s = win_matched * 1000u / (now - win_start);
        sampled_n = per_s > UI_ROWS_PER_S ? (per_s + UI_ROWS_PER_S - 1) / UI_ROWS_PER_S : 1;
        win_matched = 0;
        win_start = now;
    }
}

bool disp_filter_set_ex(const disp_filter_t *f)
{
    if (!f || (unsigned)f->channel >= DF_CH_NUM || (unsigned)f->type >= DF_T_NUM ||
        (unsigned)f->direction >= DF_DIR_NUM || (unsigned)f->format >= DF_FMT_NUM ||
        f->id > 0x1FFFFFFFu || f->mask > 0x1FFFFFFFu)
        return false;
    if (filter.channel != f->channel || filter.type != f->type || filter.id != f->id ||
        filter.mask != f->mask || filter.direction != f->direction || filter.format != f->format) {
        rows_head = 0;
        disp_clear_ids();
        hidden = 0;
        win_matched = 0;
        sampled_n = 1;
        win_start = app_now_ms();
    }
    filter = *f;
    return true;
}

void disp_filter_get_ex(disp_filter_t *out)
{
    if (out) *out = filter;
}

void disp_set_filter(disp_ch_filter_t ch, disp_type_filter_t t)
{
    disp_filter_t f = filter;
    f.channel = ch;
    f.type = t;
    (void)disp_filter_set_ex(&f);
}

void disp_get_filter(disp_ch_filter_t *ch, disp_type_filter_t *t)
{
    if (ch) *ch = filter.channel;
    if (t) *t = filter.type;
}

void disp_pause(bool on)
{
#if DISP_DATA > 16
    if (on && !paused) {
        memcpy(frozen_ids,ids,sizeof frozen_ids);
        frozen_id_count=n_ids; frozen_others=id_others; frozen_at_us=port_capture_time_us();
    }
#endif
    paused = on;
    if (!on)
        hidden = 0;
}

bool disp_paused(void) { return paused; }
uint64_t disp_view_time_us(void) {
#if DISP_DATA > 16
    if (paused) return frozen_at_us;
#endif
    return port_capture_time_us();
}

uint32_t disp_rows_available(void)
{
    return rows_head < DISP_ROWS ? rows_head : DISP_ROWS;
}

bool disp_row(uint32_t back, disp_row_t *out)
{
    if (back >= disp_rows_available())
        return false;
#ifdef CANFD_REVD_TARGET
    uint32_t index = (rows_head - 1u - back) % DISP_ROWS;
    const stored_row_t *r = &rows[index];
    memset(out, 0, sizeof *out);
    out->ts_us = r->ts_us; out->id = r->id; out->dt_same_us = r->dt_same_us;
    out->ch = r->ch; out->flags = r->flags; out->len = r->len;
    unsigned prefix = r->len < sizeof r->data ? r->len : sizeof r->data;
    memcpy(out->data, r->data, prefix);
    unsigned full = r->len < DISP_DATA ? r->len : DISP_DATA;
    if (full > sizeof r->data)
        memcpy(out->data + sizeof r->data, row_tail[index], full - sizeof r->data);
#else
    *out = rows[(rows_head - 1 - back) % DISP_ROWS];
#endif
    return true;
}

bool disp_row_seq(uint32_t back, uint32_t *sequence)
{
    if (!sequence || back >= disp_rows_available()) return false;
    *sequence = rows_head - 1u - back;
    return true;
}
bool disp_row_back_for_seq(uint32_t sequence, uint32_t *back)
{
    if (!back) return false;
    uint32_t candidate = rows_head - 1u - sequence;
    if (candidate >= disp_rows_available()) return false;
    *back = candidate;
    return true;
}
bool disp_id_byte_changed(const disp_id_t *entry, unsigned byte)
{
    if (!entry || byte >= DISP_DATA) return false;
#if DISP_DATA > 16
    return (entry->changed_full & (UINT64_C(1) << byte)) != 0;
#else
    return (entry->chg_mask & (1u << byte)) != 0;
#endif
}
uint32_t disp_hidden_while_paused(void) { return hidden; }
uint32_t disp_sampled_n(void) { return sampled_n; }
int disp_id_count(void) {
#if DISP_DATA > 16
    if (paused) return frozen_id_count;
#endif
    return n_ids;
}
const disp_id_t *disp_id_at(int i) {
#if DISP_DATA > 16
    if (paused) return i >= 0 && i < frozen_id_count ? &frozen_ids[i] : NULL;
#endif
    return i >= 0 && i < n_ids ? &ids[i] : NULL;
}
uint32_t disp_id_others(void) {
#if DISP_DATA > 16
    if (paused) return frozen_others;
#endif
    return id_others;
}

void disp_clear_ids(void)
{
    n_ids = 0;
    id_others = 0;
#if DISP_DATA > 16
    frozen_id_count=0; frozen_others=0;
#endif
}
