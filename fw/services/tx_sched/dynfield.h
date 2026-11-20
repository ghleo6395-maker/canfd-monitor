/* 动态字段（需求 F2.4）：滚动计数器 + 校验字节。每次发送前调用 dyn_apply。 */
#ifndef DYNFIELD_H
#define DYNFIELD_H

#include "services/core/app_types.h"

typedef enum { CHK_NONE, CHK_XOR, CHK_SUM, CHK_CRC8_J1850, CHK_CRC8_H2F } chk_type_t;

typedef struct {
    uint8_t    cnt_byte;     /* 计数器所在字节 */
    uint8_t    cnt_bits;     /* 0 = 不用计数器；4 = 低 4 位；8 = 整字节 */
    uint8_t    cnt_max;      /* 计到这个值后回 0 */
    uint8_t    cnt_val;
    chk_type_t chk;
    uint8_t    chk_byte;     /* 校验字节位置 */
    uint8_t    chk_from, chk_to;   /* 参与计算的字节范围（含），自动跳过校验字节本身 */
} dynfield_t;

void        dyn_apply(dynfield_t *d, uint8_t *data, uint8_t len);
uint8_t     crc8_j1850(const uint8_t *p, size_t n);   /* SAE J1850：多项式 0x1D，初值 0xFF，结果异或 0xFF */
uint8_t     crc8_h2f(const uint8_t *p, size_t n);     /* AUTOSAR CRC8H2F：多项式 0x2F，初值 0xFF，结果异或 0xFF */
const char *chk_name(chk_type_t t);

#endif
