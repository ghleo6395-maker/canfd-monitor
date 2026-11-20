/* SPDX-License-Identifier: MIT */
#ifndef REVD_STORAGE_SD_H
#define REVD_STORAGE_SD_H

#include <stdbool.h>
#include <stdint.h>

/* CPU-only queue; frame encoding and all IDMA staging remain outside DTCM. */
#define REVD_SD_RECORD_QUEUE_FRAMES 1024u

/* Native I/O runs in the cooperative main loop; REVD_FREERTOS I/O runs only
 * on rtos/storage_owner. Public submission/query wrappers are task-only.
 * SDMMC1 kernel clock must already be selected (100 MHz PLL2R).
 * Call init after HAL_Init/clock setup, task once per main-loop iteration.
 * SDMMC IDMA stages through 32-byte-aligned AXI SRAM, D-cache disabled.
 * FatFs calls execute in task context; synchronous disk waits invoke the board's
 * revd_io_wait_poll() hook. Per-frame recording enqueues and never accesses I/O.
 * ISR callbacks only publish IDMA completion/error flags.
 * REVD_FREERTOS: rtos/storage_owner owns all HAL/FatFs and publishes cached
 * queries. Frame/command submission is task-only, bounded and non-blocking;
 * RPC replay/raw operations can block their caller until definite completion.
 * No request retains a caller stack buffer after returning. */
void storage_sd_init(void);
void storage_sd_task(void);

typedef struct {
    uint32_t queued_frames, queue_high_water, queue_overflows;
    /* encoded_frames reached the text buffer, not necessarily durable media. */
    uint32_t enqueued_frames, encoded_frames, discarded_frames;
    uint32_t dma_transfers, dma_errors, dma_timeouts, cooperative_polls;
    uint32_t card_busy_timeouts, max_card_busy_ms, max_dma_wait_ms;
    uint32_t write_calls, sync_calls, max_write_ms, max_sync_ms, max_task_ms;
    uint32_t segments_opened, segments_closed, command_high_water, command_overflows;
    /* Current mounted catalog is a bounded 128-entry cache, not a recording
     * limit. A full cache reserves an unpinned representative for the current
     * session without deleting old media files. Omitted counts uncached logs;
     * mount recounts it by directory
     * enumeration. Omitted files cannot be selected/protected/deleted by index.
     * Other counters are lifetime totals and high-water/duration maxima. */
    uint32_t catalog_limited, catalog_omitted;
    uint32_t event_deferred,event_coalesced,event_high_water,event_fallback;
    uint64_t written_bytes; /* f_write-accepted bytes; not a power-loss durability claim. */
} storage_sd_stats_t;
void storage_sd_stats(storage_sd_stats_t *out);

/* Exclusive MSC ownership. give requires no recording or active player and an
 * unmounted local volume; it never silently ejects an open file. reclaim does
 * not mount; storage_sm handles remounting on exit/eject/disconnect. */
bool storage_sd_media_give(void);
void storage_sd_media_reclaim(void);
bool storage_sd_raw_ready(void);
bool storage_sd_raw_sync(void);
uint32_t storage_sd_raw_block_count(void);
bool storage_sd_raw_read(uint32_t lba, uint8_t *data, uint32_t count);
bool storage_sd_raw_write(uint32_t lba, const uint8_t *data, uint32_t count);

/* FatFs diskio adapter only: local owner, 512-byte sectors. Do not use these
 * from USB; raw_* additionally enforce MSC ownership. */
bool storage_sd_disk_init(void);
bool storage_sd_disk_ready(void);
bool storage_sd_disk_read(uint32_t lba, uint8_t *data, uint32_t count);
bool storage_sd_disk_write(uint32_t lba, const uint8_t *data, uint32_t count);
bool storage_sd_disk_sync(void);
uint32_t storage_sd_disk_block_count(void);

#endif
