/* 模拟器对外接口：网页（经 sim/web/server.py）和单元测试都通过它驱动"整机"。 */
#ifndef SIM_API_H
#define SIM_API_H

#include <stdint.h>
#include "sim/tx_sequence.h"
#include "sim/local_files_ui.h"
#include "sim/send_plan.h"
#include "sim/tx_scope.h"

#if defined(_WIN32)
#define SIM_EXPORT __declspec(dllexport)
#else
#define SIM_EXPORT __attribute__((visibility("default")))
#endif

SIM_EXPORT void sim_reset(void);
SIM_EXPORT void sim_tick(int ms);
SIM_EXPORT int sim_local_backspace(int action, uint32_t keyboard_token);
SIM_EXPORT int sim_local_keyboard_replace(uint32_t keyboard_token, const char *value);
SIM_EXPORT void sim_key(int key, int down);                 /* key: 0..7 = ↑↓←→ OK BACK REC POWER */
SIM_EXPORT void sim_touch(int kind, int a);                 /* kind: 0 条目，1 返回，2 对话框按钮 */
SIM_EXPORT int  sim_env(const char *cmd, int a, int b);     /* 环境控制，见 env_sim.c env_cmd */
SIM_EXPORT int  sim_host(const char *cmd, int a, int b);    /* 模拟上位机发来的请求 */
SIM_EXPORT int  sim_snapshot(char *buf, int cap);           /* 返回写入长度；JSON：{"dev":..., "env":...} */
SIM_EXPORT int  sim_tx_entry(int ch, int idx, uint32_t id, int flags, int len,
                             const uint8_t *data, uint32_t period_ms, uint32_t expected_revision);
SIM_EXPORT const char *sim_tx_result_text(int result);
SIM_EXPORT int  sim_tx_edit(int ch, int idx);
SIM_EXPORT int  sim_filter(int channel, int type, uint32_t id, uint32_t mask, int direction, int format);
/* Review navigation only: 0 navigate, 1 live view, 2 live pause, 3 detail,
 * 4 open filter, 5 channel, 6 signed live cursor movement (-128..128).
 * Detail may address a stable scroll sequence instead of index.
 * All actions retain power/lock/dialog/draft guards in the shared UI model. */
SIM_EXPORT int  sim_ui_action(int action, int value, uint32_t sequence, int use_sequence);
/* Device record buttons: 0 start a new session, 1 stop/close, 2 mark.
 * UI_REVIEW_* guards; -8 wrong state, -9 storage not writable, -10 sample capacity. */
SIM_EXPORT int  sim_rec_action(int action);
SIM_EXPORT int sim_sequence(int action, int ch, const txseq_entry_config_t *entries,
                            uint32_t id_gap_ms, uint32_t revision);
SIM_EXPORT const char *sim_sequence_text(int result);
SIM_EXPORT int sim_tx_control(int action, int ch, int index);
SIM_EXPORT int sim_tx_select(int ch, int index);
SIM_EXPORT int sim_local_open(int kind, int ch, int index, uint32_t token);
SIM_EXPORT int sim_detail_to_tx(int ch, int index);
SIM_EXPORT int sim_plan_open(int mode,int ch);
SIM_EXPORT int sim_tx_scope_select(uint32_t mask);
SIM_EXPORT int sim_tx_target_select(uint32_t mask);
/* Desired-state channel switch: 0 stop, 1 run; repeats are idempotent. */
SIM_EXPORT int sim_tx_channel_send(int ch, int enabled);
SIM_EXPORT int sim_scope_control(int action,uint32_t mask,uint32_t index);
SIM_EXPORT const char *sim_scope_text(int result);
SIM_EXPORT int sim_plan_refresh(void);
SIM_EXPORT int sim_plan_io_peek(sp_io_request_t *request);
SIM_EXPORT int sim_plan_io_complete(uint32_t id,int result,const char *message);
SIM_EXPORT int sim_plan_catalog_replace(const sp_file_t *files,uint32_t count);
SIM_EXPORT const char *sim_plan_result_text(int result);
SIM_EXPORT int sim_database_store(const char *name, const local_db_frame_t *frames, uint32_t count);
SIM_EXPORT const char *sim_local_result_text(int result);
SIM_EXPORT int sim_sequence_defaults_all(uint32_t first_revision, uint32_t second_revision);
typedef struct {
    uint32_t id, revision, period_ms;
    int index, flags, len;
    uint8_t data[64];
} sim_tx_update_t;
SIM_EXPORT int sim_tx_batch(int ch, const sim_tx_update_t *rows, int count);
SIM_EXPORT int sim_record_name(const char *prefix);
SIM_EXPORT int sim_log_rename(int index, uint32_t token, const char *name);
SIM_EXPORT int sim_log_name(int index, uint32_t token, char *buf, uint32_t cap);
SIM_EXPORT int  sim_log_import(const char *name, const char *text, uint32_t size, uint32_t *line, uint32_t *frames);
SIM_EXPORT int  sim_log_export(int file, char *buf, uint32_t cap);
SIM_EXPORT const char *sim_log_error_text(int result);

#endif
