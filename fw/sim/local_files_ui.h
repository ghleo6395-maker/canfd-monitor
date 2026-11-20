/* Shared device keyboard/naming. Database import is a simulator-only helper. */
#ifndef LOCAL_FILES_UI_H
#define LOCAL_FILES_UI_H

#include "services/core/app_types.h"
#include "services/core/jsonw.h"

#define LOCAL_DB_MAX_FILES 4u
#define LOCAL_DB_MAX_FRAMES 256u
#define LOCAL_DB_NAME_BYTES 47u
#define LOCAL_DB_FRAME_NAME_BYTES 63u

typedef struct {
    uint32_t id, flags, length, cycle_ms;
    char name[LOCAL_DB_FRAME_NAME_BYTES + 1u];
} local_db_frame_t;

typedef enum {
    LOCAL_FILES_OK = 0, LOCAL_FILES_ERR_ARGUMENT = -20,
    LOCAL_FILES_ERR_LIMIT = -21, LOCAL_FILES_ERR_FRAME = -22,
    LOCAL_FILES_ERR_FULL = -23, LOCAL_FILES_ERR_UTF8 = -24,
    LOCAL_FILES_ERR_SD = -25, LOCAL_FILES_ERR_NAME = -26,
    LOCAL_FILES_ERR_CHANGED = -27, LOCAL_FILES_ERR_UNSUPPORTED = -28
} local_files_result_t;

/* Atomically validate and install a parsed database in the simulated SD.
 * Same name replaces a catalogue only after complete validation. Returns its
 * slot 0..3; negative means no catalogue changed. No payload is imported. */
int local_db_store(const char *name, const local_db_frame_t *frames, uint32_t count);
void local_db_clear(void);
unsigned local_db_count(void);
/* Read-only file inventory for the device's unified file list. */
bool local_db_file_info(unsigned ordinal,char *name,unsigned capacity,uint32_t *frames);
const char *local_files_result_text(int result);

/* init and close reset/cancel just UI state. sim_reset must explicitly call
 * local_db_clear; ordinary power-on/UI initialization preserves SD contents.
 * kind2 DB chooser (index is destination TX slot), kind3 record prefix,
 * kind4 rename indexed log bound to token (0 binds current), kind5 log chooser.
 * Call through shared UI review guards; this module also checks power/lock/SD. */
void local_files_init(void);
void local_files_close(void);
int local_files_open(int kind, int ch, int index, uint32_t token);
bool local_files_active(void);
bool local_files_draft(void);
int local_files_cursor(void);
int local_files_item_count(void);
void local_files_key(key_id_t key, gesture_t gesture);
void local_files_touch(int index);
/* Backend-owned UTF-8 backspace repeat. Begin deletes one character now,
 * repeats after 400 ms and then once per 90 ms; matching end never deletes.
 * Each new keyboard has a fresh token, including across simulator resets.
 * Browser holds expire after 1500 ms without begin renewal. Renewing an active
 * hold never deletes again or resets the initial/repeat schedule. Physical OK
 * holds are released by the actual key state and do not use this lease. */
uint32_t local_files_keyboard_token(void);
int local_files_backspace_begin(uint32_t keyboard_token);
int local_files_backspace_end(uint32_t keyboard_token);
void local_files_tick(void);
/* Writes fields inside an existing ui object, never opens/closes that object. */
void local_files_json(jw_t *j);
const char *local_files_notice(void);
bool local_files_error(void);
/* Reusable on-device text keyboard. The save callback returns NULL on accepted
 * save, or a stable error string to keep this draft open. Cancellation includes
 * BACK, normal page teardown and power-off. The context is owned by the caller. */
typedef const char *(*local_text_save_t)(const char *value, void *context);
typedef void (*local_text_cancel_t)(void *context);
int local_text_open(const char *title, const char *initial, unsigned max_bytes,
                    local_text_save_t save, local_text_cancel_t cancel, void *context);
typedef enum { LOCAL_INPUT_TEXT,LOCAL_INPUT_HEX,LOCAL_INPUT_DECIMAL,LOCAL_INPUT_PAYLOAD } local_input_mode_t;
#define LOCAL_INPUT_MAX_BYTES 191u
/* Typed on-device field input. Confirm updates the caller's draft only. */
int local_input_open(local_input_mode_t mode,const char *title,const char *initial,unsigned max_bytes,
                     local_text_save_t save,local_text_cancel_t cancel,void *context);
/* Replace only a typed keyboard draft; generation is checked by its caller. */
int local_files_keyboard_replace(uint32_t token,const char *value);
/* Successful recording close only: bind the expected session and stable file
 * token. Opens local_record_save with the automatic filename. Base names gain
 * .log on save; BACK / keep-default never removes the already saved file. */
int local_record_save_open(uint32_t session,uint32_t file_token);
/* Forward relevant storage/power events to invalidate a catalogue on format. */
void local_files_handle_event(const app_evt_t *event);

#endif
