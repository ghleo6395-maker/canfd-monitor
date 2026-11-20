/* Regression coverage for shared device/host editing and display-only filters.
 * Build this as a separate executable with the same CORE_SRC/SIM_SRC as test_sm. */
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/disp/disp_pipe.h"
#include "services/frame_ring/frame_ring.h"
#include "services/player/play_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/sim_api.h"
#include "sim/local_files_ui.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void run(int ms) { for (int i = 0; i < ms; i += 10) sim_tick(10); }
static void post(ev_type_t type, int ch, int a, int b)
{ evt_post4(type, (uint8_t)ch, SRC_UI, a, b); run(20); }
static void key(key_id_t k) { post(EV_KEY, k, GEST_SHORT, 0); }
static void enter_value(const char *value){CHECK(local_files_keyboard_replace(local_files_keyboard_token(),value)==0);local_files_touch(local_files_item_count()-2);}
static void boot(void)
{
    sim_reset(); sim_key(KEY_POWER, 1); run(1200); sim_key(KEY_POWER, 0); run(500);
}

static void frame_validation(void)
{
    boot();
    can_frame_t f = { .id = 0x7FF, .len = 8 };
    CHECK(tx_validate_frame(0, &f, false) == TX_OK);
    f.id = 0x800; CHECK(tx_validate_frame(0, &f, false) == TX_ERR_ID);
    f.flags = CANF_EXT; f.id = 0x1FFFFFFF; CHECK(tx_validate_frame(1, &f, false) == TX_OK);
    f.id++; CHECK(tx_validate_frame(1, &f, false) == TX_ERR_ID);
    f.id = 0x123;
    for (int i = 0; i <= 64; i++) {
        f.len = (uint8_t)i;
        f.flags = 0;
        CHECK((tx_validate_frame(0, &f, false) == TX_OK) == (i <= 8));
        f.flags = CANF_FD | CANF_BRS;
        bool legal = i <= 8 || i == 12 || i == 16 || i == 20 || i == 24 || i == 32 || i == 48 || i == 64;
        CHECK((tx_validate_frame(0, &f, false) == TX_OK) == legal);
    }
    f.len = 8; f.flags = CANF_BRS; CHECK(tx_validate_frame(0, &f, true) == TX_ERR_FLAGS);
    f.flags = CANF_FD | CANF_RTR; CHECK(tx_validate_frame(0, &f, true) == TX_ERR_FLAGS);
    f.flags = CANF_TX; CHECK(tx_validate_frame(0, &f, false) == TX_ERR_FLAGS);
    f.flags = CANF_ERR; CHECK(tx_validate_frame(0, &f, false) == TX_ERR_FLAGS);
    f.flags = CANF_ESI; CHECK(tx_validate_frame(0, &f, false) == TX_ERR_FLAGS);
    f.flags = CANF_FD; cfg_mut()->fd[0] = false;
    CHECK(tx_validate_frame(0, &f, false) == TX_OK);
    CHECK(tx_validate_frame(0, &f, true) == TX_ERR_FD_DISABLED);
    CHECK(tx_validate_frame(-1, &f, true) == TX_ERR_CHANNEL);
    CHECK(tx_validate_frame(2, &f, true) == TX_ERR_CHANNEL);
    CHECK(tx_validate_frame(0, NULL, true) == TX_ERR_NULL);
    CHECK(!tx_submit(-1, &f, TXSRC_HOST));
    CHECK(!tx_submit(0, NULL, TXSRC_HOST));
}

static void atomic_edit_and_send(void)
{
    boot();
    uint8_t data[64]; for (int i = 0; i < 64; i++) data[i] = (uint8_t)(255 - i);
    tx_entry_t before = tx_get()->e[0][0];
    CHECK(tx_entry_update(0, 0, 0x800, 0, 8, data, 100) == TX_ERR_ID);
    CHECK(memcmp(&before, &tx_get()->e[0][0], sizeof before) == 0);
    CHECK(tx_entry_update(0, 0, 0x123, 0, 8, data, 65536) == TX_ERR_PERIOD);
    CHECK(tx_entry_update(0, 0, 0x123, 0, 8, NULL, 100) == TX_ERR_NULL);
    CHECK(tx_entry_update(0, 4, 0x123, 0, 8, data, 100) == TX_ERR_ENTRY);
    CHECK(tx_entry_update(-1, 0, 0x123, 0, 8, data, 100) == TX_ERR_CHANNEL);
    CHECK(memcmp(&before, &tx_get()->e[0][0], sizeof before) == 0);
    CHECK(tx_entry_update(0, 0, 0x1ABCDEFF, CANF_EXT | CANF_FD | CANF_BRS, 64, data, 65535) == TX_OK);
    const tx_entry_t *e = &tx_get()->e[0][0];
    CHECK(e->f.id == 0x1ABCDEFF && e->f.len == 64 && e->period_ms == 65535);
    CHECK(memcmp(e->f.data, data, 64) == 0);
    CHECK(e->dyn.cnt_bits == 0 && e->dyn.chk == CHK_NONE && e->revision == 1);
    /* Runtime channel capability must reject a stored FD entry before asking
     * to switch mode, and it must reject direct host/replay submissions too. */
    post(EV_REQ_CH_FD, 0, 0, 0);
    post(EV_REQ_TX_SEND, 0, 0, 0);
    CHECK(tx_last_result(0) == TX_ERR_FD_DISABLED);
    CHECK(!ui_dialog_open() && tx_get()->pending[0] == 0);
    CHECK(!tx_submit(0, &e->f, TXSRC_REPLAY));
    post(EV_REQ_CH_FD, 0, 1, 0);
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 2);
    post(EV_REQ_TX_CYC, 0, 0, 3);
    CHECK(tx_get()->e[0][0].active);
    before = tx_get()->e[0][0];
    CHECK(tx_entry_update(0, 0, 0x321, 0, 8, data, 100) == TX_ERR_ACTIVE);
    CHECK(memcmp(&before, &tx_get()->e[0][0], sizeof before) == 0);
    post(EV_REQ_TX_CYC, 0, 0, 0);
    CHECK(tx_entry_update(0, 0, 0x321, CANF_RTR, 8, NULL, 0) == TX_OK);
    CHECK(tx_get()->e[0][0].f.data[0] == 0 && tx_get()->e[0][0].period_ms == 0);
}

