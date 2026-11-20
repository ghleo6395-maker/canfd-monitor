/* Durable send-plan model shared by the device UI and simulator. */
#ifndef SEND_PLAN_H
#define SEND_PLAN_H
#include "services/core/app_types.h"
#include "services/core/jsonw.h"

#define SP_MAX_FILES 16u
#define SP_NAME_BYTES 47u
#define SP_PROJECT_BYTES 47u
#define SP_VERSION 1u
/* The serialized wire layout contains uint32_t only, except raw data/name bytes.
 * dynamic: byte,bits,max,next_value,checksum,checksum_byte,from,to.
 * checksum: 0 none,1 xor,2 sum,3 crc8-j1850,4 crc8-h2f. */
typedef struct {
    uint32_t id, flags, length, period_ms;
    uint8_t data[64];
    uint32_t dynamic[8];
    uint32_t enabled, repeat_count, frame_gap_ms, cycle_ms;
} send_plan_entry_t;
typedef struct { uint32_t id_gap_ms; send_plan_entry_t entries[4]; } send_plan_channel_t;
typedef struct { uint32_t channel_mask; send_plan_channel_t channels[2]; } send_plan_t;
typedef struct { char name[48], project[48]; send_plan_t plan; uint32_t token; } sp_file_t;
typedef enum { SP_IO_NONE, SP_IO_SAVE, SP_IO_REFRESH } sp_io_kind_t;
typedef struct { uint32_t kind,id; char name[48]; send_plan_t plan; } sp_io_request_t;
typedef enum { SP_IDLE, SP_LOADED, SP_CONFIRMING, SP_ARMING, SP_RUNNING } sp_run_state_t;
typedef struct {
    uint32_t loaded_mask;
    char loaded_name[48];
    sp_run_state_t state;
    uint32_t request;
    uint32_t run_once;
    int last_error;
    char message[192];
    uint32_t io_pending,io_kind,io_id;
} sp_status_t;
typedef enum {
    SP_OK=0, SP_WAIT_CONFIRM=1,
    SP_ERR_ARGUMENT=-40, SP_ERR_FORMAT=-41, SP_ERR_BUSY=-42,
    SP_ERR_CHANGED=-43, SP_ERR_POWER=-44, SP_ERR_FILE=-45,
    SP_ERR_NAME=-46, SP_ERR_IO=-47, SP_ERR_EMPTY=-48,
    SP_ERR_MODE=-49, SP_ERR_FRAME=-50, SP_ERR_LOAD=-51
} sp_result_t;

/* Full process/MCU reset clears cached files; the storage owner refreshes them.
 * A software power cycle resets runtime ownership but preserves the catalogue
 * and any immutable in-flight file operation. */
void sp_init(void);
void sp_reset_runtime(void);
int sp_validate(const send_plan_t *plan);
int sp_capture(uint32_t channel_mask,send_plan_t *out);
void sp_default_name(uint32_t channel_mask,char *out,uint32_t capacity);
int sp_request_save(const char *name,const send_plan_t *snapshot);
int sp_request_refresh(void);
bool sp_io_peek(sp_io_request_t *out);
int sp_io_complete(uint32_t id,int result,const char *message);
int sp_catalog_replace(const sp_file_t *files,uint32_t count);
unsigned sp_catalog_count(void);
bool sp_catalog_get(unsigned index,sp_file_t *out);
int sp_load(unsigned index,uint32_t token);
int sp_start(void);
int sp_start_once(void);
int sp_answer(uint32_t request,bool allow);
void sp_stop(void);
void sp_tick(void);
void sp_handle(const app_evt_t *event);
const sp_status_t *sp_state(void);
const send_plan_t *sp_loaded_plan(void);
const char *sp_result_text(int result);
/* Adds a send_plans member to an existing object. */
void sp_json(jw_t *json);
#endif
