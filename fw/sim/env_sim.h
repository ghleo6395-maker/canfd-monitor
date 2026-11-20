/* 模拟板子：实现 services/core/port.h，模拟两条 CAN 总线、SD 卡、USB、电池、芯片版本。
 * 只给模拟器和单元测试用；上板后由 platform/ 下的驱动替换。 */
#ifndef ENV_SIM_H
#define ENV_SIM_H

#include "services/core/app_types.h"
#include "services/core/jsonw.h"

/* Host-only bounded samples; board storage must use streaming FatFs I/O. */
#define ENV_LOG_MAX_FILES 16u
#define ENV_LOG_MAX_FRAMES 512u
#define ENV_LOG_MAX_TEXT 131072u
#define ENV_REC_PREFIX_MAX_BYTES 18u
#define ENV_FILE_NAME_MAX_BYTES 47u

typedef enum {
    ENV_LOG_OK = 0, ENV_LOG_ERR_ARGUMENT = -1, ENV_LOG_ERR_LIMIT = -2,
    ENV_LOG_ERR_FORMAT = -3, ENV_LOG_ERR_TIMESTAMP = -4, ENV_LOG_ERR_NO_SLOT = -5,
    ENV_LOG_ERR_NOT_AVAILABLE = -6, ENV_LOG_ERR_SPACE = -7, ENV_LOG_ERR_BUSY = -8,
    ENV_LOG_ERR_EXISTS = -9, ENV_LOG_ERR_READONLY = -10, ENV_LOG_ERR_CHANGED = -11,
    ENV_LOG_ERR_NAME = -12, ENV_LOG_ERR_EXTENSION = -13
} env_log_status_t;

typedef struct {
    int status;
    uint32_t error_line, frames;
} env_log_result_t;

/* Import is atomic: any bad line, descending timestamp, input/frame limit or
 * unavailable slot rejects the entire file. No silent truncation on import.
 * Filename max 47 valid UTF-8 bytes, .log/.txt only, no paths or reserved names.
 * Duplicate names are rejected (ASCII case insensitive). Error lines are one based. */
int env_log_import(const char *name, const char *text, size_t size, env_log_result_t *result);
/* Export imported/recorded samples as candump -L; no output on insufficient cap.
 * Returns byte count without NUL, needed includes NUL. Presets have no real text.
 * A truncated recorded file exports its retained prefix; inspect file_info_t. */
int env_log_export(int idx, char *buf, size_t cap, size_t *needed);
const char *env_log_error_name(int status);

/* Host simulator naming only. Empty prefix restores the REC default. A prefix
 * is at most 18 UTF-8 bytes; generated names include RTC date/time and session.
 * Metadata writes require an idle, mounted, writable simulated SD. */
int env_rec_name_set(const char *prefix);
const char *env_rec_name_get(void);
/* Stable token survives index compaction; zero means no such file. Requesters
 * must also bind the simulator generation to detect a whole-simulator reset. */
uint32_t env_file_token(int idx);
/* Actual host recording session -> stable file token, including while writing.
 * Imports/fixtures have no recording session. Callers still check closed state
 * before metadata writes; zero also denotes absent/ambiguous/removed SD. */
uint32_t env_record_file_token(uint32_t session);
/* Only closed, unprotected, non-fixture, unused recorded/imported files may be
 * renamed. The expected token makes a delayed request fail after compaction. */
int env_log_rename(int idx, uint32_t expected_token, const char *name);

void env_reset(void);
void env_tick(uint32_t ms);
void env_set_key(key_id_t k, bool down);
int  env_cmd(const char *cmd, int a, int b);   /* 返回 0 成功，-1 命令不认识 */
void env_json(jw_t *j);

#endif
