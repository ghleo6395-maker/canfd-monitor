#include "sim/sim_api.h"
#include "app/app.h"
#include "services/core/app_evt.h"
#include "services/power/power_sm.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/port.h"
#include "services/disp/disp_pipe.h"
#include "services/tx_sched/tx_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "sim/env_sim.h"

#include <string.h>
#include <stdio.h>

void sim_reset(void)
{
    env_reset();
    local_db_clear();
    app_init();
}

void sim_tick(int ms)
{
    if (ms <= 0)
        return;
    app_tick((uint32_t)ms);
    env_tick((uint32_t)ms);
}

void sim_key(int key, int down)
{
    if (key < 0 || key >= KEY_NUM)
        return;
    env_set_key((key_id_t)key, down != 0);
    app_key_raw((key_id_t)key, down != 0);
}

void sim_touch(int kind, int a)
{
    static const ev_type_t map[] = { EV_TOUCH_ITEM, EV_TOUCH_BACK, EV_TOUCH_DLG };
    if (kind < 0 || kind > 2)
        return;
    app_touch(map[kind], a);
}

int sim_env(const char *cmd, int a, int b)
{
    return env_cmd(cmd, a, b);
}

int sim_local_backspace(int action,uint32_t token)
{
    if(action<0||action>1)return LOCAL_FILES_ERR_ARGUMENT;
    int rc=action==0?local_files_backspace_begin(token):local_files_backspace_end(token);
    if(!rc&&action==0)power_activity();
    return rc;
}
int sim_local_keyboard_replace(uint32_t token,const char *value)
{
    int rc=local_files_keyboard_replace(token,value);
    if(!rc)power_activity();
    return rc;
}

