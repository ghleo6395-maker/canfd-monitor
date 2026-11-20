/* SPDX-License-Identifier: MIT
 * Private storage-owner implementation names. Never include in app/USB code.
 */
#ifndef REVD_STORAGE_OWNER_DRIVER_H
#define REVD_STORAGE_OWNER_DRIVER_H
#include "services/core/port.h"
#include "storage_sd.h"
bool owner_port_sd_present(void);
void owner_storage_sd_stats(storage_sd_stats_t *out);
bool owner_storage_sd_media_give(void);
void owner_storage_sd_media_reclaim(void);
bool owner_storage_sd_raw_ready(void);
bool owner_storage_sd_raw_sync(void);
uint32_t owner_storage_sd_raw_block_count(void);
bool owner_storage_sd_raw_read(uint32_t lba,uint8_t *data,uint32_t count);
bool owner_storage_sd_raw_write(uint32_t lba,const uint8_t *data,uint32_t count);
bool owner_port_sd_replay_info(int index,uint8_t source,uint32_t *frames,uint64_t *duration_us);
bool owner_port_sd_read_frame(int index,uint8_t source,uint32_t ordinal,can_frame_t *out);
int owner_port_sd_file_count(void);
bool owner_port_sd_file_info(int index,file_info_t *out);
uint32_t owner_port_sd_free_kb(void);
uint32_t owner_port_sd_total_kb(void);
uint32_t owner_port_sd_next_session(void);
bool owner_port_sd_busy(void);
/* Always I/O-free; owner snapshot publication must not scan whole log files. */
bool storage_sd_peek_file_info(int index,file_info_t *out);
bool storage_sd_queue_busy(void);
bool storage_sd_recording_active(void);
void storage_sd_capture_stats(storage_sd_stats_t *out);

#ifdef REVD_STORAGE_OWNER_IMPL
#define port_sd_present owner_port_sd_present
#define storage_sd_stats owner_storage_sd_stats
#define storage_sd_media_give owner_storage_sd_media_give
#define storage_sd_media_reclaim owner_storage_sd_media_reclaim
#define storage_sd_raw_ready owner_storage_sd_raw_ready
#define storage_sd_raw_sync owner_storage_sd_raw_sync
#define storage_sd_raw_block_count owner_storage_sd_raw_block_count
#define storage_sd_raw_read owner_storage_sd_raw_read
#define storage_sd_raw_write owner_storage_sd_raw_write
#define port_sd_replay_info owner_port_sd_replay_info
#define port_sd_read_frame owner_port_sd_read_frame
#define port_sd_file_count owner_port_sd_file_count
#define port_sd_file_info owner_port_sd_file_info
#define port_sd_free_kb owner_port_sd_free_kb
#define port_sd_total_kb owner_port_sd_total_kb
#define port_sd_next_session owner_port_sd_next_session
#define port_sd_busy owner_port_sd_busy
#endif
#endif
