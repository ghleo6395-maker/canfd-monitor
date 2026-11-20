/* Side-effect-free decoding; neither UI nor hardware state is mutated. */
#include "services/device/send_plan_validate.h"
#include "sim/tx_sequence.h"
#include "services/device/file_port.h"
#include <string.h>

static bool timing_valid(const send_plan_entry_t *e)
{
    return e->enabled<=1&&e->repeat_count>=1&&e->repeat_count<=TXSEQ_MAX_COUNT&&e->frame_gap_ms<=TXSEQ_MAX_TIME_MS&&e->cycle_ms>=1&&e->cycle_ms<=TXSEQ_MAX_TIME_MS&&
        (e->repeat_count==1||e->frame_gap_ms>=1)&&e->cycle_ms>(uint64_t)(e->repeat_count-1)*e->frame_gap_ms;
}
int sp_entry_unpack(int ch,const send_plan_entry_t *source,tx_entry_t *out)
{
    if(source->flags>UINT8_MAX||source->length>64||source->period_ms>UINT16_MAX||!timing_valid(source))return SP_ERR_FORMAT;
    for(int k=0;k<8;k++)if(source->dynamic[k]>(k==4?CHK_CRC8_H2F:UINT8_MAX))return SP_ERR_FORMAT;
    memset(out,0,sizeof *out);out->f=(can_frame_t){.id=source->id,.ch=(uint8_t)ch,.flags=(uint8_t)source->flags,.len=(uint8_t)source->length};
    if(tx_validate_frame(ch,&out->f,false)!=TX_OK)return SP_ERR_FORMAT;
    if(source->flags&CANF_RTR){for(int b=0;b<64;b++)if(source->data[b])return SP_ERR_FORMAT;}
    else {memcpy(out->f.data,source->data,source->length);for(unsigned b=source->length;b<64;b++)if(source->data[b])return SP_ERR_FORMAT;}
    out->period_ms=(uint16_t)source->period_ms;
    out->dyn=(dynfield_t){.cnt_byte=(uint8_t)source->dynamic[0],.cnt_bits=(uint8_t)source->dynamic[1],.cnt_max=(uint8_t)source->dynamic[2],.cnt_val=(uint8_t)source->dynamic[3],.chk=(chk_type_t)source->dynamic[4],.chk_byte=(uint8_t)source->dynamic[5],.chk_from=(uint8_t)source->dynamic[6],.chk_to=(uint8_t)source->dynamic[7]};
    return tx_validate_dynamic(&out->dyn,&out->f)==TX_OK?SP_OK:SP_ERR_FORMAT;
}
int sp_validate(const send_plan_t *plan)
{
    if(!plan||!plan->channel_mask||(plan->channel_mask&~3u))return SP_ERR_FORMAT;
    for(int ch=0;ch<2;ch++)if(plan->channel_mask&(1u<<ch)){
        if(plan->channels[ch].id_gap_ms>TXSEQ_MAX_TIME_MS)return SP_ERR_FORMAT;
        tx_entry_t scratch;for(int i=0;i<4;i++){int rc=sp_entry_unpack(ch,&plan->channels[ch].entries[i],&scratch);if(rc)return rc;}
    }
    return SP_OK;
}

/* One filename contract for desktop saves and target SD operations. */
static unsigned lower(unsigned c){return c>='A'&&c<='Z'?c+32:c;}
static bool equal_case(const char *a,const char *b)
{while(*a&&*b)if(lower((unsigned char)*a++)!=lower((unsigned char)*b++))return false;return *a==*b;}
int sp_safe_name(const char *name,size_t maximum,int allow_empty)
{
    if(!name)return FILE_PORT_ERR_NAME;
    size_t length=0;while(length<=maximum&&name[length])length++;
    if(length>maximum)return FILE_PORT_ERR_LIMIT;
    if(!length)return allow_empty?FILE_PORT_OK:FILE_PORT_ERR_NAME;
    if(name[0]=='.'||name[0]==' '||name[length-1u]=='.'||name[length-1u]==' ')return FILE_PORT_ERR_NAME;
    size_t at=0;
    while(at<length){
        unsigned first=(unsigned char)name[at++],n=0;uint32_t point=first,min=0;
        if(first>=0xC2&&first<=0xDF){point=first&31u;n=1;min=0x80;}
        else if(first>=0xE0&&first<=0xEF){point=first&15u;n=2;min=0x800;}
        else if(first>=0xF0&&first<=0xF4){point=first&7u;n=3;min=0x10000;}
        else if(first>=0x80)return FILE_PORT_ERR_NAME;
        if(at+n>length)return FILE_PORT_ERR_NAME;
        while(n--){unsigned next=(unsigned char)name[at++];if(next<0x80||next>0xBF)return FILE_PORT_ERR_NAME;point=(point<<6)|(next&63u);}
        if(point<min||point>0x10FFFF||(point>=0xD800&&point<=0xDFFF)||point<32||(point>=127&&point<=159)||
           (point<128&&strchr("/\\:*?\"<>|",(int)point))||(point>=0x200B&&point<=0x200F)||(point>=0x2028&&point<=0x202E)||(point>=0x2066&&point<=0x2069)||point==0xFEFF)return FILE_PORT_ERR_NAME;
    }
    char base[5]={0};size_t count=0;while(count<length&&name[count]!='.')count++;
    while(count&&name[count-1u]==' ')count--;
    if(count<=4){for(size_t i=0;i<count;i++)base[i]=(char)lower((unsigned char)name[i]);
        if(!strcmp(base,"con")||!strcmp(base,"prn")||!strcmp(base,"aux")||!strcmp(base,"nul")||(count==4&&(!strncmp(base,"com",3)||!strncmp(base,"lpt",3))&&base[3]>='1'&&base[3]<='9'))return FILE_PORT_ERR_NAME;}
    return FILE_PORT_OK;
}
int sp_filename_validate(const char *name)
{
    if(sp_safe_name(name,SP_NAME_BYTES,0))return SP_ERR_NAME;
    size_t length=strlen(name);const char *suffix=".canplan.json";size_t tail=strlen(suffix);
    return length>tail&&equal_case(name+length-tail,suffix)?SP_OK:SP_ERR_NAME;
}
