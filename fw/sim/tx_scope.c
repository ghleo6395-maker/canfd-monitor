#include "sim/tx_scope.h"
#include "sim/tx_sequence.h"
#include "sim/send_plan.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/core/app_evt.h"
#include "app/ui/core/ui_sm.h"
#include <stdio.h>
#include <string.h>

static scope_status_t status;
static uint32_t serial;
static struct {
    uint32_t boot,tx[2][4],seq[2],nbr[2],dbr[2];
    bool fd[2];
} binding;
static bool included(int ch){return (status.mask&(1u<<ch))!=0;}
const char *scope_result_text(int result){return txseq_result_text(result);}
static int result(int error,const char *text)
{
    status.error=error;
    snprintf(status.message,sizeof status.message,"%s",text?text:scope_result_text(error));
    return error;
}
void scope_init(void){memset(&status,0,sizeof status);memset(&binding,0,sizeof binding);}
const scope_status_t *scope_state(void){return &status;}
static bool ready(void){return power_running()&&power_get()->screen_on&&!power_get()->locked;}
static bool timing_valid(const txseq_entry_config_t *entry)
{
    return entry->repeat_count>=1&&entry->repeat_count<=TXSEQ_MAX_COUNT&&
        entry->frame_gap_ms<=TXSEQ_MAX_TIME_MS&&entry->cycle_ms>=1&&entry->cycle_ms<=TXSEQ_MAX_TIME_MS&&
        (entry->repeat_count==1||entry->frame_gap_ms>=1)&&
        entry->cycle_ms>(uint64_t)(entry->repeat_count-1)*entry->frame_gap_ms;
}
static int check(bool bound)
{
    if(!ready())return TXSEQ_ERR_NOT_READY;
    const sp_status_t *plan=sp_state();
    if((plan->loaded_mask&status.mask)&&(plan->state==SP_RUNNING||plan->state==SP_CONFIRMING||plan->state==SP_ARMING))return TXSEQ_ERR_CONFLICT;
    if(bound&&binding.boot!=power_get()->boot_count)return TXSEQ_ERR_CHANGED;
    for(int ch=0;ch<CH_NUM;ch++)if(included(ch)){
        const txseq_channel_t *sequence=txseq_get(ch);
        if(bound&&(sequence->revision!=binding.seq[ch]||cfg_get()->nbr[ch]!=binding.nbr[ch]||
            cfg_get()->dbr[ch]!=binding.dbr[ch]||cfg_get()->fd[ch]!=binding.fd[ch]))return TXSEQ_ERR_CHANGED;
        if(sequence->state!=TXSEQ_IDLE||tx_get()->pending[ch])return TXSEQ_ERR_ACTIVE;
        if(play_get(ch)->state!=PL_IDLE)return TXSEQ_ERR_CONFLICT;
        for(int i=0;i<TX_ENTRIES;i++){
            const tx_entry_t *entry=&tx_get()->e[ch][i];
            if(entry->active)return TXSEQ_ERR_CONFLICT;
            if(bound&&entry->revision!=binding.tx[ch][i])return TXSEQ_ERR_CHANGED;
        }
        const chan_t *channel=chan_get(ch);
        if(!chan_rate_allows_tx(ch)||channel->state==CH_BUSOFF||channel->state==CH_AUTOBAUD||channel->state==CH_STANDBY)return TXSEQ_ERR_MODE;
        bool enabled=false;
        if(status.kind==SCOPE_GROUP&&sequence->id_gap_ms>TXSEQ_MAX_TIME_MS)return TXSEQ_ERR_ARGUMENT;
        for(int i=0;i<TX_ENTRIES;i++){
            if(status.kind==SCOPE_GROUP&&!timing_valid(&sequence->entries[i]))return TXSEQ_ERR_ARGUMENT;
            if(status.kind==SCOPE_GROUP?!sequence->entries[i].enabled:(unsigned)i!=status.entry)continue;
            enabled=true;const tx_entry_t *entry=&tx_get()->e[ch][i];
            if(tx_validate_frame(ch,&entry->f,true)!=TX_OK||tx_validate_dynamic(&entry->dyn,&entry->f)!=TX_OK)return TXSEQ_ERR_FRAME;
        }
        if(!enabled)return TXSEQ_ERR_EMPTY;
        if(status.kind==SCOPE_GROUP&&txseq_load_permille(ch)>1000)return TXSEQ_ERR_LOAD;
    }
    return TXSEQ_OK;
}
static void bind(void)
{
    binding.boot=power_get()->boot_count;
    for(int ch=0;ch<CH_NUM;ch++)if(included(ch)){
        binding.seq[ch]=txseq_get(ch)->revision;binding.nbr[ch]=cfg_get()->nbr[ch];binding.dbr[ch]=cfg_get()->dbr[ch];binding.fd[ch]=cfg_get()->fd[ch];
        for(int i=0;i<TX_ENTRIES;i++)binding.tx[ch][i]=tx_get()->e[ch][i].revision;
    }
}
static int activate(void)
{
    int rc=check(true);if(rc){status.state=SCOPE_IDLE;return result(rc,NULL);}
    for(int ch=0;ch<CH_NUM;ch++)if(included(ch))if(!chan_can_tx(ch))return TXSEQ_ERR_MODE;
    /* Validation and commit execute under the simulator owner lock. No app or
     * hardware events are pumped between the two channels. Both FIFO slots are
     * known free, so single submissions cannot fail after prevalidation. */
    for(int ch=0;ch<CH_NUM;ch++)if(included(ch))chan_clear_first_tx(ch);
    for(int ch=0;ch<CH_NUM;ch++)if(included(ch)){
        if(status.kind==SCOPE_GROUP)rc=txseq_start(ch,binding.seq[ch]);
        else rc=tx_entry_submit(ch,(int)status.entry,TXSRC_MANUAL)?TXSEQ_OK:TXSEQ_ERR_FRAME;
        if(rc!=TXSEQ_OK){
            if(status.kind==SCOPE_GROUP)for(int undo=0;undo<CH_NUM;undo++)if(included(undo))txseq_stop(undo);
            status.state=SCOPE_IDLE;return result(rc,NULL);
        }
    }
    status.state=SCOPE_IDLE;
    return result(TXSEQ_OK,status.kind==SCOPE_GROUP?"所选通道已按各自列表同时启动；实际时序由各自调度器执行":"所选通道已各发送本槽的一帧");
}
static int request(uint32_t mask,scope_kind_t kind,uint32_t entry)
{
    if(!mask||(mask&~3u)||entry>=TX_ENTRIES)return result(TXSEQ_ERR_ARGUMENT,NULL);
    if(status.state!=SCOPE_IDLE||ui_dialog_open()||ui_draft_open())return result(TXSEQ_ERR_ACTIVE,NULL);
    status.mask=mask;status.kind=kind;status.entry=entry;
    int rc=check(false);if(rc)return result(rc,NULL);
    bind();bool confirm=false;
    for(int ch=0;ch<CH_NUM;ch++)if(included(ch)&&(!chan_can_tx(ch)||chan_get(ch)->first_tx_pending))confirm=true;
    if(confirm){
        serial=(serial+1)&0x7FFFFFFFu;if(!serial)serial=1;status.request=serial;status.state=SCOPE_CONFIRMING;
        evt_ask(ASK_SCOPE_START,0,(int32_t)status.request,SRC_UI);
        return result(TXSEQ_WAIT_CONFIRM,"请确认所选通道的工作模式和首次发送权限；确认前不发送");
    }
    return activate();
}
int scope_start(uint32_t mask){return request(mask,SCOPE_GROUP,0);}
int scope_single(uint32_t mask,uint32_t entry){return request(mask,SCOPE_SINGLE,entry);}
void scope_cancel(void)
{
    if(status.state!=SCOPE_IDLE){status.state=SCOPE_IDLE;result(0,"已取消所选范围的发送请求；未启动发送");}
}
int scope_answer(uint32_t request_id,bool allow)
{
    if(status.state!=SCOPE_CONFIRMING||request_id!=status.request)return result(TXSEQ_ERR_REQUEST,NULL);
    if(!allow){scope_cancel();return TXSEQ_OK;}
    int rc=check(true);if(rc){status.state=SCOPE_IDLE;return result(rc,NULL);}
    for(int ch=0;ch<CH_NUM;ch++)if(included(ch)&&!chan_can_tx(ch)){
        if(!evt_post4(EV_REQ_CH_MODE,(uint8_t)ch,SRC_UI,CAN_MODE_NORMAL,2)){
            status.state=SCOPE_IDLE;return result(TXSEQ_ERR_MODE,NULL);
        }
    }
    status.state=SCOPE_ARMING;
    return result(TXSEQ_OK,"正在为所选通道设置工作模式；全部就绪后一起启动");
}
void scope_stop(uint32_t mask)
{
    if(!mask||(mask&~3u)){result(TXSEQ_ERR_ARGUMENT,NULL);return;}
    if(status.state!=SCOPE_IDLE&&(status.mask&mask))scope_cancel();
    for(int ch=0;ch<CH_NUM;ch++)if(mask&(1u<<ch)){
        txseq_stop(ch);
        for(int i=0;i<TX_ENTRIES;i++)evt_post4(EV_REQ_TX_CYC,(uint8_t)ch,SRC_UI,i,0);
    }
    result(TXSEQ_OK,"已停止所选范围的发送；其他通道保持原状态");
}
void scope_handle(const app_evt_t *event)
{
    if(!event)return;
    if(event->type==EV_REQ_TX_STOP_ALL||event->type==EV_SYS_STOP_ALL)scope_cancel();
}
void scope_tick(void)
{
    if(status.state==SCOPE_IDLE)return;
    int rc=check(true);
    if(rc){status.state=SCOPE_IDLE;result(rc,NULL);return;}
    if(status.state==SCOPE_CONFIRMING)return;
    for(int ch=0;ch<CH_NUM;ch++)if(included(ch)&&!chan_can_tx(ch))return;
    (void)activate();
}
void scope_json(jw_t *j)
{
    jw_key(j,"tx_scope");jw_obj(j);jw_kint(j,"state",status.state);jw_kint(j,"mask",status.mask);jw_kint(j,"kind",status.kind);jw_kint(j,"entry",status.entry);jw_kint(j,"request",status.request);jw_kint(j,"error",status.error);jw_kstr(j,"message",status.message);jw_end_obj(j);
}
