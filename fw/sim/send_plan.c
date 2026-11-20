#include "sim/send_plan.h"
#include "sim/tx_sequence.h"
#include "services/device/send_plan_validate.h"
#include "services/tx_sched/tx_sm.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "app/ui/core/ui_sm.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

_Static_assert(sizeof(send_plan_entry_t)==128,"send-plan entry wire layout");
_Static_assert(sizeof(send_plan_channel_t)==516,"send-plan channel wire layout");
_Static_assert(sizeof(send_plan_t)==1036,"send-plan wire layout");
_Static_assert(sizeof(sp_file_t)==1136,"send-plan file wire layout");
_Static_assert(sizeof(sp_io_request_t)==1092,"send-plan IO wire layout");
static sp_file_t files[SP_MAX_FILES];
static unsigned file_count;
static uint32_t token_serial,io_serial,request_serial,name_serial;
static sp_io_request_t io;
static sp_status_t status;
static send_plan_t loaded;
static uint32_t loaded_token;
static struct {uint32_t tx[2][4],seq[2],boot;} binding;

static void message(int error,const char *text)
{
    status.last_error=error;
    snprintf(status.message,sizeof status.message,"%s",text?text:sp_result_text(error));
}
const char *sp_result_text(int rc)
{
    switch(rc){
    case SP_OK:return "已完成";case SP_WAIT_CONFIRM:return "请在设备屏幕确认方案发送";
    case SP_ERR_ARGUMENT:return "发送方案参数无效";case SP_ERR_FORMAT:return "发送方案内容不完整或参数不合法";
    case SP_ERR_BUSY:return "所含通道仍在发送、回放或有待发帧，请先停止并等待完成";
    case SP_ERR_CHANGED:return "文件或发送配置已变化，请重新选择并加载方案";
    case SP_ERR_POWER:return "请先开机、唤醒并解除按键锁";
    case SP_ERR_FILE:return "文件不存在、目录已满或文件名已存在";
    case SP_ERR_NAME:return "名称需为有效UTF-8且不含路径，以.canplan.json结尾，最多47字节";
    case SP_ERR_IO:return "方案文件读写失败，原文件保留";case SP_ERR_EMPTY:return "没有已加载或已启用的发送方案";
    case SP_ERR_MODE:return "方案所含通道当前不能发送，请检查模式和总线设置";
    case SP_ERR_FRAME:return "方案报文与当前通道能力不匹配";case SP_ERR_LOAD:return "方案估算发送负载超过100%";
    default:return "发送方案操作未完成";
    }
}
static bool same_name(const char *a,const char *b)
{
    while(*a&&*b){unsigned x=(unsigned char)*a++,y=(unsigned char)*b++;if(x<128)x=(unsigned)tolower((int)x);if(y<128)y=(unsigned)tolower((int)y);if(x!=y)return false;}return *a==*b;
}
static bool valid_utf8(const unsigned char *s)
{
    while(*s){uint32_t cp;unsigned n;if(*s<0x80){s++;continue;}if(*s>=0xC2&&*s<=0xDF){cp=*s&31u;n=1;}else if(*s>=0xE0&&*s<=0xEF){cp=*s&15u;n=2;}else if(*s>=0xF0&&*s<=0xF4){cp=*s&7u;n=3;}else return false;s++;for(unsigned i=0;i<n;i++){if(!*s||(*s&0xC0)!=0x80)return false;cp=(cp<<6)|(*s++&63u);}if((n==1&&cp<0x80)||(n==2&&cp<0x800)||(n==3&&cp<0x10000)||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF))return false;}return true;
}
static bool valid_name(const char *name)
{return sp_filename_validate(name)==SP_OK;}
/* Project is a display label, never a directory. Empty legacy wire labels
 * normalize to the old-file group below. JSON rejects an explicit empty label. */
