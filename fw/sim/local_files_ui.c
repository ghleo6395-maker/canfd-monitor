#include "sim/local_files_ui.h"
#include "services/device/file_port.h"
#include "services/core/port.h"
#include "services/core/app_timer.h"
#include "services/input/keys.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "app/ui/core/ui_sm.h"

#include <stdio.h>
#include <string.h>

#define ACTION 0x10u
#define DISABLED 0x02u
#define ACTIVE 0x08u
#define NOTICE_BYTES 192u
#define KEYS_COLUMNS 10
#define BACKSPACE_DELAY_MS 400u
#define BACKSPACE_PERIOD_MS 90u
#define BACKSPACE_LEASE_MS 1500u
_Static_assert(sizeof(local_db_frame_t) == 80, "local database FFI layout changed");
#ifdef CANFD_REVD_TARGET
/* Real devices accept the PC-exported .canplan.json format. Do not reserve the
 * simulator's 4 x 256 database rows (80 KiB) in target SRAM. The one-row stub
 * keeps the dormant preview code bounded without allocating an import store. */
#define DB_FILE_SLOTS 1u
#define DB_FRAME_SLOTS 1u
#else
#define DB_FILE_SLOTS LOCAL_DB_MAX_FILES
#define DB_FRAME_SLOTS LOCAL_DB_MAX_FRAMES
#endif

typedef struct {
    bool used;
    char name[LOCAL_DB_NAME_BYTES + 1u];
    uint32_t count, token;
    local_db_frame_t frames[DB_FRAME_SLOTS];
} database_t;
static database_t databases[DB_FILE_SLOTS];
static uint32_t catalogue_token,keyboard_serial;
typedef enum { OFF, DATABASES, FRAMES, FRAME, PREFIX, RENAME, FILES, TEXT } view_t;
static struct {
    view_t view;
    int cursor, ch, slot, db, frame, file_index;
    uint32_t db_token, target_revision, file_token;
    local_db_frame_t preview;
    char text[LOCAL_INPUT_MAX_BYTES + 1u];
    char notice[NOTICE_BYTES];
    bool error,record_save;
    uint32_t record_session,keyboard_token,backspace_next,backspace_lease;
    bool backspace_hold,backspace_physical;
    struct { int index; uint32_t token; char name[FILE_PORT_NAME_MAX_BYTES + 1u]; } logs[24];
    int log_count;
    char text_title[48];
    unsigned text_maximum;
    local_input_mode_t input_mode;
    local_text_save_t text_save;
    local_text_cancel_t text_cancel;
    void *text_context;
} U;

