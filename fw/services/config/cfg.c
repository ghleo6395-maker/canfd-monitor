#include "services/config/cfg.h"

static cfg_t cfg;
static uint32_t segment_override_kib;

/* 1 MB 一档只为在模拟器里能很快看到切段，固件里去掉 */
static const uint32_t seg_kb[REC_SEG_OPTIONS] = { 1024, 16 * 1024, 64 * 1024, 128 * 1024, 512 * 1024 };
static const char *const seg_name[REC_SEG_OPTIONS] = { "1 MB(仅模拟)", "16 MB", "64 MB", "128 MB", "512 MB" };

const cfg_t *cfg_get(void) { return &cfg; }
cfg_t *cfg_mut(void) { return &cfg; }

void cfg_defaults(void)
{
    segment_override_kib = 0;
    for (int ch = 0; ch < CH_NUM; ch++) {
        cfg.nbr[ch] = 500000;
        cfg.dbr[ch] = 2000000;
        cfg.fd[ch] = true;
        cfg.rate_saved[ch] = true;
    }
    cfg.busoff_auto = false;
    cfg.screen_off_s = 60;
    cfg.auto_off_min = 10;
    cfg.rec_ch_mask = 0x3;
    cfg.rec_seg_idx = 2;
    cfg.rec_full_delete = false;
    cfg.play_speed_idx = 1;
    cfg.play_loop = false;
    cfg.backlight_pct = 80;
}

uint32_t cfg_rec_seg_kb(void)
{
    if (segment_override_kib) return segment_override_kib;
    return seg_kb[cfg.rec_seg_idx % REC_SEG_OPTIONS];
}

void cfg_rec_seg_override(uint32_t kib) { segment_override_kib = kib; }

const char *cfg_rec_seg_name(uint8_t idx)
{
    return seg_name[idx % REC_SEG_OPTIONS];
}
