/* SPDX-License-Identifier: MIT */
#ifndef REVD_REALTIME_IO_H
#define REVD_REALTIME_IO_H
#include <stdint.h>
/* Main loop and nested I/O waits share one elapsed-time cursor, preventing
 * double-counting when an SD operation advances the application clock. */
void revd_loop_clock_start(uint32_t now_ms);
uint32_t revd_loop_clock_take(uint32_t now_ms);
/* Foreground cooperative wait service. Never call from ISR and never dispatch
 * app commands, enter LVGL rendering or reenter FatFs/storage_sd_task here. */
void revd_io_wait_poll(void);
void revd_sdmmc_cmd_wait_poll(void);
#endif