static bool ready(void)
{
    return port_sd_present() && storage_get()->state == ST_READY && !port_sd_busy();
}
#ifndef CANFD_REVD_TARGET
static unsigned lower_ascii(unsigned c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static bool names_equal(const char *a, const char *b)
{
    while (*a && *b) if (lower_ascii((unsigned char)*a++) != lower_ascii((unsigned char)*b++)) return false;
    return *a == *b;
}
#endif
/* Strict UTF-8, no controls or directional overrides. Bounded scan never
 * accepts a name by cutting it into a different valid name. */
static int text_check(const char *text, size_t maximum, bool filename)
{
    if (!text) return LOCAL_FILES_ERR_ARGUMENT;
    size_t length = 0;
    while (length <= maximum && text[length]) length++;
    if (!length) return LOCAL_FILES_ERR_NAME;
    if (length > maximum) return LOCAL_FILES_ERR_LIMIT;
    if (filename && (text[0] == '.' || text[0] == ' ' || text[length-1] == '.' || text[length-1] == ' ')) return LOCAL_FILES_ERR_NAME;
    size_t at = 0;
    while (at < length) {
        unsigned first = (unsigned char)text[at++], point = first, minimum = 0, trailing = 0;
        if (first >= 0xC2 && first <= 0xDF) { trailing=1;point=first & 31u;minimum=0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { trailing=2;point=first & 15u;minimum=0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { trailing=3;point=first & 7u;minimum=0x10000; }
        else if (first >= 0x80) return LOCAL_FILES_ERR_UTF8;
        if (at + trailing > length) return LOCAL_FILES_ERR_UTF8;
        while (trailing--) {
            unsigned next = (unsigned char)text[at++];
            if ((next & 0xC0) != 0x80) return LOCAL_FILES_ERR_UTF8;
            point=(point<<6) | (next & 63u);
        }
        if (point < minimum || point > 0x10FFFF || (point>=0xD800 && point<=0xDFFF)) return LOCAL_FILES_ERR_UTF8;
        if (point < 32 || point == 127 || (point>=0x202A && point<=0x202E) || (point>=0x2066 && point<=0x2069)) return LOCAL_FILES_ERR_NAME;
        if (filename && point < 128 && strchr("/\\:*?\"<>|", (int)point)) return LOCAL_FILES_ERR_NAME;
    }
    return 0;
}
int local_db_store(const char *name, const local_db_frame_t *frames, uint32_t count)
{
#ifdef CANFD_REVD_TARGET
    (void)name;(void)frames;(void)count;return LOCAL_FILES_ERR_UNSUPPORTED;
#else
    if(!port_sd_present() || storage_get()->state==ST_MSC || storage_get()->state==ST_FORMATTING)
        return LOCAL_FILES_ERR_SD;
    int rc=text_check(name,LOCAL_DB_NAME_BYTES,true);
    if (rc) return rc;
    const char *extension=strrchr(name,'.');
    if (!extension || (!names_equal(extension,".dbc") && !names_equal(extension,".dbf"))) return LOCAL_FILES_ERR_NAME;
    if (!frames || !count) return LOCAL_FILES_ERR_ARGUMENT;
    if (count > LOCAL_DB_MAX_FRAMES) return LOCAL_FILES_ERR_LIMIT;
    for (uint32_t i=0;i<count;i++) {
        const local_db_frame_t *row=&frames[i];
        rc=text_check(row->name,LOCAL_DB_FRAME_NAME_BYTES,false);
        if (rc) return rc;
        if (row->flags > UINT8_MAX || row->length > 64) return LOCAL_FILES_ERR_FRAME;
        can_frame_t frame={.id=row->id,.flags=(uint8_t)row->flags,.len=(uint8_t)row->length};
        if (tx_validate_frame(0,&frame,false) != TX_OK) return LOCAL_FILES_ERR_FRAME;
    }
    int slot=-1;
    for (unsigned i=0;i<DB_FILE_SLOTS;i++) {
        if (databases[i].used && names_equal(databases[i].name,name)) {slot=(int)i;break;}
        if (!databases[i].used && slot<0) slot=(int)i;
    }
    if (slot<0) return LOCAL_FILES_ERR_FULL;
    database_t *db=&databases[slot];
    memcpy(db->frames,frames,(size_t)count*sizeof *frames);
    snprintf(db->name,sizeof db->name,"%s",name);
    db->count=count;db->used=true;
    if (!++catalogue_token) ++catalogue_token;
    db->token=catalogue_token;
    return slot;
#endif
}
void local_db_clear(void)
{
    memset(databases,0,sizeof databases);
    /* Never recycle a token within one simulator generation. */
    if (!++catalogue_token) ++catalogue_token;
}
unsigned local_db_count(void)
{
    unsigned count=0;
    for (unsigned i=0;i<DB_FILE_SLOTS;i++) if(databases[i].used)count++;
    return count;
}
bool local_db_file_info(unsigned ordinal,char *name,unsigned capacity,uint32_t *frames)
{
    if(!name||!capacity||!frames)return false;
    for(unsigned index=0;index<DB_FILE_SLOTS;index++)
        if(databases[index].used&&ordinal--==0){
            snprintf(name,capacity,"%s",databases[index].name);*frames=databases[index].count;return true;
        }
    return false;
}
const char *local_files_result_text(int result)
{
    switch(result) {
    case LOCAL_FILES_OK:return "已完成";
    case LOCAL_FILES_ERR_ARGUMENT:return "数据库参数无效";
    case LOCAL_FILES_ERR_LIMIT:return "目录超限：最多4个数据库、每库256帧；文件名47、帧名63个UTF-8字节";
    case LOCAL_FILES_ERR_FRAME:return "数据库帧的ID、长度或协议标志无效";
    case LOCAL_FILES_ERR_FULL:return "模拟SD数据库目录已满（4个），同名可完整校验后替换";
    case LOCAL_FILES_ERR_UTF8:return "名称不是完整有效的UTF-8，未截断或替换原目录";
    case LOCAL_FILES_ERR_SD:return "SD卡未就绪、正忙或正在共享给电脑";
    case LOCAL_FILES_ERR_NAME:return "数据库名称无效，只接受无路径的.dbc/.dbf名称";
    case LOCAL_FILES_ERR_CHANGED:return "所选文件或发送槽已变化，请返回后重新选择";
    case LOCAL_FILES_ERR_UNSUPPORTED:return "请在上位机选择DBC/DBF报文并导出.canplan.json，再复制到SD";
    case -1:return "请先开机并唤醒屏幕";
    case -2:return "请先解除按键锁";
    case -3:return "请先处理设备确认";
    case -4:return "请先保存或取消当前本地草稿";
    case -6:return "选择超出范围";
    case FILE_PORT_ERR_READONLY:return "文件正在使用、受保护或为内置演示，不能改名";
    default:return "本地操作未完成";
    }
}
static void notice(const char *message,bool error)
{
    snprintf(U.notice,sizeof U.notice,"%s",message);U.error=error;
}
const char *local_files_notice(void){return U.notice;}
bool local_files_error(void){return U.error;}
bool local_files_active(void){return U.view != OFF;}
bool local_files_draft(void){return U.view == PREFIX || U.view == RENAME || U.view == TEXT;}
int local_files_cursor(void){return U.cursor;}
static void backspace_stop(void)
{
    U.backspace_hold=false;U.backspace_physical=false;U.backspace_next=U.backspace_lease=0;
}
static void keyboard_opened(void)
{
    backspace_stop();
    if(!++keyboard_serial)++keyboard_serial;
    U.keyboard_token=keyboard_serial;
}
uint32_t local_files_keyboard_token(void){return local_files_draft()?U.keyboard_token:0;}
static void close_text(bool cancelled)
{
    local_text_cancel_t cancel=U.text_cancel;
    void *context=U.text_context;
    bool was_text=U.view==TEXT;
    backspace_stop();U.keyboard_token=0;
    U.view=OFF;U.cursor=0;U.record_save=false;U.record_session=0;U.input_mode=LOCAL_INPUT_TEXT;U.text_save=NULL;U.text_cancel=NULL;U.text_context=NULL;
    if(was_text&&cancelled&&cancel)cancel(context);
}
void local_files_close(void){close_text(true);}
void local_files_init(void)
{
    /* UI initialization also runs on an ordinary power-on. Simulated SD
     * contents survive that operation; only sim_reset/format clears them. */
    local_files_close();
    memset(&U,0,sizeof U);
}
int local_text_open(const char *title,const char *initial,unsigned maximum,
                    local_text_save_t save,local_text_cancel_t cancel,void *context)
{
    const power_t *power=power_get();
    if(!power_running()||!power->screen_on)return -1;
    if(power->locked)return -2;
    if(ui_dialog_open())return -3;
    if(local_files_draft())return -4;
    if(!ready())return LOCAL_FILES_ERR_SD;
    if(!title||!initial||!save||!maximum||maximum>FILE_PORT_NAME_MAX_BYTES||strlen(initial)>maximum)
        return LOCAL_FILES_ERR_ARGUMENT;
    int result=text_check(initial,maximum,false);if(result&&initial[0])return result;
    local_files_close();
    U.view=TEXT;U.cursor=0;U.notice[0]=0;U.error=false;keyboard_opened();
    snprintf(U.text,sizeof U.text,"%s",initial);
    snprintf(U.text_title,sizeof U.text_title,"%s",title);
    U.text_maximum=maximum;U.text_save=save;U.text_cancel=cancel;U.text_context=context;
    return 0;
}
static bool typed_character(local_input_mode_t mode,unsigned char c)
{
    bool digit=c>='0'&&c<='9',hex=digit||(c>='A'&&c<='F')||(c>='a'&&c<='f');
    if(mode==LOCAL_INPUT_DECIMAL)return digit;
    return hex||(mode==LOCAL_INPUT_PAYLOAD&&(c==' '||c=='\t'||c=='\r'||c=='\n'));
}
static int typed_check(local_input_mode_t mode,const char *value,unsigned maximum)
{
    if(!value)return LOCAL_FILES_ERR_ARGUMENT;
    unsigned length=0;
    while(length<=maximum&&value[length]){
        unsigned char c=(unsigned char)value[length];
        if(c>=128)return LOCAL_FILES_ERR_UTF8;
        if(!typed_character(mode,c))return LOCAL_FILES_ERR_ARGUMENT;
        length++;
    }
    return length>maximum?LOCAL_FILES_ERR_LIMIT:0;
}
int local_input_open(local_input_mode_t mode,const char *title,const char *initial,unsigned maximum,
                     local_text_save_t save,local_text_cancel_t cancel,void *context)
{
    if(!power_running()||!power_get()->screen_on)return -1;
    if(power_get()->locked)return -2;
    if(ui_dialog_open())return -3;
    if(local_files_draft())return -4;
    if(mode<LOCAL_INPUT_HEX||mode>LOCAL_INPUT_PAYLOAD||!title||!save||!maximum||maximum>LOCAL_INPUT_MAX_BYTES)return LOCAL_FILES_ERR_ARGUMENT;
    int result=typed_check(mode,initial,maximum);if(result)return result;
    local_files_close();U.view=TEXT;U.cursor=0;U.notice[0]=0;U.error=false;keyboard_opened();
    U.input_mode=mode;U.text_maximum=maximum;U.text_save=save;U.text_cancel=cancel;U.text_context=context;
    snprintf(U.text,sizeof U.text,"%s",initial);snprintf(U.text_title,sizeof U.text_title,"%s",title);
    return 0;
}
int local_files_keyboard_replace(uint32_t token,const char *value)
{
    if(!token||token!=local_files_keyboard_token())return LOCAL_FILES_ERR_CHANGED;
    if(!power_running()||!power_get()->screen_on)return -1;
    if(power_get()->locked)return -2;
    if(ui_dialog_open())return -3;
    if(U.input_mode==LOCAL_INPUT_TEXT)return LOCAL_FILES_ERR_ARGUMENT;
    backspace_stop();
    int result=typed_check(U.input_mode,value,U.text_maximum);if(result)return result;
    snprintf(U.text,sizeof U.text,"%s",value);U.notice[0]=0;U.error=false;return 0;
}
static int database_slot(int ordinal)
{
    for(unsigned i=0;i<DB_FILE_SLOTS;i++) if(databases[i].used && ordinal--==0)return (int)i;
    return -1;
}
static void snapshot_logs(void)
{
    U.log_count=0;
    for(int i=0;i<port_sd_file_count() && U.log_count<24;i++) {
        file_info_t info;
        if(!port_sd_file_info(i,&info))continue;
#ifdef CANFD_REVD_TARGET
        if(!file_port_token(i))continue;
#else
        if(!info.sample_available)continue;
#endif
        int n=U.log_count++;
        U.logs[n].index=i;U.logs[n].token=file_port_token(i);
        snprintf(U.logs[n].name,sizeof U.logs[n].name,"%s",info.name);
    }
}
static void editable_name(const file_info_t *file,char out[FILE_PORT_NAME_MAX_BYTES+1u])
{
    const char *name=file->name,*component=name;size_t length=strlen(name);
    /* Target catalogue paths describe physical segments; the naming operation
     * applies to their session directory. Present a legal editable label. */
    for(const char *at=name;*at;at++)if(*at=='/'||*at=='\\'){
        size_t n=(size_t)(at-component);
        if(file->is_session&&n>=8&&component[0]=='S'){
            bool session=true;for(unsigned i=1;i<8;i++)if(component[i]<'0'||component[i]>'9')session=false;
            if(session){name=component;length=n;goto chosen;}
        }
        component=at+1;
    }
    name=component;length=strlen(name);
chosen:
    if(length>FILE_PORT_NAME_MAX_BYTES)length=FILE_PORT_NAME_MAX_BYTES;
    memcpy(out,name,length);out[length]=0;
    const char *extension=strrchr(out,'.');bool log_extension=false;
    if(extension&&strlen(extension)==4){
        char suffix[5];for(unsigned i=0;i<4;i++){unsigned c=(unsigned char)extension[i];suffix[i]=(char)(c>='A'&&c<='Z'?c+32u:c);}suffix[4]=0;
        log_extension=!strcmp(suffix,".log")||!strcmp(suffix,".txt");
    }
    if((!extension||(file->is_session&&!log_extension))&&length<=FILE_PORT_NAME_MAX_BYTES-4u)memcpy(out+length,".log",5);
}
int local_files_open(int kind,int ch,int index,uint32_t token)
{
    const power_t *power=power_get();
    if(!power_running()||!power->screen_on)return -1;
    if(power->locked)return -2;
    if(ui_dialog_open())return -3;
    if(local_files_draft())return -4;
    if(!ready())return LOCAL_FILES_ERR_SD;
    if(kind<2||kind>5||ch<0||ch>=CH_NUM)return -6;
#ifdef CANFD_REVD_TARGET
    if(kind==2)return LOCAL_FILES_ERR_UNSUPPORTED;
#endif
    if(kind==2 && (index<0||index>=TX_ENTRIES))return -6;
    file_info_t file;
    if(kind==4) {
        if(!port_sd_file_info(index,&file)||!file_port_token(index))return -6;
        if(token && token!=file_port_token(index))return LOCAL_FILES_ERR_CHANGED;
        if(file.writing||file.protected_||play_file_in_use(index))return FILE_PORT_ERR_READONLY;
#ifndef CANFD_REVD_TARGET
        if(!file.sample_available)return FILE_PORT_ERR_READONLY;
#endif
    }
    if(kind==3 && rec_get()->state!=REC_IDLE)return LOCAL_FILES_ERR_SD;
    U.notice[0]=0;U.error=false;U.cursor=0;U.ch=ch;
    if(kind==2) {U.slot=index;U.target_revision=tx_get()->e[ch][index].revision;U.view=DATABASES;}
    else if(kind==3){U.view=PREFIX;snprintf(U.text,sizeof U.text,"%s",file_port_rec_prefix_get());}
    else if(kind==4){char name[FILE_PORT_NAME_MAX_BYTES+1u];editable_name(&file,name);U.view=RENAME;U.file_index=index;U.file_token=file_port_token(index);snprintf(U.text,sizeof U.text,"%s",name);}
    else {U.view=FILES;snapshot_logs();}
    if(local_files_draft())keyboard_opened();
    else {backspace_stop();U.keyboard_token=0;}
    return 0;
}
static const char characters[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-. ";
static const char *const words[]={"路试","台架","测试"};
#define CHARACTER_COUNT ((int)(sizeof characters-1u))
#define WORD_COUNT 3
#define BACKSPACE_KEY (CHARACTER_COUNT+WORD_COUNT)
#define CLEAR_KEY (BACKSPACE_KEY+1)
#define SAVE_KEY (BACKSPACE_KEY+2)
#define CANCEL_KEY (BACKSPACE_KEY+3)
#define LOG_KEY (BACKSPACE_KEY+4)
#define TXT_KEY (BACKSPACE_KEY+5)
static const char *keyboard_characters(void){return U.input_mode==LOCAL_INPUT_DECIMAL?"0123456789":U.input_mode==LOCAL_INPUT_PAYLOAD?"0123456789ABCDEF ":U.input_mode==LOCAL_INPUT_HEX?"0123456789ABCDEF":characters;}
static int keyboard_backspace(void){return U.input_mode==LOCAL_INPUT_TEXT?BACKSPACE_KEY:(int)strlen(keyboard_characters());}
static int keyboard_columns(void){return U.input_mode==LOCAL_INPUT_DECIMAL?4:U.input_mode==LOCAL_INPUT_HEX?5:U.input_mode==LOCAL_INPUT_PAYLOAD?6:KEYS_COLUMNS;}
static bool keyboard_ready(void){return U.input_mode!=LOCAL_INPUT_TEXT||ready();}
int local_files_item_count(void)
{
    switch(U.view) {
    case DATABASES:return (int)(local_db_count()?local_db_count():1u)+2;
    case FRAMES:return U.db>=0&&U.db<(int)DB_FILE_SLOTS?(int)databases[U.db].count:0;
    case FRAME:return 6;
    case PREFIX:return CANCEL_KEY+1;
    case RENAME:return TXT_KEY+1;
    case TEXT:return U.input_mode!=LOCAL_INPUT_TEXT?keyboard_backspace()+4:U.record_save?TXT_KEY+1:LOG_KEY+1;
    case FILES:return U.log_count+1;
    default:return 0;
    }
}
static const char *name_error(int rc)
{
    switch(rc) {
    case FILE_PORT_ERR_LIMIT:return "名称超过UTF-8字节上限，草稿保留";
    case FILE_PORT_ERR_BUSY:return "请先停止录制并等待SD就绪，草稿保留";
    case FILE_PORT_ERR_EXISTS:return "已存在同名文件，请改名后再保存";
    case FILE_PORT_ERR_READONLY:return "文件受保护或为内置演示，不能改名";
    case FILE_PORT_ERR_CHANGED:return "文件已变化，请取消并重新选择";
    case FILE_PORT_ERR_EXTENSION:return "完整文件名必须保留.log或.txt后缀";
    case FILE_PORT_ERR_IO:return "SD写入失败，原名称和草稿保留";
    default:return "名称包含非法字符、路径或保留设备名，草稿保留";
    }
}
static int record_file_by_token(uint32_t token,file_info_t *info)
{
    if(!token)return -1;
    for(int i=0;i<port_sd_file_count();i++)
        if(file_port_token(i)==token&&port_sd_file_info(i,info))return i;
    return -1;
}
static const char *record_name_save(const char *value,void *context)
{
    (void)context;
    char name[FILE_PORT_NAME_MAX_BYTES+1u];size_t length=strlen(value);
    if(!strrchr(value,'.')){
        if(length>FILE_PORT_NAME_MAX_BYTES-4u)return name_error(FILE_PORT_ERR_LIMIT);
        snprintf(name,sizeof name,"%s.log",value);
    } else snprintf(name,sizeof name,"%s",value);
    file_info_t info;int index=record_file_by_token(U.file_token,&info);
    if(index<0||!info.is_session||!info.sample_available)return name_error(FILE_PORT_ERR_CHANGED);
    int rc=file_port_rename(index,U.file_token,name);
    return rc?name_error(rc):NULL;
}
int local_record_save_open(uint32_t session,uint32_t token)
{
    const rec_t *recording=rec_get();
    if(!session||!token||recording->session!=session||file_port_record_token(session)!=token)return LOCAL_FILES_ERR_CHANGED;
    if(recording->state!=REC_IDLE||recording->last_result!=MSG_REC_STOPPED)return LOCAL_FILES_ERR_SD;
    file_info_t info;int index=record_file_by_token(token,&info);
    if(index<0||!info.is_session||!info.sample_available)return LOCAL_FILES_ERR_CHANGED;
    if(info.writing||info.protected_||play_file_in_use(index))return FILE_PORT_ERR_READONLY;
    char name[FILE_PORT_NAME_MAX_BYTES+1u];editable_name(&info,name);
    int rc=local_text_open("保存录制文件",name,FILE_PORT_NAME_MAX_BYTES,record_name_save,NULL,NULL);
    if(rc)return rc;
    U.record_save=true;U.record_session=session;U.file_token=token;U.file_index=index;
    return 0;
}
static void append_text(const char *value)
{
    size_t used=strlen(U.text),added=strlen(value),maximum=U.view==PREFIX?FILE_PORT_REC_PREFIX_MAX_BYTES:U.view==TEXT?U.text_maximum:FILE_PORT_NAME_MAX_BYTES;
    if(used+added>maximum){notice(U.input_mode==LOCAL_INPUT_TEXT?"超过名称字节上限，未插入，草稿保留":"超过输入上限，未插入，草稿保留",true);return;}
    memcpy(U.text+used,value,added+1u);U.notice[0]=0;U.error=false;
}
static void backspace_one(void)
{
    size_t size=strlen(U.text);
    if(size){size--;while(size&&((unsigned char)U.text[size]&0xC0)==0x80)size--;U.text[size]=0;}
    U.notice[0]=0;U.error=false;
}
static int backspace_begin(uint32_t token,bool physical)
{
    if(!token||token!=local_files_keyboard_token())return LOCAL_FILES_ERR_CHANGED;
    if(!power_running()||!power_get()->screen_on)return -1;
    if(power_get()->locked)return -2;
    if(ui_dialog_open())return -3;
    if(!keyboard_ready())return LOCAL_FILES_ERR_SD;
    if(U.backspace_hold){
        if(!U.backspace_physical)U.backspace_lease=app_now_ms()+BACKSPACE_LEASE_MS;
        return 0;
    }
    U.cursor=keyboard_backspace();U.backspace_hold=true;U.backspace_physical=physical;
    backspace_one();
    /* Physical OK already crossed its existing long-press threshold. */
    U.backspace_next=app_now_ms()+(physical?BACKSPACE_PERIOD_MS:BACKSPACE_DELAY_MS);
    U.backspace_lease=physical?0:app_now_ms()+BACKSPACE_LEASE_MS;
    return 0;
}
int local_files_backspace_begin(uint32_t token){return backspace_begin(token,false);}
int local_files_backspace_end(uint32_t token)
{
    if(!token||token!=local_files_keyboard_token())return LOCAL_FILES_ERR_CHANGED;
    backspace_stop();return 0;
}
void local_files_tick(void)
{
    if(!U.backspace_hold)return;
    if(!U.backspace_physical&&(int32_t)(app_now_ms()-U.backspace_lease)>=0){backspace_stop();return;}
    for(int key=0;key<KEY_NUM;key++)if(keys_is_down((key_id_t)key)&&
        (key!=KEY_OK||!U.backspace_physical)){backspace_stop();return;}
    if(!local_files_draft()||!power_running()||!power_get()->screen_on||power_get()->locked||
        ui_dialog_open()||!keyboard_ready()||U.cursor!=keyboard_backspace()||
        (U.backspace_physical&&!keys_is_down(KEY_OK))){backspace_stop();return;}
    if((int32_t)(app_now_ms()-U.backspace_next)>=0){
        backspace_one();
        /* A stalled UI never catches up with an uncontrolled deletion burst. */
        U.backspace_next=app_now_ms()+BACKSPACE_PERIOD_MS;
    }
}
static void activate_keyboard(void)
{
    if(U.input_mode!=LOCAL_INPUT_TEXT){
        int back=keyboard_backspace();
        if(U.cursor<back){char text[2]={keyboard_characters()[U.cursor],0};append_text(text);}
        else if(U.cursor==back)backspace_one();
        else if(U.cursor==back+1){U.text[0]=0;U.notice[0]=0;U.error=false;}
        else if(U.cursor==back+3){notice("已取消字段输入，原草稿保留",false);local_files_close();}
        else if(U.cursor==back+2){const char *error=U.text_save(U.text,U.text_context);if(error){notice(error,true);return;}notice("字段已更新到草稿，请在原页面保存",false);close_text(false);}
        return;
    }
    if(U.cursor<CHARACTER_COUNT){char text[2]={characters[U.cursor],0};append_text(text);}
    else if(U.cursor<BACKSPACE_KEY)append_text(words[U.cursor-CHARACTER_COUNT]);
    else if(U.cursor==BACKSPACE_KEY) {
        backspace_one();
    } else if(U.cursor==CLEAR_KEY){U.text[0]=0;U.notice[0]=0;U.error=false;}
    else if(U.cursor==CANCEL_KEY){notice(U.record_save?"已保留自动文件名，录制内容已保存":"已取消命名，原名称保留",false);local_files_close();}
    else if(U.cursor==LOG_KEY)append_text(U.view==TEXT&&!U.record_save?".canplan.json":".log");
    else if(U.cursor==TXT_KEY)append_text(".txt");
    else if(U.cursor==SAVE_KEY) {
        if(U.view==TEXT) {
            const char *error=U.text_save(U.text,U.text_context);
            if(error){notice(error,true);return;}
            notice(U.record_save?"录制文件名已保存；未修改录制内容":"方案已提交保存，请查看文件保存结果",false);close_text(false);return;
        }
        int rc=U.view==PREFIX?file_port_rec_prefix_set(U.text):file_port_rename(U.file_index,U.file_token,U.text);
        if(rc){notice(name_error(rc),true);return;}
        notice(U.view==PREFIX?"录制名称已保存，下次录制使用新名称":"文件已重命名，未修改日志内容",false);
        local_files_close();
    }
}
static bool selected_database_valid(void)
{
    return U.db>=0&&U.db<(int)DB_FILE_SLOTS&&databases[U.db].used&&databases[U.db].token==U.db_token;
}
static void target_select(int delta)
{
    if(U.cursor==0)U.ch=(U.ch+CH_NUM+delta)%CH_NUM;
    else U.slot=(U.slot+TX_ENTRIES+delta)%TX_ENTRIES;
    U.target_revision=tx_get()->e[U.ch][U.slot].revision;
}
static void activate(void)
{
    if(U.view==PREFIX||U.view==RENAME||U.view==TEXT){activate_keyboard();return;}
    if(!ready()){notice(local_files_result_text(LOCAL_FILES_ERR_SD),true);return;}
    if(U.view==DATABASES){
        if(U.cursor<2){target_select(1);return;}
        int db=database_slot(U.cursor-2);
        if(db<0){notice("数据库目录已变化，请重新选择",true);return;}
        U.db=db;U.db_token=databases[db].token;U.view=FRAMES;U.cursor=0;U.notice[0]=0;
    } else if(U.view==FRAMES){
        if(!selected_database_valid()){notice("数据库已更新，请返回目录重新选择",true);return;}
        if(U.cursor<0||U.cursor>=(int)databases[U.db].count)return;
        U.frame=U.cursor;U.preview=databases[U.db].frames[U.frame];U.view=FRAME;U.cursor=4;U.notice[0]=0;
    } else if(U.view==FRAME){
        if(U.cursor==5){U.view=FRAMES;U.cursor=U.frame;U.notice[0]=0;return;}
        if(U.cursor!=4)return;
        if(!selected_database_valid()){notice("数据库已更新，请返回目录重新选择",true);return;}
        if(tx_get()->e[U.ch][U.slot].revision!=U.target_revision){notice(local_files_result_text(LOCAL_FILES_ERR_CHANGED),true);return;}
        const local_db_frame_t *row=&U.preview;
        uint8_t data[64]={0};
        tx_result_t rc=tx_entry_update(U.ch,U.slot,row->id,(uint8_t)row->flags,(uint8_t)row->length,data,0);
        if(rc!=TX_OK){notice(tx_result_text(rc),true);return;}
        U.target_revision=tx_get()->e[U.ch][U.slot].revision;
        if(ui_page()==PG_TX)(void)ui_tx_select(U.ch,U.slot);
        snprintf(U.notice,sizeof U.notice,"已加入 CAN%d 第%d条（0x%lX），数据初值00，未发送",U.ch+1,U.slot+1,(unsigned long)row->id);U.error=false;
    } else if(U.view==FILES){
        if(U.cursor==U.log_count){local_files_close();return;}
        if(U.cursor<0||U.cursor>=U.log_count)return;
        int at=U.cursor;
        if(file_port_token(U.logs[at].index)!=U.logs[at].token){notice("文件列表已变化，请返回后重新打开",true);return;}
        file_info_t info;
        if(!port_sd_file_info(U.logs[at].index,&info)||info.writing||info.protected_||play_file_in_use(U.logs[at].index)) {notice("文件正在使用或受保护，请先停止或解除保护",true);return;}
        U.file_index=U.logs[at].index;U.file_token=U.logs[at].token;U.view=RENAME;U.cursor=0;keyboard_opened();
        char name[FILE_PORT_NAME_MAX_BYTES+1u];editable_name(&info,name);
        snprintf(U.text,sizeof U.text,"%s",name);U.notice[0]=0;
    }
}
void local_files_touch(int index)
{
    if(!local_files_active()||index<0||index>=local_files_item_count())return;
    backspace_stop();U.cursor=index;activate();
}
void local_files_key(key_id_t key,gesture_t gesture)
{
    if(!local_files_active())return;
    if(key==KEY_OK&&gesture==GEST_LONG&&local_files_draft()&&U.cursor==keyboard_backspace()){
        (void)backspace_begin(U.keyboard_token,true);return;
    }
    backspace_stop();
    if(key==KEY_BACK){
        if(local_files_draft()){notice(U.input_mode!=LOCAL_INPUT_TEXT?"已取消字段输入，原草稿保留":U.record_save?"已保留自动文件名，录制内容已保存":"已取消命名，原名称保留",false);local_files_close();}
        else if(U.view==FRAME){U.view=FRAMES;U.cursor=U.frame;U.notice[0]=0;}
        else if(U.view==FRAMES){U.view=DATABASES;U.cursor=0;U.notice[0]=0;}
        else local_files_close();
        return;
    }
    if(gesture==GEST_LONG)return;
    int count=local_files_item_count(),step=0;
    if(local_files_draft()) {
        if(key==KEY_LEFT)step=-1;else if(key==KEY_RIGHT)step=1;
        else if(key==KEY_UP)step=-keyboard_columns();else if(key==KEY_DOWN)step=keyboard_columns();
    } else if(U.view==DATABASES && U.cursor<2 && (key==KEY_LEFT||key==KEY_RIGHT)){
        target_select(key==KEY_LEFT?-1:1);return;
    } else {if(key==KEY_UP)step=-1;else if(key==KEY_DOWN)step=1;}
    if(step){U.cursor+=step;if(U.cursor<0)U.cursor=0;if(U.cursor>=count)U.cursor=count-1;}
    else if(key==KEY_OK && gesture==GEST_SHORT)activate();
}
void local_files_handle_event(const app_evt_t *event)
{
    if(!event)return;
    if(event->type==EV_COMBO||event->type==EV_TOUCH_ITEM||event->type==EV_TOUCH_BACK||
       event->type==EV_TOUCH_DLG||event->type==EV_SD_REMOVE||event->type==EV_NTF_STORAGE_LOST||
       (event->type==EV_KEY&&event->ch!=KEY_OK))backspace_stop();
    if(event->type==EV_SD_FORMAT_DONE && event->a==SD_OK) {
        backspace_stop();local_db_clear();
        if(U.view==DATABASES||U.view==FRAMES||U.view==FRAME){U.view=DATABASES;U.cursor=0;notice("SD已格式化，数据库目录已清空",false);}
    } else if(event->type==EV_SYS_STOP_ALL)local_files_close();
}
static void item(jw_t *j,const char *label,const char *value,unsigned flags)
{
    jw_obj(j);jw_kstr(j,"l",label);jw_kstr(j,"v",value);jw_kint(j,"f",flags);jw_end_obj(j);
}
static const char *key_text(int index,char scratch[2],const char **kind,const char **value)
{
    if(U.input_mode!=LOCAL_INPUT_TEXT){
        int back=keyboard_backspace();*kind="text";
        if(index<back){scratch[0]=keyboard_characters()[index];scratch[1]=0;*value=scratch;return scratch[0]==' '?"空格":scratch;}
        *value="";*kind="action";return index==back?"退格":index==back+1?"清空":index==back+2?"确定":"取消";
    }
    *kind="text";
    if(index<CHARACTER_COUNT){scratch[0]=characters[index];scratch[1]=0;*value=scratch;return characters[index]==' '?"空格":scratch;}
    if(index<BACKSPACE_KEY){*value=words[index-CHARACTER_COUNT];return *value;}
    *value="";*kind="action";
    if(index==BACKSPACE_KEY)return "退格";
    if(index==CLEAR_KEY)return "清空";
    if(index==SAVE_KEY)return "保存";
    if(index==CANCEL_KEY)return U.record_save?"保留默认名":"取消";
    *kind="text";*value=index==LOG_KEY?(U.view==TEXT&&!U.record_save?".canplan.json":".log"):".txt";
    return U.view==TEXT&&!U.record_save&&index==LOG_KEY?"方案后缀":*value;
}
void local_files_json(jw_t *j)
{
    const char *page=U.view==DATABASES?"local_db":U.view==FRAMES?"local_frames":U.view==FRAME?"local_frame":U.view==PREFIX?"local_name":U.view==RENAME?"local_rename":U.view==TEXT?(U.input_mode!=LOCAL_INPUT_TEXT?"local_input":U.record_save?"local_record_save":"local_plan_name"):"local_files";
    const char *title=U.view==DATABASES?"SD报文数据库":U.view==FRAMES?"选择数据库报文":U.view==FRAME?"报文定义预览":U.view==PREFIX?"本地录制命名":U.view==RENAME?"本地文件改名":U.view==TEXT?U.text_title:"选择要改名的日志";
    jw_kstr(j,"page",page);jw_kstr(j,"title",title);jw_kint(j,"cursor",U.cursor);
    if(U.input_mode!=LOCAL_INPUT_TEXT){jw_kstr(j,"parent_page",ui_base_page_id());jw_kstr(j,"return_page",ui_base_page_id());}
    jw_kbool(j,"numeric",false);jw_kint(j,"digit",0);
    if(U.record_save){jw_kint(j,"record_session",U.record_session);jw_kint(j,"record_token",U.file_token);}
    jw_kstr(j,"hint",U.input_mode!=LOCAL_INPUT_TEXT?"键盘直接输入 · 确定更新草稿 · 取消不修改 · 外层保存生效":U.record_save?"录制内容已自动保存；基础名称自动补.log；BACK保留默认名":local_files_draft()?"方向键选字符 · OK 输入 · 保存生效 · BACK 取消":"↑↓ 选择 · OK 确认 · BACK 返回；不会自动发送");
    bool stale=(U.view==FRAMES||U.view==FRAME)&&!selected_database_valid();
    jw_kstr(j,"local_notice",stale?"数据库已更新，请返回目录重新选择":U.notice);jw_kbool(j,"local_error",U.error||stale);
    jw_kbool(j,"local_draft",local_files_draft());
    jw_key(j,"local_target");jw_obj(j);jw_kint(j,"ch",U.ch);jw_kint(j,"entry",U.slot);jw_end_obj(j);
    if(local_files_draft()) {
        jw_key(j,"keyboard");jw_obj(j);jw_kstr(j,"value",U.text);jw_kint(j,"columns",keyboard_columns());
        jw_kstr(j,"mode",U.input_mode==LOCAL_INPUT_HEX?"hex":U.input_mode==LOCAL_INPUT_DECIMAL?"decimal":U.input_mode==LOCAL_INPUT_PAYLOAD?"payload":"text");
        jw_kint(j,"token",U.keyboard_token);jw_kbool(j,"backspace_holding",U.backspace_hold);
        jw_kint(j,"backspace_delay_ms",BACKSPACE_DELAY_MS);jw_kint(j,"backspace_period_ms",BACKSPACE_PERIOD_MS);jw_kint(j,"backspace_lease_ms",BACKSPACE_LEASE_MS);
        jw_kint(j,"max_bytes",U.view==PREFIX?FILE_PORT_REC_PREFIX_MAX_BYTES:U.view==TEXT?U.text_maximum:FILE_PORT_NAME_MAX_BYTES);
        jw_kstr(j,"charset",U.input_mode==LOCAL_INPUT_DECIMAL?"0-9（十进制）":U.input_mode==LOCAL_INPUT_HEX?"0-9 A-F（十六进制）":U.input_mode==LOCAL_INPUT_PAYLOAD?"完整十六进制字节，可带空格":"ASCII字母数字、_-.空格；中文快捷词：路试/台架/测试");
        jw_key(j,"keys");jw_arr(j);
        for(int i=0;i<local_files_item_count();i++) {
            char scratch[2];const char *kind,*value,*label=key_text(i,scratch,&kind,&value);
            jw_obj(j);jw_kint(j,"index",i);jw_kstr(j,"label",label);jw_kstr(j,"value",value);jw_kstr(j,"kind",kind);jw_end_obj(j);
        }
        jw_end_arr(j);jw_end_obj(j);
    }
    jw_key(j,"items");jw_arr(j);
    char value[128],label[96];
    if(local_files_draft()) {
        for(int i=0;i<local_files_item_count();i++) {
            char scratch[2];const char *kind,*text,*key=key_text(i,scratch,&kind,&text);
            item(j,key,"",U.input_mode!=LOCAL_INPUT_TEXT?(i==keyboard_backspace()+2||i==keyboard_backspace()+3?ACTION:0):(i==SAVE_KEY||i==CANCEL_KEY?ACTION:0));
        }
    } else if(U.view==DATABASES) {
        snprintf(value,sizeof value,"CAN%d",U.ch+1);item(j,"目标发送通道",value,0);
        snprintf(value,sizeof value,"第%d条（替换该槽定义，不发送）",U.slot+1);item(j,"目标发送槽",value,0);
        for(unsigned ordinal=0;ordinal<local_db_count();ordinal++) {
            int index=database_slot((int)ordinal);
            snprintf(value,sizeof value,"%lu 帧 · OK 浏览",(unsigned long)databases[index].count);
            item(j,databases[index].name,value,ready()?ACTION:DISABLED);
        }
        if(!local_db_count())item(j,"暂无DBC/DBF数据库","请把数据库文件放入模拟SD后再选择",DISABLED);
    } else if(U.view==FRAMES) {
        if(U.db>=0 && U.db<(int)DB_FILE_SLOTS) for(uint32_t i=0;i<databases[U.db].count;i++) {
            const local_db_frame_t *frame=&databases[U.db].frames[i];
            snprintf(value,sizeof value,"0x%0*lX · %s · %luB",frame->flags&CANF_EXT?8:3,(unsigned long)frame->id,frame->flags&CANF_FD?"FD":"CAN",(unsigned long)frame->length);
            item(j,frame->name,value,selected_database_valid()&&ready()?ACTION:DISABLED);
        }
    } else if(U.view==FRAME && U.db>=0 && U.db<(int)DB_FILE_SLOTS) {
        const local_db_frame_t *frame=&U.preview;
        snprintf(value,sizeof value,"0x%0*lX · %s",frame->flags&CANF_EXT?8:3,(unsigned long)frame->id,frame->name);item(j,"报文 ID / 名称",value,0);
        item(j,"帧格式",frame->flags&CANF_RTR?"RTR（无数据载荷）":frame->flags&CANF_FD?frame->flags&CANF_BRS?"CAN FD / BRS":"CAN FD":"Classic CAN",0);
        snprintf(value,sizeof value,"%lu 字节；数据初始全00",(unsigned long)frame->length);item(j,"数据长度",value,0);
        snprintf(value,sizeof value,"%lu ms（定义参考；不自动启用周期）",(unsigned long)frame->cycle_ms);item(j,"数据库周期",value,0);
        snprintf(label,sizeof label,"加入 CAN%d 第%d条",U.ch+1,U.slot+1);item(j,label,"替换ID/格式/长度/数据；不会发送",ready()&&selected_database_valid()?ACTION:DISABLED);
        item(j,"返回帧列表","保留目标通道与槽",ACTION);
    } else if(U.view==FILES) {
        for(int i=0;i<U.log_count;i++) {
            file_info_t info;bool valid=file_port_token(U.logs[i].index)==U.logs[i].token && port_sd_file_info(U.logs[i].index,&info);
            const char *state=!valid?"文件已变化":info.writing?"正在写入":info.protected_?"受保护":play_file_in_use(U.logs[i].index)?"正在使用":"OK 本地改名";
            item(j,U.logs[i].name,state,valid&&!info.writing&&!info.protected_&&!play_file_in_use(U.logs[i].index)?ACTION:DISABLED);
        }
        item(j,"返回",U.log_count?"原文件不变":"暂无可改名的已保存日志",ACTION);
    }
    jw_end_arr(j);
}
