/* Pure TX frame/dynamic checks, shared by scheduler and bounded file codec. */
#include "services/tx_sched/tx_sm.h"
#include "services/config/cfg.h"

bool tx_length_valid(uint8_t len, bool fd)
{
    return len <= 8 || (fd && (len == 12 || len == 16 || len == 20 ||
           len == 24 || len == 32 || len == 48 || len == 64));
}

tx_result_t tx_validate_frame(int ch, const can_frame_t *f, bool for_send)
{
    if (ch < 0 || ch >= CH_NUM) return TX_ERR_CHANNEL;
    if (!f) return TX_ERR_NULL;
    if (f->id > ((f->flags & CANF_EXT) ? 0x1FFFFFFFu : 0x7FFu)) return TX_ERR_ID;
    const unsigned allowed = CANF_EXT | CANF_FD | CANF_BRS | CANF_RTR;
    if ((f->flags & ~allowed) || ((f->flags & CANF_BRS) && !(f->flags & CANF_FD)) ||
        ((f->flags & CANF_RTR) && (f->flags & CANF_FD))) return TX_ERR_FLAGS;
    if (!tx_length_valid(f->len, (f->flags & CANF_FD) != 0)) return TX_ERR_LENGTH;
    if (for_send && (f->flags & CANF_FD) && !cfg_get()->fd[ch]) return TX_ERR_FD_DISABLED;
    return TX_OK;
}

tx_result_t tx_validate_dynamic(const dynfield_t *d,const can_frame_t *f)
{
    if(!d||!f)return TX_ERR_NULL;
    if(d->cnt_bits!=0&&d->cnt_bits!=4&&d->cnt_bits!=8)return TX_ERR_DYNAMIC;
    if((unsigned)d->chk>CHK_CRC8_H2F)return TX_ERR_DYNAMIC;
    if(f->flags&CANF_RTR)return d->cnt_bits||d->chk!=CHK_NONE?TX_ERR_DYNAMIC:TX_OK;
    if(d->cnt_bits&&(d->cnt_byte>=f->len||d->cnt_max>(d->cnt_bits==4?15:255)||d->cnt_val>d->cnt_max))return TX_ERR_DYNAMIC;
    if(d->chk!=CHK_NONE&&(d->chk_byte>=f->len||d->chk_from>d->chk_to||d->chk_to>=f->len))return TX_ERR_DYNAMIC;
    if(d->cnt_bits&&d->chk!=CHK_NONE&&d->cnt_byte==d->chk_byte)return TX_ERR_DYNAMIC;
    return TX_OK;
}

