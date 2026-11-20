/* 帧时长估算：用于负载率统计、周期发送的负载检查。
 * 按 ISO 11898-1 帧结构计位，动态填充位按 +5% 近似（精确计算要逐帧算 CRC，放到 P1）。 */
#ifndef BITTIME_H
#define BITTIME_H

#include "services/core/app_types.h"

uint32_t can_frame_time_ns(uint8_t len, uint8_t flags, uint32_t nbr, uint32_t dbr);

#endif
