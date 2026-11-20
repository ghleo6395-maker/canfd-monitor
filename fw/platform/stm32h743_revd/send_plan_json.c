/* SPDX-License-Identifier: MIT
 * Schema-specific streaming JSON codec. Reuses the shared send-plan validator
 * and jsonw encoder. No heap, file I/O, UI calls, or general JSON DOM.
 */
#include "send_plan_json.h"
#include "services/device/send_plan_validate.h"
#include "services/core/jsonw.h"
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

#ifdef CANFD_REVD_TARGET
#define CPU_WORK __attribute__((section(".revd_dtcm_bss"),aligned(8)))
#else
#define CPU_WORK
#endif
static struct {
    sp_json_read_fn read; void *context;
    uint8_t input[512]; size_t pos,used,total;
    int look,error; bool active;
    uint32_t line,column;char field[32];const char *reason;
    send_plan_t plan; char project[48];
} parser CPU_WORK;

static bool fail(int error) { if (!parser.error) parser.error=error; return false; }
static int peek(void)
{
    if(parser.look!=-2)return parser.look;
    if(parser.pos==parser.used){
        int n=parser.read(parser.context,parser.input,sizeof parser.input);
        if(n<0){fail(SP_ERR_IO);return parser.look=-1;}
        if((unsigned)n>sizeof parser.input){fail(SP_ERR_IO);return parser.look=-1;}
        parser.pos=0;parser.used=(size_t)n;parser.total+=(size_t)n;
        if(parser.total>SP_JSON_MAX_BYTES){fail(SP_ERR_FORMAT);return parser.look=-1;}
        if(!n)return parser.look=-1;
    }
    return parser.look=parser.input[parser.pos++];
}
static int take(void) {int value=peek();parser.look=-2;if(value=='\n'){parser.line++;parser.column=0;}else if(value>=0)parser.column++;return value;}
static void whitespace(void) {while(peek()==' '||peek()=='\n'||peek()=='\r'||peek()=='\t')take();}
static bool character(int expected) {whitespace();return take()==expected||fail(SP_ERR_FORMAT);}
static int nibble(int c)
{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static bool hex4(uint32_t *out)
{
    *out=0;
    for(unsigned i=0;i<4;i++){int digit=nibble(take());if(digit<0)return fail(SP_ERR_FORMAT);*out=(*out<<4)|(unsigned)digit;}
    return true;
}
static bool unicode_raw(int first,uint32_t *point)
{
    if(first<0)return fail(SP_ERR_FORMAT);
    unsigned trailing;uint32_t minimum;
    if(first<0x80){*point=(unsigned)first;return true;}
    if(first>=0xC2&&first<=0xDF){*point=(unsigned)first&31u;trailing=1;minimum=0x80;}
    else if(first>=0xE0&&first<=0xEF){*point=(unsigned)first&15u;trailing=2;minimum=0x800;}
    else if(first>=0xF0&&first<=0xF4){*point=(unsigned)first&7u;trailing=3;minimum=0x10000;}
    else return fail(SP_ERR_FORMAT);
    while(trailing--){int next=take();if(next<0x80||next>0xBF)return fail(SP_ERR_FORMAT);*point=(*point<<6)|((unsigned)next&63u);}
    return (*point>=minimum&&*point<=0x10FFFF&&!(*point>=0xD800&&*point<=0xDFFF))||fail(SP_ERR_FORMAT);
}
static size_t utf8(uint32_t p,char out[4])
{
    if(p<0x80){out[0]=(char)p;return 1;}
    if(p<0x800){out[0]=(char)(0xC0|(p>>6));out[1]=(char)(0x80|(p&63u));return 2;}
    if(p<0x10000){out[0]=(char)(0xE0|(p>>12));out[1]=(char)(0x80|((p>>6)&63u));out[2]=(char)(0x80|(p&63u));return 3;}
    out[0]=(char)(0xF0|(p>>18));out[1]=(char)(0x80|((p>>12)&63u));out[2]=(char)(0x80|((p>>6)&63u));out[3]=(char)(0x80|(p&63u));return 4;
}
static bool string(char *out,size_t capacity,size_t max_points)
{
    if(!character('"'))return false;
    size_t used=0,points=0;
    for(;;){
        int c=take();uint32_t point;
        if(c=='"'){if(out)out[used]=0;return true;}
        if(c<0x20)return fail(SP_ERR_FORMAT);
        if(c=='\\'){
            c=take();
            switch(c){
            case '"':case '\\':case '/':point=(unsigned)c;break;
            case 'b':point=8;break;case 'f':point=12;break;case 'n':point=10;break;case 'r':point=13;break;case 't':point=9;break;
            case 'u':
                if(!hex4(&point))return false;
                if(point>=0xD800&&point<=0xDBFF){uint32_t low;if(take()!='\\'||take()!='u'||!hex4(&low)||low<0xDC00||low>0xDFFF)return fail(SP_ERR_FORMAT);point=0x10000+((point-0xD800)<<10)+(low-0xDC00);}
                else if(point>=0xDC00&&point<=0xDFFF)return fail(SP_ERR_FORMAT);
                break;
            default:return fail(SP_ERR_FORMAT);
            }
        }else if(!unicode_raw(c,&point))return false;
        if(out&&!point)return fail(SP_ERR_FORMAT);
        char encoded[4];size_t n=utf8(point,encoded);
        if(++points>max_points||used+n>=capacity)return fail(SP_ERR_FORMAT);
        if(out)memcpy(out+used,encoded,n);
        used+=n;
    }
}
static bool number(uint32_t *out,uint32_t maximum)
{
    whitespace();int c=peek();
    if(c<'0'||c>'9')return fail(SP_ERR_FORMAT);
    uint32_t value=0;
    if(c=='0'){take();if(peek()>='0'&&peek()<='9')return fail(SP_ERR_FORMAT);}
    else while(peek()>='0'&&peek()<='9'){
        uint32_t digit=(unsigned)(take()-'0');
        if(value>maximum/10u||(value==maximum/10u&&digit>maximum%10u))return fail(SP_ERR_FORMAT);
        value=value*10u+digit;
    }
    if(peek()=='.'||peek()=='e'||peek()=='E')return fail(SP_ERR_FORMAT);
    *out=value;return true;
}
static bool boolean(uint32_t *out)
{
    whitespace();const char *text=peek()=='t'?"true":peek()=='f'?"false":NULL;
    if(!text)return fail(SP_ERR_FORMAT);
    *out=*text=='t';
    while(*text)if(take()!=*text++)return fail(SP_ERR_FORMAT);
    return true;
}
static int key_index(const char *key,const char *const *keys,unsigned count)
{for(unsigned i=0;i<count;i++)if(!strcmp(key,keys[i]))return (int)i;return -1;}
static bool member(uint32_t *seen,const char *const *keys,unsigned count,int *index)
{
    if(*seen&&!character(','))return false;
    char key[32];if(!string(key,sizeof key,31)||!character(':'))return false;
    memcpy(parser.field,key,strlen(key)+1u);
    *index=key_index(key,keys,count);
    if(*index<0||(*seen&(1u<<(unsigned)*index))){parser.reason="duplicate or unknown field";return fail(SP_ERR_FORMAT);}
    *seen|=1u<<(unsigned)*index;return true;
}
static bool dynamic(send_plan_entry_t *entry)
{
    static const char *const keys[]={"counter_byte","counter_bits","counter_max","counter_next","checksum","checksum_byte","checksum_from","checksum_to"};
    static const char *const checksums[]={"none","xor","sum","crc8-j1850","crc8-h2f"};
    uint32_t seen=0;if(!character('{'))return false;
    while((whitespace(),peek())!='}'){
        int key;if(!member(&seen,keys,8,&key))return false;
        if(key==4){char value[16];if(!string(value,sizeof value,15))return false;int checksum=key_index(value,checksums,5);if(checksum<0)return fail(SP_ERR_FORMAT);entry->dynamic[4]=(uint32_t)checksum;}
        else if(!number(&entry->dynamic[key],255))return false;
    }
    return (character('}')&&seen==255u)||fail(SP_ERR_FORMAT);
}
static bool payload(const char *text,send_plan_entry_t *entry)
{
    unsigned used=0;
    while(*text){
        if(strchr(" \t\r\n\v\f",*text)){text++;continue;}
        int hi=nibble((unsigned char)*text++);if(!*text)return fail(SP_ERR_FORMAT);
        int lo=nibble((unsigned char)*text++);if(hi<0||lo<0||used>=64)return fail(SP_ERR_FORMAT);
        entry->data[used++]=(uint8_t)((hi<<4)|lo);
    }
    return used==((entry->flags&CANF_RTR)?0u:entry->length)||fail(SP_ERR_FORMAT);
}
static bool entry_object(send_plan_entry_t *entry,unsigned slot)
{
    static const char *const keys[]={"slot","id","extended","fd","brs","rtr","length","data","period_ms","enabled","repeat_count","frame_gap_ms","cycle_ms","dynamic"};
    uint32_t seen=0;char data[193]={0};if(!character('{'))return false;
    while((whitespace(),peek())!='}'){
        int key;if(!member(&seen,keys,14,&key))return false;
        uint32_t value;
        switch(key){
        case 0:if(!number(&value,4)||value!=slot)return fail(SP_ERR_FORMAT);break;
        case 1:{char ident[11];if(!string(ident,sizeof ident,10))return false;const char *p=ident;if(p[0]=='0'&&(p[1]=='x'||p[1]=='X'))p+=2;unsigned digits=0;value=0;while(*p){int digit=nibble((unsigned char)*p++);if(digit<0||++digits>8)return fail(SP_ERR_FORMAT);value=(value<<4)|(unsigned)digit;}if(!digits)return fail(SP_ERR_FORMAT);entry->id=value;break;}
        case 2:case 3:case 4:case 5:
            if(!boolean(&value))return false;
            if(value)entry->flags|=key==2?CANF_EXT:key==3?CANF_FD:key==4?CANF_BRS:CANF_RTR;
            break;
        case 6:if(!number(&entry->length,64))return false;break;
        case 7:if(!string(data,sizeof data,192))return false;break;
        case 8:if(!number(&entry->period_ms,65535))return false;break;
        case 9:if(!boolean(&entry->enabled))return false;break;
        case 10:if(!number(&entry->repeat_count,10000))return false;break;
        case 11:if(!number(&entry->frame_gap_ms,3600000))return false;break;
        case 12:if(!number(&entry->cycle_ms,3600000))return false;break;
        case 13:if(!dynamic(entry))return false;break;
        default:return fail(SP_ERR_FORMAT);
        }
    }
    return (character('}')&&seen==((1u<<14)-1u)&&payload(data,entry))||fail(SP_ERR_FORMAT);
}
static bool channel_object(unsigned *channel)
{
    static const char *const keys[]={"channel","id_gap_ms","entries"};
    send_plan_channel_t value={0};uint32_t seen=0,ch=0;if(!character('{'))return false;
    while((whitespace(),peek())!='}'){
        int key;if(!member(&seen,keys,3,&key))return false;
        if(key==0){if(!number(&ch,2)||!ch)return fail(SP_ERR_FORMAT);}
        else if(key==1){if(!number(&value.id_gap_ms,3600000))return false;}
        else {
            if(!character('['))return false;
            for(unsigned i=0;i<4;i++){if(i&&!character(','))return false;if(!entry_object(&value.entries[i],i+1))return false;}
            if(!character(']'))return false;
        }
    }
    if(!character('}')||seen!=7)return fail(SP_ERR_FORMAT);
    parser.plan.channels[ch-1u]=value;*channel=ch;return true;
}
static bool project_valid(const char *project)
{
    size_t length=strlen(project);
    if(!length||length>47||project[0]==' '||project[length-1u]==' '||!strcmp(project,".")||!strcmp(project,".."))return false;
    for(size_t i=0;i<length;){
        unsigned first=(unsigned char)project[i++],n=0;uint32_t point=first,minimum=0;
        if(first>=0xC2&&first<=0xDF){point=first&31u;n=1;minimum=0x80;}
        else if(first>=0xE0&&first<=0xEF){point=first&15u;n=2;minimum=0x800;}
        else if(first>=0xF0&&first<=0xF4){point=first&7u;n=3;minimum=0x10000;}
        else if(first>=0x80)return false;
        if(i+n>length)return false;
        while(n--){unsigned next=(unsigned char)project[i++];if(next<0x80||next>0xBF)return false;point=(point<<6)|(next&63u);}
        if(point<minimum||point>0x10FFFF||(point>=0xD800&&point<=0xDFFF)||point<32||(point>=127&&point<=159)||point=='/'||point=='\\')return false;
    }
    return true;
}
static bool plan_object(void)
{
    static const char *const keys[]={"format","version","channel_mask","channels","description","project"};
    uint32_t seen=0,actual_mask=0;if(!character('{'))return false;
    while((whitespace(),peek())!='}'){
        int key;if(!member(&seen,keys,6,&key))return false;
        uint32_t value;
        switch(key){
        case 0:{char format[32];if(!string(format,sizeof format,31)||strcmp(format,"canfd-send-plan"))return fail(SP_ERR_FORMAT);break;}
        case 1:if(!number(&value,1)||value!=1)return fail(SP_ERR_FORMAT);break;
        case 2:if(!number(&parser.plan.channel_mask,3)||!parser.plan.channel_mask)return fail(SP_ERR_FORMAT);break;
        case 3:{
            if(!character('['))return false;
            unsigned previous=0,count=0;
            while((whitespace(),peek())!=']'){
                if(count&&!character(','))return false;
                unsigned channel;if(++count>2||!channel_object(&channel)||channel<=previous)return fail(SP_ERR_FORMAT);
                previous=channel;actual_mask|=1u<<(channel-1u);
            }
            if(!count||!character(']'))return fail(SP_ERR_FORMAT);
            break;
        }
        case 4:if(!string(NULL,1025,256))return false;break;
        case 5:if(!string(parser.project,sizeof parser.project,47)||!project_valid(parser.project))return fail(SP_ERR_FORMAT);break;
        default:return fail(SP_ERR_FORMAT);
        }
    }
    return (character('}')&&(seen&15u)==15u&&actual_mask==parser.plan.channel_mask)||fail(SP_ERR_FORMAT);
}
int sp_json_decode(sp_json_read_fn read,void *context,send_plan_t *out,char project[48],char error[96])
{
    if(!read||!out||parser.active)return SP_ERR_ARGUMENT;
    memset(&parser,0,sizeof parser);parser.active=true;parser.read=read;parser.context=context;parser.look=-2;parser.line=1;
    if(peek()==0xEF){take();if(take()!=0xBB||take()!=0xBF)fail(SP_ERR_FORMAT);}
    if(!parser.error&&!plan_object())fail(SP_ERR_FORMAT);
    whitespace();if(peek()!=-1)fail(SP_ERR_FORMAT);
    if(!parser.error&&sp_validate(&parser.plan)!=SP_OK){parser.reason="CAN frame, dynamic byte or cycle range";fail(SP_ERR_FORMAT);}
    int result=parser.error;
    if(!result){*out=parser.plan;if(project)memcpy(project,parser.project,48);}
    if(error){if(result)snprintf(error,96,"line %lu col %lu %.31s: %s",(unsigned long)parser.line,(unsigned long)parser.column,parser.field,
        result==SP_ERR_IO?"SD read failed":parser.reason?parser.reason:"invalid JSON / UTF-8 / value");else error[0]=0;}
    parser.active=false;return result;
}

int sp_json_safe_name(const char *name,size_t maximum,int allow_empty)
{return sp_safe_name(name,maximum,allow_empty);}
int sp_json_filename(const char *name)
{return sp_filename_validate(name);}
int sp_json_encode(const send_plan_t *plan,const char *project,char *buffer,size_t capacity,size_t *length)
{
    if(length)*length=0;
    if(!plan||!buffer||!capacity||sp_validate(plan)!=SP_OK||(project&&project[0]&&!project_valid(project)))return SP_ERR_FORMAT;
    jw_t j;jw_init(&j,buffer,capacity);jw_obj(&j);jw_kstr(&j,"format","canfd-send-plan");jw_kint(&j,"version",1);jw_kint(&j,"channel_mask",plan->channel_mask);
    if(project&&project[0])jw_kstr(&j,"project",project);
    jw_key(&j,"channels");jw_arr(&j);
    static const char *const dyn[]={"counter_byte","counter_bits","counter_max","counter_next","checksum","checksum_byte","checksum_from","checksum_to"};
    static const char *const checksums[]={"none","xor","sum","crc8-j1850","crc8-h2f"};
    for(unsigned ch=0;ch<2;ch++)if(plan->channel_mask&(1u<<ch)){
        jw_obj(&j);jw_kint(&j,"channel",ch+1);jw_kint(&j,"id_gap_ms",plan->channels[ch].id_gap_ms);jw_key(&j,"entries");jw_arr(&j);
        for(unsigned i=0;i<4;i++){
            const send_plan_entry_t *e=&plan->channels[ch].entries[i];char ident[9],data[193]={0};
            snprintf(ident,sizeof ident,(e->flags&CANF_EXT)?"%08lX":"%03lX",(unsigned long)e->id);
            if(!(e->flags&CANF_RTR))for(unsigned k=0;k<e->length;k++)snprintf(data+k*3,sizeof data-k*3,"%02X%s",e->data[k],k+1<e->length?" ":"");
            jw_obj(&j);jw_kint(&j,"slot",i+1);jw_kstr(&j,"id",ident);jw_kbool(&j,"extended",(e->flags&CANF_EXT)!=0);jw_kbool(&j,"fd",(e->flags&CANF_FD)!=0);jw_kbool(&j,"brs",(e->flags&CANF_BRS)!=0);jw_kbool(&j,"rtr",(e->flags&CANF_RTR)!=0);
            jw_kint(&j,"length",e->length);jw_kstr(&j,"data",data);jw_kint(&j,"period_ms",e->period_ms);jw_kbool(&j,"enabled",e->enabled!=0);jw_kint(&j,"repeat_count",e->repeat_count);jw_kint(&j,"frame_gap_ms",e->frame_gap_ms);jw_kint(&j,"cycle_ms",e->cycle_ms);
            jw_key(&j,"dynamic");jw_obj(&j);for(unsigned k=0;k<8;k++){if(k==4)jw_kstr(&j,dyn[k],checksums[e->dynamic[k]]);else jw_kint(&j,dyn[k],e->dynamic[k]);}jw_end_obj(&j);jw_end_obj(&j);
        }
        jw_end_arr(&j);jw_end_obj(&j);
    }
    jw_end_arr(&j);jw_end_obj(&j);size_t n=jw_finish(&j);
    if(j.overflow){buffer[0]=0;return SP_ERR_IO;}
    if(length)*length=n;
    return SP_OK;
}