static bool is(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* 上位机请求：src=SRC_HOST，危险操作由各服务要求设备上确认 */
int sim_host(const char *cmd, int a, int b)
{
    const power_t *p = power_get();
    if (!p->host_connected || p->state != PWR_RUNNING)
        return -2;
    if (a < 0 || a >= CH_NUM)
        return -3;
    uint8_t ch = (uint8_t)a;
    if (is(cmd, "rec_start"))
        evt_post4(EV_REQ_REC_START, 0, SRC_HOST, 0, 0);
    else if (is(cmd, "rec_stop"))
        evt_post4(EV_REQ_REC_STOP, 0, SRC_HOST, 0, 0);
    else if (is(cmd, "ch_normal"))
        evt_post4(EV_REQ_CH_MODE, ch, SRC_HOST, CAN_MODE_NORMAL, 0);
    else if (is(cmd, "ch_listen"))
        evt_post4(EV_REQ_CH_MODE, ch, SRC_HOST, CAN_MODE_LISTEN, 0);
    else if (is(cmd, "ch_rate"))
        evt_post4(EV_REQ_CH_RATE, ch, SRC_HOST, b, 0);
    else if (is(cmd, "msc"))
        evt_post4(EV_REQ_MSC, 0, SRC_HOST, 1, 0);
    else if (is(cmd, "cyc_on"))
        evt_post4(EV_REQ_TX_CYC, ch, SRC_HOST, b, 1);
    else if (is(cmd, "cyc_off"))
        evt_post4(EV_REQ_TX_CYC, ch, SRC_HOST, b, 0);
    else if (is(cmd, "tx_send"))
        evt_post4(EV_REQ_TX_SEND, ch, SRC_HOST, b, 0);
    else if (is(cmd, "tx_stop"))
        evt_post4(EV_REQ_TX_STOP_ALL, ch, SRC_HOST, 0, 0);
    else if (is(cmd, "autobaud"))
        evt_post4(EV_REQ_AUTOBAUD, ch, SRC_HOST, CAN_MODE_LISTEN, 0);
    else if (is(cmd, "shutdown"))
        evt_post4(EV_REQ_SHUTDOWN, 0, SRC_HOST, 0, 0);
    else
        return -1;
    app_tick(0);
    return 0;
}

/* Workbench controls prepare data but still use device service validation and
 * confirmation. They never claim that a real USB device is connected. */
static bool editable(void)
{
    return power_running() && !power_get()->locked;
}

static int controls_guard(void)
{
    if (!editable() || !power_get()->screen_on) return -100;
    if (ui_dialog_open()) return -102;
    if (ui_draft_open()) return -103;
    return 0;
}

int sim_sequence(int action, int ch, const txseq_entry_config_t *entries,
                 uint32_t id_gap_ms, uint32_t revision)
{
    int guard = controls_guard();
    if (guard) return guard;
    if (ch < 0 || ch >= CH_NUM) return TXSEQ_ERR_CHANNEL;
    int rc;
    switch (action) {
    case 0: rc = txseq_configure(ch, entries, id_gap_ms, revision); break;
    case 1: rc = txseq_defaults(ch, revision); break;
    case 2: rc = txseq_start(ch, revision); break;
    case 3: txseq_stop(ch); rc = 0; break;
    default: return TXSEQ_ERR_ARGUMENT;
    }
    power_activity();
    app_tick(0);
    return rc;
}

const char *sim_sequence_text(int result)
{
    return result <= -100 ? sim_tx_result_text(result) : txseq_result_text(result);
}

int sim_tx_control(int action, int ch, int index)
{
    int guard = controls_guard();
    if (guard) return guard;
    if (ch < 0 || ch >= CH_NUM) return TX_ERR_CHANNEL;
    if (action == 0) {
        if (index < 0 || index >= TX_ENTRIES) return TX_ERR_ENTRY;
        if (txseq_get(ch)->state != TXSEQ_IDLE) return TX_ERR_ACTIVE;
        tx_result_t rc = tx_validate_frame(ch, &tx_get()->e[ch][index].f, true);
        if (rc != TX_OK) return rc;
        evt_post4(EV_REQ_TX_SEND, (uint8_t)ch, SRC_UI, index, 0);
    } else if (action == 1) {
        txseq_stop(ch);
        for (int i = 0; i < TX_ENTRIES; i++)
            evt_post4(EV_REQ_TX_CYC, (uint8_t)ch, SRC_UI, i, 0);
    } else if (action == 2) {
        evt_post4(EV_REQ_TX_STOP_ALL,0,SRC_UI,0,0);
    } else return -101;
    power_activity();
    app_tick(0);
    return 0;
}

int sim_tx_select(int ch, int index)
{
    int rc = ui_tx_select(ch,index);
    if (!rc) power_activity();
    return rc;
}

int sim_tx_scope_select(uint32_t mask)
{
    int rc=ui_tx_scope_select(mask);
    if(!rc)power_activity();
    return rc;
}
int sim_tx_target_select(uint32_t mask)
{
    int rc=ui_tx_target_select(mask);
    if(!rc)power_activity();
    return rc;
}
int sim_tx_channel_send(int ch,int enabled)
{
    int guard=controls_guard();if(guard)return guard;
    if(ch<0||ch>=CH_NUM||enabled<0||enabled>1)return TXSEQ_ERR_ARGUMENT;
    if(ui_page()!=PG_TX)return -104;
    int rc=ui_tx_channel_send(ch,enabled!=0);
    if(rc>=0){power_activity();app_tick(0);}
    return rc;
}
int sim_scope_control(int action,uint32_t mask,uint32_t index)
{
    int guard=controls_guard();if(guard)return guard;
    if(!mask||(mask&~3u)||action<0||action>2||index>=TX_ENTRIES)return TXSEQ_ERR_ARGUMENT;
    int rc=0;
    if(action==0)rc=scope_start(mask);
    else if(action==1)rc=scope_single(mask,index);
    else scope_stop(mask);
    power_activity();app_tick(0);
    return rc;
}
const char *sim_scope_text(int result)
{
    return result<=-100?sim_tx_result_text(result):scope_result_text(result);
}

int sim_local_open(int kind, int ch, int index, uint32_t token)
{
    if (kind < 0 || kind > 5 || ch < 0 || ch >= CH_NUM || index < 0) return UI_REVIEW_RANGE;
    if (kind == 4 && (!token || env_file_token(index) != token)) return LOCAL_FILES_ERR_CHANGED;
    int rc = ui_local_open(kind,ch,index);
    if (!rc) power_activity();
    return rc;
}

int sim_detail_to_tx(int ch, int index)
{
    int rc=ui_detail_to_tx(ch,index);
    if(!rc)power_activity();
    return rc;
}

int sim_plan_open(int mode,int ch)
{
    int rc=ui_plan_open(mode,ch);
    if(!rc)power_activity();
    return rc;
}

int sim_plan_io_peek(sp_io_request_t *request) { return sp_io_peek(request) ? 1 : 0; }
int sim_plan_refresh(void) { return sp_request_refresh(); }
int sim_plan_io_complete(uint32_t id,int result,const char *message) { return sp_io_complete(id,result,message); }
int sim_plan_catalog_replace(const sp_file_t *files,uint32_t count) { return sp_catalog_replace(files,count); }
const char *sim_plan_result_text(int result) { return sp_result_text(result); }

int sim_database_store(const char *name, const local_db_frame_t *frames, uint32_t count)
{
    return local_db_store(name,frames,count);
}

const char *sim_local_result_text(int result)
{
    if (result <= -20) return local_files_result_text(result);
    switch (result) {
    case UI_REVIEW_NOT_READY: return "请先开机并唤醒屏幕";
    case UI_REVIEW_LOCKED: return "请先解除按键锁";
    case UI_REVIEW_DIALOG: return "请先处理屏幕中的确认提示";
    case UI_REVIEW_DRAFT: return "请先保存或取消本地编辑草稿";
    default: return "本地菜单当前不可用或参数超出范围";
    }
}

int sim_sequence_defaults_all(uint32_t first_revision, uint32_t second_revision)
{
    int guard = controls_guard();
    if (guard) return guard;
    uint32_t revisions[CH_NUM] = {first_revision,second_revision};
    for (int ch = 0; ch < CH_NUM; ch++) {
        const txseq_channel_t *q = txseq_get(ch);
        if (q->revision != revisions[ch]) return TXSEQ_ERR_CHANGED;
        if (q->state != TXSEQ_IDLE) return TXSEQ_ERR_ACTIVE;
    }
    for (int ch = 0; ch < CH_NUM; ch++) txseq_defaults(ch,revisions[ch]);
    power_activity();
    return 0;
}

int sim_tx_batch(int ch, const sim_tx_update_t *rows, int count)
{
    int guard = controls_guard();
    if (guard) return guard;
    if (ch < 0 || ch >= CH_NUM) return TX_ERR_CHANNEL;
    if (!rows || count < 1 || count > TX_ENTRIES) return TX_ERR_ENTRY;
    if (txseq_get(ch)->state != TXSEQ_IDLE) return TX_ERR_ACTIVE;
    unsigned seen = 0;
    for (int i = 0; i < count; i++) {
        const sim_tx_update_t *row = &rows[i];
        if (row->index < 0 || row->index >= TX_ENTRIES || (seen & (1u << row->index))) return TX_ERR_ENTRY;
        seen |= 1u << row->index;
        if (row->flags < 0 || row->flags > UINT8_MAX || row->len < 0 || row->len > 64) return -101;
        const tx_entry_t *old = &tx_get()->e[ch][row->index];
        if (old->revision != row->revision) return TX_ERR_CHANGED;
        if (old->active) return TX_ERR_ACTIVE;
        if (row->period_ms > UINT16_MAX) return TX_ERR_PERIOD;
        can_frame_t frame = {.id=row->id, .ch=(uint8_t)ch, .flags=(uint8_t)row->flags, .len=(uint8_t)row->len};
        tx_result_t rc = tx_validate_frame(ch, &frame, false);
        if (rc != TX_OK) return rc;
    }
    /* Fully validated, single runtime lock: no event pumping between commits. */
    for (int i = 0; i < count; i++) {
        const sim_tx_update_t *row = &rows[i];
        tx_entry_update(ch, row->index, row->id, (uint8_t)row->flags,
                        (uint8_t)row->len, row->data, row->period_ms);
    }
    power_activity();
    return 0;
}

int sim_record_name(const char *prefix)
{
    int guard = controls_guard();
    return guard ? guard : env_rec_name_set(prefix);
}

int sim_log_rename(int index, uint32_t token, const char *name)
{
    int guard = controls_guard();
    return guard ? guard : env_log_rename(index, token, name);
}

int sim_log_name(int index, uint32_t token, char *buf, uint32_t cap)
{
    file_info_t info;
    if (!token || env_file_token(index) != token) return ENV_LOG_ERR_CHANGED;
    if (!buf || !cap || !port_sd_file_info(index, &info)) return ENV_LOG_ERR_ARGUMENT;
    if (strlen(info.name) >= cap) return ENV_LOG_ERR_LIMIT;
    snprintf(buf, cap, "%s", info.name);
    return 0;
}

int sim_tx_entry(int ch, int idx, uint32_t id, int flags, int len, const uint8_t *data, uint32_t period_ms, uint32_t expected_revision)
{
    if (!editable()) return -100;
    if (ui_dialog_open()) return -102;
    if (ch < 0 || ch >= CH_NUM) return TX_ERR_CHANNEL;
    if (idx < 0 || idx >= TX_ENTRIES) return TX_ERR_ENTRY;
    if (tx_get()->e[ch][idx].revision != expected_revision) return TX_ERR_CHANGED;
    if (flags < 0 || flags > UINT8_MAX || len < 0 || len > 64) return -101;
    return tx_entry_update(ch, idx, id, (uint8_t)flags, (uint8_t)len, data, period_ms);
}

const char *sim_tx_result_text(int result)
{
    if (result == -100) return "请先开机并解除按键锁";
    if (result == -102) return "请先在设备屏幕处理当前确认提示";
    if (result == -103) return "请先保存或取消设备屏幕上的编辑草稿";
    if (result < 0) return "参数无效";
    return tx_result_text((tx_result_t)result);
}

int sim_tx_edit(int ch, int idx)
{
    if (!editable()) return -100;
    if (ui_dialog_open()) return -102;
    if (ui_draft_open()) return -103;
    if (!ui_tx_edit_open(ch, idx)) return -101;
    power_wake();
    power_activity();
    return 0;
}

int sim_filter(int channel, int type, uint32_t id, uint32_t mask, int direction, int format)
{
    if (!editable()) return -100;
    disp_filter_t f = { .channel = (disp_ch_filter_t)channel, .type = (disp_type_filter_t)type,
                       .id = id, .mask = mask, .direction = (disp_direction_filter_t)direction,
                       .format = (disp_format_filter_t)format };
    return disp_filter_set_ex(&f) ? 0 : -101;
}

int sim_ui_action(int action, int value, uint32_t sequence, int use_sequence)
{
    if (action < 0 || action > 6 || (action != 6 && value < 0) || (use_sequence != 0 && use_sequence != 1))
        return UI_REVIEW_RANGE;
    int result;
    switch (action) {
    case 0:
        if (value >= PG_NUM) return UI_REVIEW_RANGE;
        result = ui_review_navigate((page_t)value);
        break;
    case 1:
        if (value > 1) return UI_REVIEW_RANGE;
        result = ui_live_set_view((unsigned)value);
        break;
    case 2:
        if (value > 1) return UI_REVIEW_RANGE;
        result = ui_live_set_paused(value != 0);
        break;
    case 3:
        result = use_sequence ? ui_live_open_detail_seq(sequence) : ui_live_open_detail((unsigned)value);
        break;
    case 4:
        result = ui_review_filter_open();
        break;
    case 5:
        if(value<1||value>2)return UI_REVIEW_RANGE;
        result=ui_live_set_channel((unsigned)value);
        break;
    default:
        result=ui_live_scroll(value);
        break;
    }
    if (result == UI_REVIEW_OK) power_activity();
    return result;
}

/* These are visible device controls, not remote-host commands. Explicit stop
 * closes the current session; only a subsequent start creates another one. */
int sim_rec_action(int action)
{
    if (action < 0 || action > 2) return UI_REVIEW_RANGE;
    const power_t *p = power_get();
    if (!power_running() || !p->screen_on) return UI_REVIEW_NOT_READY;
    if (p->locked) return UI_REVIEW_LOCKED;
    if (ui_dialog_open()) return UI_REVIEW_DIALOG;
    if (ui_draft_open()) return UI_REVIEW_DRAFT;
    rec_state_t state = rec_get()->state;
    if ((action == 0 && state != REC_IDLE) ||
        (action == 1 && state != REC_PREPARING && state != REC_RUNNING) ||
        (action == 2 && state != REC_RUNNING)) return -8;
    if (action == 0) {
        msg_t why;
        if (!storage_writable(&why)) return -9;
        unsigned samples_used = 0;
        for (int i = 0; i < port_sd_file_count(); i++) {
            file_info_t info;
            if (port_sd_file_info(i,&info) && info.sample_available) samples_used++;
        }
        if (samples_used >= ENV_LOG_MAX_FILES || port_sd_file_count() >= 24) return -10;
    }
    static const ev_type_t types[] = { EV_REQ_REC_START, EV_REQ_REC_STOP, EV_REQ_REC_MARK };
    evt_post4(types[action], 0, SRC_UI, 0, 0);
    power_activity();
    app_tick(0);
    if(ui_page()==PG_REC&&action<2)(void)ui_review_navigate(PG_LIVE);
    return UI_REVIEW_OK;
}

int sim_log_import(const char *name, const char *text, uint32_t size, uint32_t *line, uint32_t *frames)
{
    env_log_result_t result = {0};
    int idx = env_log_import(name, text, size, &result);
    if (line) *line = result.error_line;
    if (frames) *frames = result.frames;
    return idx;
}

int sim_log_export(int file, char *buf, uint32_t cap)
{
    size_t needed;
    return env_log_export(file, buf, cap, &needed);
}

const char *sim_log_error_text(int result)
{
    return env_log_error_name(result);
}

static void work_json(jw_t *j)
{
    jw_key(j, "work"); jw_obj(j);
    sp_json(j);
    scope_json(j);
    jw_kstr(j, "rec_name", env_rec_name_get());
    jw_kint(j, "local_database_count", local_db_count());
    jw_kint(j, "local_database_max_frames", LOCAL_DB_MAX_FRAMES);
    jw_kint(j, "prefix_max_bytes", 18); jw_kint(j, "name_max_bytes", 47);
    jw_key(j, "sequence"); jw_arr(j);
    for (int ch = 0; ch < CH_NUM; ch++) {
        const txseq_channel_t *q = txseq_get(ch);
        jw_obj(j);
        jw_kint(j, "revision", q->revision); jw_kint(j, "state", q->state);
        jw_kint(j, "id_gap_ms", q->id_gap_ms); jw_kint(j, "current_entry", q->current_entry);
        jw_kint(j, "occurrence", q->occurrence); jw_kint(j, "rounds", q->rounds);
        jw_kint(j, "sent", q->sent); jw_kint(j, "missed", q->missed);
        jw_kstr(j, "stop_reason", txseq_stop_text(q->stop_reason));
        jw_key(j, "entries"); jw_arr(j);
        for (int i = 0; i < TX_ENTRIES; i++) {
            const txseq_entry_config_t *row = &q->entries[i];
            jw_obj(j); jw_kbool(j, "enabled", row->enabled);
            jw_kint(j, "repeat_count", row->repeat_count); jw_kint(j, "frame_gap_ms", row->frame_gap_ms);
            jw_kint(j, "cycle_ms", row->cycle_ms); jw_end_obj(j);
        }
        jw_end_arr(j); jw_end_obj(j);
    }
    jw_end_arr(j);
    jw_key(j, "tx"); jw_arr(j);
    for (int ch = 0; ch < CH_NUM; ch++) {
        jw_arr(j);
        for (int i = 0; i < TX_ENTRIES; i++) {
            const tx_entry_t *e = &tx_get()->e[ch][i];
            char data[3 * 64 + 1] = "";
            if (!(e->f.flags & CANF_RTR))
                for (int b = 0; b < e->f.len; b++)
                    snprintf(data + b * 3, sizeof data - (size_t)b * 3, "%02X%s", e->f.data[b], b + 1 < e->f.len ? " " : "");
            jw_obj(j);
            jw_kint(j, "id", e->f.id); jw_kint(j, "flags", e->f.flags);
            jw_kint(j, "len", e->f.len); jw_kstr(j, "data", data);
            jw_kint(j, "period", e->period_ms); jw_kbool(j, "active", e->active);
            jw_kint(j, "revision", e->revision);
            jw_kint(j, "sent", e->sent); jw_kint(j, "missed", e->missed);
            jw_end_obj(j);
        }
        jw_end_arr(j);
    }
    jw_end_arr(j);
    disp_filter_t f; disp_filter_get_ex(&f);
    jw_key(j, "filter"); jw_obj(j);
    jw_kint(j, "channel", f.channel); jw_kint(j, "type", f.type);
    jw_kint(j, "id", f.id); jw_kint(j, "mask", f.mask);
    jw_kint(j, "direction", f.direction); jw_kint(j, "format", f.format);
    jw_end_obj(j);
    static const char *const kinds[] = { "CFR", "ASC", "candump", "发送列表", "未知" };
    jw_key(j, "files"); jw_arr(j);
    for (int i = 0; i < port_sd_file_count(); i++) {
        file_info_t fi;
        if (!port_sd_file_info(i, &fi)) continue;
        jw_obj(j); jw_kint(j, "index", i); jw_kstr(j, "name", fi.name);
        jw_kint(j, "token", env_file_token(i));
        jw_kbool(j, "renamable", !fi.writing && !fi.protected_ && fi.sample_available);
        jw_kstr(j, "kind", fi.kind <= FK_UNKNOWN ? kinds[fi.kind] : "未知");
        jw_kint(j, "frames", fi.frames); jw_kint(j, "channels", fi.channels);
        jw_kint(j, "duration_ms", fi.duration_ms);
        jw_kbool(j, "sample_available", fi.sample_available);
        jw_kbool(j, "is_session", fi.is_session);
        jw_kbool(j, "writing", fi.writing);
        jw_kbool(j, "imported", fi.imported);
        jw_kbool(j, "sample_truncated", fi.sample_truncated);
        jw_kint(j, "sample_frames", fi.sample_frames); jw_kint(j, "sample_dropped", fi.sample_dropped);
        jw_end_obj(j);
    }
    jw_end_arr(j);
    jw_key(j, "limits"); jw_obj(j);
    jw_kint(j, "files", ENV_LOG_MAX_FILES); jw_kint(j, "frames", ENV_LOG_MAX_FRAMES);
    jw_kint(j, "text_bytes", ENV_LOG_MAX_TEXT); jw_end_obj(j);
    jw_end_obj(j);
}

int sim_snapshot(char *buf, int cap)
{
    if (!buf || cap <= 0) return -1;
    jw_t j;
    jw_init(&j, buf, (size_t)cap);
    jw_obj(&j);
    jw_key(&j, "dev");
    app_json(&j);
    jw_key(&j, "env");
    env_json(&j);
    work_json(&j);
    jw_end_obj(&j);
    return j.overflow ? -1 : (int)jw_finish(&j);
}