static void device_editor(void)
{
    boot();
    sim_touch(0, 1); CHECK(ui_page() == PG_TX);
    sim_key(KEY_OK, 1); run(1100); sim_key(KEY_OK, 0); run(20);
    CHECK(ui_page() == PG_TXEDIT && !ui_dialog_open());
    CHECK(tx_get()->e[0][0].sent == 0); /* long OK opens; it must never transmit */
    sim_touch(0, 0); enter_value("124");
    sim_touch(0, 10);
    CHECK(ui_page() == PG_TX && tx_get()->e[0][0].f.id == 0x124);
    CHECK(tx_get()->e[0][0].dyn.chk == CHK_NONE);
    sim_touch(0, 0); CHECK(ui_page() == PG_TXEDIT); /* touch is edit, not send */
    sim_touch(0, 0); CHECK(local_files_keyboard_replace(local_files_keyboard_token(),"125")==0);key(KEY_BACK); /* Cancel field input. */
    sim_touch(0, 10); CHECK(tx_get()->e[0][0].f.id == 0x124);
    CHECK(ui_tx_edit_open(0, 2));
    sim_touch(0, 7);enter_value("63");
    sim_touch(0, 8);enter_value("40");
    sim_touch(0, 10);
    CHECK(tx_get()->e[0][2].f.data[63] == 0x40 && tx_get()->e[0][2].f.len == 64);
    CHECK(ui_tx_edit_open(0, 0));
    sim_touch(0, 0);enter_value("125");
    sim_touch(1, 0); CHECK(ui_page() == PG_TX);
    CHECK(tx_get()->e[0][0].f.id == 0x124); /* BACK discards draft */
    CHECK(ui_tx_edit_open(0, 0));
    uint8_t data[8] = { 0xAA };
    CHECK(tx_entry_update(0, 0, 0x456, 0, 8, data, 10) == TX_OK);
    sim_touch(0, 10);
    CHECK(ui_page() == PG_TXEDIT && tx_get()->e[0][0].f.id == 0x456);
    CHECK(strstr(ui_last_toast(), "其他界面") != NULL);
    ui_tx_edit_cancel();
    CHECK(!ui_tx_edit_open(2, 0) && !ui_tx_edit_open(0, 4));
    ui_filter_open(); CHECK(ui_page() == PG_FILTER);
    sim_touch(0, 0); sim_touch(0, 4); sim_touch(0, 5); sim_touch(0, 6);
    disp_filter_t f; disp_filter_get_ex(&f);
    CHECK(ui_page() == PG_LIVE && f.channel == DF_CH_1 && f.direction == DF_DIR_RX && f.format == DF_FMT_STD);
    static char buf[65536]; int n = sim_snapshot(buf, sizeof buf);
    CHECK(n > 100 && buf[n - 1] == '}' && strstr(buf, "filter_cfg") != NULL);
}

static void editor_reopen_preserves_draft(void)
{
    boot();
    CHECK(ui_tx_edit_open(0, 0));
    sim_touch(0, 0);enter_value("124");
    CHECK(!ui_tx_edit_open(0, 0));
    CHECK(!ui_tx_edit_open(1, 1));
    sim_touch(0, 10);
    CHECK(ui_page() == PG_TX && tx_get()->e[0][0].f.id == 0x124);
    key(KEY_OK);
    CHECK(ui_dialog_open());
    CHECK(!ui_tx_edit_open(0, 0) && ui_page() == PG_TX);
}