static const char *project_name(const sp_file_t *file){return file->project[0]?file->project:"未分组";}
static bool valid_project(const char *name)
{
    size_t n=0;while(n<=SP_PROJECT_BYTES&&name[n])n++;
    if(!n||n>SP_PROJECT_BYTES||name[0]==' '||name[n-1]==' '||!strcmp(name,".")||!strcmp(name,"..")||!valid_utf8((const unsigned char*)name))return false;
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)name[i];
        if(c<32||c==127||c=='/'||c=='\\'||(c==0xC2&&i+1<n&&(unsigned char)name[i+1]>=0x80&&(unsigned char)name[i+1]<=0x9F))return false;
    }
    return true;
}
void sp_init(void)
{
    memset(files,0,sizeof files);file_count=0;memset(&io,0,sizeof io);memset(&status,0,sizeof status);memset(&loaded,0,sizeof loaded);memset(&binding,0,sizeof binding);loaded_token=0;
}
void sp_reset_runtime(void)
{
    /* File I/O results describe retained storage, not the running send owner.
     * Keep them visible across OFF->BOOT, including startup scan failures. */
    status.loaded_mask=0;status.loaded_name[0]=0;status.state=SP_IDLE;
    status.request=0;status.run_once=0;
    status.io_pending=io.kind?1u:0u;status.io_kind=io.kind;status.io_id=io.id;
    memset(&loaded,0,sizeof loaded);
    memset(&binding,0,sizeof binding);loaded_token=0;
}
int sp_capture(uint32_t mask,send_plan_t *out)
{
    if(!out||!mask||(mask&~3u))return SP_ERR_ARGUMENT;
    if(!power_running()||power_get()->locked)return SP_ERR_POWER;
    memset(out,0,sizeof *out);out->channel_mask=mask;
    for(int ch=0;ch<2;ch++)if(mask&(1u<<ch)){
        const txseq_channel_t *q=txseq_get(ch);out->channels[ch].id_gap_ms=q->id_gap_ms;
        for(int i=0;i<4;i++){
            const tx_entry_t *e=&tx_get()->e[ch][i];send_plan_entry_t *row=&out->channels[ch].entries[i];
            row->id=e->f.id;row->flags=e->f.flags;row->length=e->f.len;row->period_ms=e->period_ms;
            if(!(e->f.flags&CANF_RTR))memcpy(row->data,e->f.data,e->f.len);
            row->dynamic[0]=e->dyn.cnt_byte;row->dynamic[1]=e->dyn.cnt_bits;row->dynamic[2]=e->dyn.cnt_max;row->dynamic[3]=e->dyn.cnt_val;row->dynamic[4]=e->dyn.chk;row->dynamic[5]=e->dyn.chk_byte;row->dynamic[6]=e->dyn.chk_from;row->dynamic[7]=e->dyn.chk_to;
            row->enabled=q->entries[i].enabled;row->repeat_count=q->entries[i].repeat_count;row->frame_gap_ms=q->entries[i].frame_gap_ms;row->cycle_ms=q->entries[i].cycle_ms;
        }
    }
    return sp_validate(out);
}
void sp_default_name(uint32_t mask,char *out,uint32_t capacity)
{
    if(!out||!capacity)return;
    bool found;
    do{if(!++name_serial)name_serial=1;snprintf(out,capacity,"T_%s_%03lu.canplan.json",mask==3?"BOTH":mask==2?"CAN2":"CAN1",(unsigned long)name_serial);found=false;for(unsigned i=0;i<file_count;i++)if(same_name(files[i].name,out))found=true;}while(found);
}
int sp_request_save(const char *name,const send_plan_t *snapshot)
{
    if(io.kind)return SP_ERR_BUSY;
    if(!valid_name(name))return SP_ERR_NAME;
    int rc=sp_validate(snapshot);if(rc)return rc;
    for(unsigned i=0;i<file_count;i++)if(same_name(files[i].name,name))return SP_ERR_FILE;
    if(file_count>=SP_MAX_FILES)return SP_ERR_FILE;
    memset(&io,0,sizeof io);io.kind=SP_IO_SAVE;io.id=++io_serial;if(!io.id)io.id=++io_serial;
    snprintf(io.name,sizeof io.name,"%s",name);io.plan=*snapshot;status.io_pending=1;status.io_kind=io.kind;status.io_id=io.id;
    message(0,"正在把发送方案保存到SD文件…");return SP_OK;
}
int sp_request_refresh(void)
{
    if(io.kind)return SP_ERR_BUSY;
    memset(&io,0,sizeof io);io.kind=SP_IO_REFRESH;io.id=++io_serial;if(!io.id)io.id=++io_serial;status.io_pending=1;status.io_kind=io.kind;status.io_id=io.id;message(0,"正在刷新SD发送方案目录…");return 0;
}
bool sp_io_peek(sp_io_request_t *out){if(!out||!io.kind)return false;*out=io;return true;}
int sp_io_complete(uint32_t id,int result,const char *text)
{
    if(!io.kind||io.id!=id)return SP_ERR_CHANGED;
    memset(&io,0,sizeof io);status.io_pending=status.io_kind=status.io_id=0;message(result,text&&*text?text:result?sp_result_text(result):"发送方案文件操作完成");return result;
}
unsigned sp_catalog_count(void){return file_count;}
bool sp_catalog_get(unsigned index,sp_file_t *out){if(index>=file_count||!out)return false;*out=files[index];return true;}
int sp_catalog_replace(const sp_file_t *source,uint32_t count)
{
    if(count>SP_MAX_FILES||(!source&&count))return SP_ERR_FILE;
    for(uint32_t i=0;i<count;i++){
        if(!valid_name(source[i].name)||!valid_project(project_name(&source[i]))||sp_validate(&source[i].plan))return SP_ERR_FORMAT;
        for(uint32_t k=0;k<i;k++)if(same_name(source[k].name,source[i].name))return SP_ERR_NAME;
    }
    /* Precompute all stable identities before touching the old catalogue. A
     * second full 18 KiB catalogue on the stack would exceed the MCU stack. */
    uint32_t tokens[SP_MAX_FILES]={0};
    for(uint32_t i=0;i<count;i++){
        for(unsigned k=0;k<file_count;k++)if(same_name(files[k].name,source[i].name)&&!strcmp(files[k].project,project_name(&source[i]))&&!memcmp(&files[k].plan,&source[i].plan,sizeof(send_plan_t)))tokens[i]=files[k].token;
        if(!tokens[i]){tokens[i]=++token_serial;if(!tokens[i])tokens[i]=++token_serial;}
    }
    if(count)memmove(files,source,count*sizeof *files);
    for(uint32_t i=0;i<count;i++){
        if(!files[i].project[0])snprintf(files[i].project,sizeof files[i].project,"%s","未分组");
        files[i].token=tokens[i];
    }
    memset(files+count,0,(SP_MAX_FILES-count)*sizeof *files);file_count=count;return 0;
}
static bool ready(void){return power_running()&&power_get()->screen_on&&!power_get()->locked;}
static bool channels_idle(uint32_t mask)
{
    for(int ch=0;ch<2;ch++)if(mask&(1u<<ch)){
        if(tx_get()->pending[ch]||play_get(ch)->state!=PL_IDLE||txseq_get(ch)->state!=TXSEQ_IDLE)return false;
        for(int i=0;i<4;i++)if(tx_get()->e[ch][i].active)return false;
    }
    return true;
}
static void bind_loaded(void)
{
    binding.boot=power_get()->boot_count;
    for(int ch=0;ch<2;ch++)if(loaded.channel_mask&(1u<<ch)){
        binding.seq[ch]=txseq_get(ch)->revision;for(int i=0;i<4;i++)binding.tx[ch][i]=tx_get()->e[ch][i].revision;
    }
}
static bool unchanged(void)
{
    if(binding.boot!=power_get()->boot_count)return false;
    for(int ch=0;ch<2;ch++)if(loaded.channel_mask&(1u<<ch)){
        if(binding.seq[ch]!=txseq_get(ch)->revision)return false;
        for(int i=0;i<4;i++)if(binding.tx[ch][i]!=tx_get()->e[ch][i].revision)return false;
    }
    return true;
}
static bool source_current(void)
{
    for(unsigned i=0;i<file_count;i++)if(files[i].token==loaded_token&&same_name(files[i].name,status.loaded_name))return true;
    return false;
}
static int apply(const send_plan_t *plan)
{
    int rc=sp_validate(plan);if(rc)return rc;
    if(!channels_idle(plan->channel_mask))return SP_ERR_BUSY;
    tx_entry_t entries[2][4];memset(entries,0,sizeof entries);
    txseq_entry_config_t timings[2][4];memset(timings,0,sizeof timings);
    for(int ch=0;ch<2;ch++)if(plan->channel_mask&(1u<<ch))for(int i=0;i<4;i++){
        const send_plan_entry_t *row=&plan->channels[ch].entries[i];rc=sp_entry_unpack(ch,row,&entries[ch][i]);if(rc)return rc;
        timings[ch][i]=(txseq_entry_config_t){row->enabled!=0,row->repeat_count,row->frame_gap_ms,row->cycle_ms};
    }
    /* Every operation below is already validated and no event pumping occurs.
     * The caller holds the simulator lock throughout the whole transaction. */
    if(tx_restore_entries((uint8_t)plan->channel_mask,entries)!=TX_OK)return SP_ERR_BUSY;
    for(int ch=0;ch<2;ch++)if(plan->channel_mask&(1u<<ch)){
        rc=txseq_configure(ch,timings[ch],plan->channels[ch].id_gap_ms,txseq_get(ch)->revision);
        if(rc)return SP_ERR_FORMAT; /* unreachable after prevalidation */
    }
    return 0;
}
int sp_load(unsigned index,uint32_t token)
{
    if(!ready()||ui_dialog_open()||ui_draft_open())return SP_ERR_POWER;
    /* One plan owns its mask until stopped; loading a disjoint file must not
     * orphan a still-running previous plan that sp_stop could no longer reach. */
    if(status.state==SP_RUNNING||status.state==SP_CONFIRMING||status.state==SP_ARMING)return SP_ERR_BUSY;
    if(index>=file_count||!token||files[index].token!=token)return SP_ERR_CHANGED;
    int rc=apply(&files[index].plan);if(rc){message(rc,NULL);return rc;}
    loaded=files[index].plan;loaded_token=files[index].token;status.loaded_mask=loaded.channel_mask;
    snprintf(status.loaded_name,sizeof status.loaded_name,"%s",files[index].name);status.state=SP_LOADED;bind_loaded();message(0,"方案已加载到发送列表；尚未启动发送");return 0;
}
static int start_check(void)
{
    if(!ready())return SP_ERR_POWER;
    if(!status.loaded_mask)return SP_ERR_EMPTY;
    if(!source_current()||!unchanged())return SP_ERR_CHANGED;
    if(!channels_idle(loaded.channel_mask))return SP_ERR_BUSY;
    for(int ch=0;ch<2;ch++)if(loaded.channel_mask&(1u<<ch)){
        if(!chan_rate_allows_tx(ch)||chan_get(ch)->state==CH_BUSOFF||chan_get(ch)->state==CH_STANDBY||chan_get(ch)->state==CH_AUTOBAUD)return SP_ERR_MODE;
        bool enabled=false;for(int i=0;i<4;i++)if(loaded.channels[ch].entries[i].enabled){enabled=true;if(tx_validate_frame(ch,&tx_get()->e[ch][i].f,true)!=TX_OK)return SP_ERR_FRAME;}
        if(!enabled)return SP_ERR_EMPTY;
        if(txseq_load_permille(ch)>1000)return SP_ERR_LOAD;
    }
    return 0;
}
static int activate_all(void)
{
    for(int ch=0;ch<2;ch++)if(loaded.channel_mask&(1u<<ch)){
        chan_clear_first_tx(ch);int rc=txseq_start_rounds(ch,txseq_get(ch)->revision,status.run_once?1:0);
        if(rc){for(int done=0;done<2;done++)if(loaded.channel_mask&(1u<<done))txseq_stop(done);message(SP_ERR_MODE,"方案启动中断；所含通道已停止");status.state=SP_LOADED;return SP_ERR_MODE;}
    }
    status.state=SP_RUNNING;message(0,"发送方案正在执行；时序与数据复用手动发送路径");return 0;
}
static int start_mode(bool once)
{
    if(ui_dialog_open()||ui_draft_open())return SP_ERR_BUSY;
    int rc=start_check();if(rc){message(rc,NULL);return rc;}
    /* Start repeats from the saved dynamic-counter initial state every time. */
    rc=apply(&loaded);if(rc){message(rc,NULL);return rc;}bind_loaded();
    status.run_once=once?1u:0u;
    bool confirm=false;for(int ch=0;ch<2;ch++)if(loaded.channel_mask&(1u<<ch))if(!chan_can_tx(ch)||chan_get(ch)->first_tx_pending)confirm=true;
    if(confirm){status.request=++request_serial;if(!status.request)status.request=++request_serial;status.state=SP_CONFIRMING;evt_ask(ASK_PLAN_START,0,(int32_t)status.request,SRC_UI);message(0,"请确认方案所含通道的模式与首次发送权限");return SP_WAIT_CONFIRM;}
    return activate_all();
}
int sp_start(void){return start_mode(false);}
int sp_start_once(void){return start_mode(true);}
int sp_answer(uint32_t request,bool allow)
{
    if(status.state!=SP_CONFIRMING||status.request!=request)return SP_ERR_CHANGED;
    if(!allow){status.state=SP_LOADED;message(0,"已取消方案启动；未发送任何报文");return 0;}
    int rc=start_check();if(rc){status.state=SP_LOADED;message(rc,NULL);return rc;}
    for(int ch=0;ch<2;ch++)if(loaded.channel_mask&(1u<<ch))if(!chan_can_tx(ch)){
        if(!evt_post4(EV_REQ_CH_MODE,(uint8_t)ch,SRC_UI,CAN_MODE_NORMAL,2)){status.state=SP_LOADED;message(SP_ERR_MODE,NULL);return SP_ERR_MODE;}
    }
    status.state=SP_ARMING;message(0,"正在为方案所含通道设置工作模式…");return 0;
}
void sp_stop(void)
{
    for(int ch=0;ch<2;ch++)if(status.loaded_mask&(1u<<ch)){
        txseq_stop(ch);for(int i=0;i<4;i++)evt_post4(EV_REQ_TX_CYC,(uint8_t)ch,SRC_UI,i,0);
    }
    status.state=status.loaded_mask?SP_LOADED:SP_IDLE;message(0,"方案所含通道已停止；未包含的通道保持原状态");
}
void sp_handle(const app_evt_t *event)
{
    if(!event||(event->type!=EV_REQ_TX_STOP_ALL&&event->type!=EV_SYS_STOP_ALL))return;
    if(status.state==SP_CONFIRMING||status.state==SP_ARMING||status.state==SP_RUNNING){
        status.state=status.loaded_mask?SP_LOADED:SP_IDLE;
        message(0,"全局停止已取消方案启动或运行；文件及已加载配置保留");
    }
}
void sp_tick(void)
{
    if(status.state!=SP_ARMING&&status.state!=SP_CONFIRMING&&status.state!=SP_RUNNING)return;
    if(!power_running()||binding.boot!=power_get()->boot_count){sp_stop();message(SP_ERR_POWER,"设备已停止，方案文件保留；开机后重新加载");return;}
    if(status.state==SP_CONFIRMING)return;
    if(status.state==SP_ARMING){
        int rc=start_check();if(rc){status.state=SP_LOADED;message(rc,NULL);return;}
        for(int ch=0;ch<2;ch++)if(loaded.channel_mask&(1u<<ch))if(!chan_can_tx(ch))return;
        (void)activate_all();return;
    }
    bool active=false;for(int ch=0;ch<2;ch++)if(loaded.channel_mask&(1u<<ch)){
        const txseq_channel_t *q=txseq_get(ch);if(q->state!=TXSEQ_IDLE)active=true;
        else if(q->stop_reason!=TXSEQ_STOP_USER&&q->stop_reason!=TXSEQ_STOP_NONE&&q->stop_reason!=TXSEQ_STOP_COMPLETE){sp_stop();message(SP_ERR_MODE,"方案因通道或发送故障停止，请检查总线状态");return;}
    }
    if(!active){status.state=SP_LOADED;message(0,status.run_once?"整屏方案的一轮已结束；请核对发送与跳过计数":"方案发送已停止，可重新开始或加载其他方案");}
}
const sp_status_t *sp_state(void){return &status;}
const send_plan_t *sp_loaded_plan(void){return status.loaded_mask?&loaded:NULL;}
void sp_json(jw_t *j)
{
    jw_key(j,"send_plans");jw_obj(j);jw_kint(j,"state",status.state);jw_kint(j,"loaded_mask",status.loaded_mask);jw_kstr(j,"loaded_name",status.loaded_name);jw_kint(j,"request",status.request);jw_kbool(j,"run_once",status.run_once!=0);jw_kint(j,"error",status.last_error);jw_kstr(j,"message",status.message);jw_kbool(j,"io_pending",status.io_pending!=0);jw_kint(j,"io_kind",status.io_kind);jw_key(j,"files");jw_arr(j);
    for(unsigned i=0;i<file_count;i++){jw_obj(j);jw_kint(j,"index",i);jw_kint(j,"token",files[i].token);jw_kstr(j,"name",files[i].name);jw_kstr(j,"project",files[i].project);jw_kint(j,"channel_mask",files[i].plan.channel_mask);jw_end_obj(j);}jw_end_arr(j);jw_end_obj(j);
}
