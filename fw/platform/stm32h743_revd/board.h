/* SPDX-License-Identifier: MIT */
#ifndef REVD_BOARD_H
#define REVD_BOARD_H
#include "stm32h7xx_hal.h"
#include <stdint.h>
void board_clock_init(void);
void board_gpio_init(void);
void board_io_task(void);
void board_can_task(void);
void board_fatal(uint32_t code);
uint64_t board_time_us(void);
/* Per-MCU-boot random identity for benchmark evidence; empty on RNG failure. */
const char *board_boot_id(void);
#endif