static void confirmation_binds_entry_revision(void)
{
    for (int kind = 0; kind < 4; kind++) {
        boot();
        sim_touch(0, 1); /* Home opens the send list directly. */
        if (kind >= 2) post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 1);
        key((kind & 1) ? KEY_RIGHT : KEY_OK);
        CHECK(ui_dialog_ask() == ((kind & 1) ? (kind >= 2 ? ASK_TX_FIRST : ASK_CYC_NORMAL) : ASK_SCOPE_START));
        uint8_t data[8] = { 0xBB };
        /* Direct shared-C edits must invalidate a pending UI confirmation even
         * when a transport wrapper also blocks edits while dialogs are open. */
        CHECK(tx_entry_update(0, 0, 0x456, 0, 8, data, 100) == TX_OK);
        sim_touch(2, 0);
        CHECK(!ui_dialog_open() && !tx_get()->e[0][0].active);
        CHECK(tx_get()->e[0][0].sent == 0 && tx_get()->pending[0] == 0);
        CHECK(strstr(ui_last_toast(),(kind&1)?"重新检查报文":"配置或报文已变化") != NULL);
        CHECK(kind >= 2 ? chan_get(0)->first_tx_pending : chan_get(0)->state == CH_LISTEN);
        /* The user can deliberately issue a fresh request for the edited frame. */
        key(KEY_OK);
        CHECK(ui_dialog_ask() == ASK_SCOPE_START);
        sim_touch(2, 0);
        run(20); /* Scope commits only after the selected channels are ready. */
        CHECK(tx_get()->e[0][0].sent == 1);
    }
}

static void play_selection_navigation(void)
{
    boot();
    CHECK(!ui_play_select(-1, 0) && !ui_play_select(2, 0));
    CHECK(!ui_play_select(0, -1) && !ui_play_select(0, 24));
    CHECK(ui_tx_edit_open(0, 0));
    CHECK(!ui_play_select(1, 0) && ui_page() == PG_TXEDIT);
    ui_tx_edit_cancel();
    CHECK(!ui_play_select(1, 0) && ui_page() == PG_TX);
    CHECK(ui_plan_open(3,0)==UI_REVIEW_RANGE && !play_any_active());
    CHECK(ui_review_navigate(PG_LOGPLAY)==UI_REVIEW_RANGE && !ui_dialog_open());
}

static void display_only_filters(void)
{
    boot();
    /* Isolate the broadcast ring, retaining a raw lossless reader as recorder
     * surrogate. The display consumer must not alter any source frame. */
    ring_init(); disp_init();
    ring_reader_t raw; ring_reader_attach(&raw, true);
    disp_filter_t f = { .channel = DF_CH_1, .id = 0x120, .mask = 0x7F0,
        .direction = DF_DIR_RX, .format = DF_FMT_STD };
    CHECK(disp_filter_set_ex(&f));
    can_frame_t frames[] = {
        { .id = 0x123, .ch = 0, .len = 8, .data = { 0xAA } },
        { .id = 0x124, .ch = 1, .len = 8 },
        { .id = 0x125, .ch = 0, .flags = CANF_TX, .len = 8 },
        { .id = 0x123, .ch = 0, .flags = CANF_EXT, .len = 8 },
        { .id = 0x139, .ch = 0, .len = 8 }
    };
    for (int i = 0; i < 5; i++) { frames[i].ts_us = (uint64_t)(i + 1) * 1000; CHECK(ring_push(&frames[i])); }
    disp_tick();
    CHECK(disp_rows_available() == 1 && disp_id_count() == 1);
    disp_row_t row; CHECK(disp_row(0, &row) && row.id == 0x123 && row.data[0] == 0xAA);
    for (int i = 0; i < 5; i++) {
        const can_frame_t *r = ring_peek(&raw);
        CHECK(r && memcmp(r, &frames[i], sizeof *r) == 0); ring_advance(&raw);
    }
    CHECK(!ring_peek(&raw));
    disp_filter_t invalid = f; invalid.mask = 0x20000000;
    CHECK(!disp_filter_set_ex(&invalid) && disp_rows_available() == 1);
    invalid = f; invalid.channel = DF_CH_NUM; CHECK(!disp_filter_set_ex(&invalid));
    CHECK(!disp_filter_set_ex(NULL));
    f.channel = DF_CH_ALL; f.direction = DF_DIR_TX; f.format = DF_FMT_ALL;
    CHECK(disp_filter_set_ex(&f));
    CHECK(disp_rows_available() == 0 && disp_id_count() == 0);
    CHECK(ring_push(&frames[2])); disp_tick();
    CHECK(disp_rows_available() == 1 && disp_id_count() == 1);
    memset(&f, 0, sizeof f); CHECK(disp_filter_set_ex(&f));
    CHECK(ring_push(&frames[0]) && ring_push(&frames[3])); disp_tick();
    CHECK(disp_id_count() == 2); /* equal numeric STD/EXT IDs remain distinct */
    disp_pause(true); uint32_t n = disp_rows_available();
    CHECK(ring_push(&frames[0])); disp_tick();
    CHECK(disp_rows_available() == n && disp_hidden_while_paused() == 1);
}

int main(void)
{
    frame_validation(); atomic_edit_and_send(); device_editor(); editor_reopen_preserves_draft(); confirmation_binds_entry_revision(); play_selection_navigation(); display_only_filters();
    printf("editor/filter: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
