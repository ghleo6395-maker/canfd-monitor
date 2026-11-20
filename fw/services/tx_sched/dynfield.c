#include "services/tx_sched/dynfield.h"

static uint8_t crc8(const uint8_t *p, size_t n, uint8_t poly)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ poly : crc << 1);
    }
    return crc ^ 0xFF;
}

uint8_t crc8_j1850(const uint8_t *p, size_t n) { return crc8(p, n, 0x1D); }
uint8_t crc8_h2f(const uint8_t *p, size_t n) { return crc8(p, n, 0x2F); }

const char *chk_name(chk_type_t t)
{
    static const char *const names[] = { "无", "XOR", "SUM", "CRC8-J1850", "CRC8H2F" };
    return t <= CHK_CRC8_H2F ? names[t] : "?";
}

void dyn_apply(dynfield_t *d, uint8_t *data, uint8_t len)
{
    if (d->cnt_bits && d->cnt_byte < len) {
        if (d->cnt_bits == 4)
            data[d->cnt_byte] = (uint8_t)((data[d->cnt_byte] & 0xF0) | (d->cnt_val & 0x0F));
        else
            data[d->cnt_byte] = d->cnt_val;
        d->cnt_val = d->cnt_val >= d->cnt_max ? 0 : (uint8_t)(d->cnt_val + 1);
    }
    if (d->chk == CHK_NONE || d->chk_byte >= len)
        return;
    uint8_t buf[64];
    size_t n = 0;
    uint8_t to = d->chk_to < len ? d->chk_to : (uint8_t)(len - 1);
    for (uint8_t i = d->chk_from; i <= to; i++)
        if (i != d->chk_byte)
            buf[n++] = data[i];
    uint8_t v = 0;
    switch (d->chk) {
    case CHK_XOR:
        for (size_t i = 0; i < n; i++)
            v ^= buf[i];
        break;
    case CHK_SUM:
        for (size_t i = 0; i < n; i++)
            v = (uint8_t)(v + buf[i]);
        break;
    case CHK_CRC8_J1850:
        v = crc8_j1850(buf, n);
        break;
    case CHK_CRC8_H2F:
        v = crc8_h2f(buf, n);
        break;
    default:
        break;
    }
    data[d->chk_byte] = v;
}
