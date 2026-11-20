#include "services/can_core/bittime.h"

uint32_t can_frame_time_ns(uint8_t len, uint8_t flags, uint32_t nbr, uint32_t dbr)
{
    bool ext = flags & CANF_EXT;
    if (nbr == 0)
        return 0;
    if (!(flags & CANF_FD)) {
        if (flags & CANF_RTR)
            len = 0;
        if (len > 8)
            len = 8;
        /* SOF..CRC 定界 + ACK 2 + EOF 7 + 帧间隔 3 */
        uint32_t bits = (ext ? 67u : 47u) + 8u * len;
        return (uint32_t)((uint64_t)bits * 1050000000u / nbr);   /* +5% 填充 */
    }
    /* FD：仲裁段（到 BRS）+ 尾部（ACK、EOF、帧间隔）按标称速率；数据段按数据速率 */
    uint32_t arb_bits = ext ? 36u : 17u;
    uint32_t tail_bits = 12u;
    uint32_t crc_bits = len <= 16 ? 17u : 21u;
    uint32_t fixed_stuff = len <= 16 ? 6u : 7u;
    uint32_t data_bits = 1u /*ESI*/ + 4u /*DLC*/ + 8u * len + 4u /*填充计数*/ + crc_bits + fixed_stuff + 1u /*CRC 定界*/;
    uint32_t rate_data = (flags & CANF_BRS) && dbr ? dbr : nbr;
    uint64_t ns = (uint64_t)(arb_bits + tail_bits) * 1050000000u / nbr
                + (uint64_t)data_bits * 1000000000u / rate_data;
    return (uint32_t)ns;
}
