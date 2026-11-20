/* Host integration tests for candump adaptation, bounded storage and replay.
 * Build with every service C file plus app/app.c, app/ui/core/ui_sm.c and sim C files,
 * -I. -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter and ASan/UBSan. */
#include "app/app.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/config/cfg.h"
#include "services/bench/bench.h"
#include "services/frame_ring/frame_ring.h"
#include "services/logfmt/candump.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
static void run(int ms) { while (ms > 0) { int step = ms > 10 ? 10 : ms; sim_tick(step); ms -= step; } }
static void boot(void)
{
    sim_reset(); sim_key(KEY_POWER, 1); run(1200); sim_key(KEY_POWER, 0); run(520);
    env_cmd("bus_fps", 0, 0); env_cmd("bus_fps", 1, 0);
}
static void request(ev_type_t type, int ch, int a, int b)
{
    evt_post4(type, (uint8_t)ch, SRC_UI, a, b); app_tick(0);
}
static int import(const char *text)
{
    env_log_result_t result;
    char name[40];
    snprintf(name, sizeof name, "sample-%d.log", port_sd_file_count());
    int idx = env_log_import(name, text, strlen(text), &result);
    CHECK(idx >= 0 && result.status == ENV_LOG_OK);
    return idx;
}
static void start(int ch, int idx)
{
    request(EV_REQ_PLAY_OPEN, ch, idx, 0);
    CHECK(play_get(ch)->asking == ASK_PLAY_NORMAL);
    request(EV_REQ_PLAY_ANSWER, ch, ASK_PLAY_NORMAL, 1);
    CHECK(play_get(ch)->state == PL_PLAYING || play_get(ch)->state == PL_IDLE);
}

static void codec(void)
{
    const char *valid[] = {
        "(0.000001) can0 123#1122",
        "(17.1) can1 00000001#A5",
        "(17.123456) can0 123#R8",
        "(17.123456) can0 123#R",
        "(17.123456) can0 123##3AABBCCDDEEFF001122334455",
        "(17.123456) can0 123##4",
        "(17.123456) can15 20000004#0102030405060708",
        " (18446744073709.551615) can0 7FF#00.11.22.33.44.55.66.77\r\n"
    };
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) {
        can_frame_t f, copy; char encoded[LOGFMT_MAX_LINE + 1u];
        CHECK(logfmt_parse_line(valid[i], strlen(valid[i]), &f) == LOGFMT_OK);
        CHECK(logfmt_format_line(&f, encoded, sizeof encoded) > 0);
        CHECK(logfmt_line_size(&f) == (int)strlen(encoded));
        CHECK(logfmt_parse_line(encoded, strlen(encoded), &copy) == LOGFMT_OK);
        CHECK(f.id == copy.id && f.ts_us == copy.ts_us && f.ch == copy.ch && f.flags == copy.flags && f.len == copy.len);
        CHECK(!memcmp(f.data, copy.data, sizeof f.data));
        char small[4] = "xx";
        CHECK(logfmt_format_line(&f, small, sizeof small) == LOGFMT_ERR_SPACE && !small[0]);
    }
    const char *bad[] = {
        "(0.0) can0 800#00", "(0.0) can0 20000000##0", "(0.0) can0 123#R9",
        "(0.0) can0 123#R2AA", "(0.0) can0 123#010203040506070809",
        "(0.0) can0 123##1010203040506070809", "(0.0) can0 123##8",
        "(0.0) can0 123##", "(0.0) can0 123#A", "(0.0) can0 123#..AA",
        "(0.0) can0 123#AA.", "(0.0) can0 123#AA garbage", "(0.0) can0 123#0000000000000000_F",
        "(0.0000001) can0 123#", "(-1.0) can0 123#", "(1.) can0 123#", "(1.0)can0 123#",
        "(1.0) can16 123#", "(1.0) can0 80000000#", "(18446744073709.551616) can0 123#"
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        can_frame_t out = { .id = 0xDEADBEEF };
        CHECK(logfmt_parse_line(bad[i], strlen(bad[i]), &out) < 0 && out.id == 0xDEADBEEF);
    }
    can_frame_t f;
    CHECK(logfmt_parse_line("  # comment", 11, &f) == LOGFMT_SKIP);
    CHECK(logfmt_parse_line("\r\n\t", 3, &f) == LOGFMT_SKIP);
    for (unsigned len = 0; len <= 65; len++) {
        f = (can_frame_t){ .id = 0x123, .len = (uint8_t)len, .flags = CANF_FD };
        bool legal = len <= 8 || len == 12 || len == 16 || len == 20 || len == 24 || len == 32 || len == 48 || len == 64;
        CHECK(logfmt_frame_valid(&f) == legal);
        if (legal) {
            char encoded[257]; can_frame_t copy;
            for (unsigned j = 0; j < len; j++) f.data[j] = (uint8_t)(j + 0x80);
            CHECK(logfmt_format_line(&f, encoded, sizeof encoded) > 0);
            CHECK(logfmt_line_size(&f) == (int)strlen(encoded));
            CHECK(logfmt_parse_line(encoded, strlen(encoded), &copy) == LOGFMT_OK && copy.len == len && !memcmp(copy.data, f.data, len));
        }
    }
    CHECK(logfmt_line_size(NULL) == LOGFMT_ERR_RANGE);
    /* Decimal timestamp/channel widths and every Classic/FD/RTR/error form. */
    for (unsigned variant = 0; variant < 6; ++variant) {
        const uint8_t flags[] = {0, CANF_EXT, CANF_FD|CANF_BRS, CANF_RTR, CANF_ERR, CANF_FD|CANF_EXT|CANF_ESI};
        for (unsigned ch = 0; ch < 16; ++ch) {
            for (unsigned time = 0; time < 3; ++time) {
                const uint64_t times[] = {0, UINT64_C(10000000), UINT64_MAX};
                f = (can_frame_t){.id = 0x123, .flags = flags[variant], .ch = ch,
                    .len = (flags[variant] & CANF_FD) ? 64 : 8, .ts_us = times[time]};
                char encoded[257];
                CHECK(logfmt_line_size(&f) == logfmt_format_line(&f, encoded, sizeof encoded));
            }
        }
    }
}

static void recording_text_accounting(void)
{
    boot();
    bench_config_t config = {64, 0, 0};
    CHECK(!strncmp(bench_execute(BENCH_CONFIG, &config), "OK", 2));
    CHECK(!strncmp(bench_execute(BENCH_ON, NULL), "OK", 2));
    request(EV_REQ_REC_START, 0, 0, 0); run(200);
    CHECK(rec_get()->state == REC_RUNNING);
    uint64_t expected = 0, segment_bytes = 0;
    uint32_t segment = 1;
    for (unsigned i = 0; i < 500; ++i) {
        can_frame_t frame = {.ch = i & 1u, .id = 0x123, .flags = CANF_FD|CANF_BRS,
            .len = 64, .ts_us = i * UINT64_C(1000000)};
        char line[257];
        unsigned bytes = (unsigned)logfmt_format_line(&frame, line, sizeof line);
        CHECK(ring_push(&frame));
        app_capture_poll();
        expected += bytes; segment_bytes += bytes;
        if (segment_bytes >= 65536u) { ++segment; segment_bytes = 0; }
        CHECK(rec_get()->bytes == expected && rec_get()->seg_bytes == segment_bytes);
        CHECK(rec_get()->seg == segment);
    }
    CHECK(rec_get()->frames == 500 && rec_get()->seg == 2 && rec_get()->drops == 0);
    request(EV_REQ_REC_STOP, 0, 0, 1); run(300);
    CHECK(rec_get()->state == REC_IDLE);
}

static void bounded_import(void)
{
    boot();
    const char *trace = "# candump -L\n(10.000001) can0 123#1122\n(10.125001) can1 00000456#A5\n"
        "(10.250001) can0 18FF50E5##1112233445566778899AABBCC\n(10.300001) can2 100#R8\n(10.750001) can0 20000004#01\n";
    int idx = import(trace); CHECK(idx == 5);
    file_info_t fi; CHECK(port_sd_file_info(idx, &fi));
    CHECK(fi.imported && fi.sample_available && fi.sample_frames == 5 && fi.frames == 5 && fi.channels == 3 && fi.fd && !fi.nbr);
    uint32_t count; uint64_t duration;
    CHECK(port_sd_replay_info(idx, 0, &count, &duration) && count == 3 && duration == 750000);
    can_frame_t f;
    CHECK(port_sd_read_frame(idx, 0, 1, &f) && f.id == 0x18FF50E5 && f.ts_us == 250000 && f.len == 12 && f.data[11] == 0xCC);
    CHECK(port_sd_replay_info(idx, 1, &count, &duration) && count == 1 && duration == 125000);
    CHECK(!port_sd_read_frame(idx, 1, 1, &f));
    char dump[2048]; size_t needed;
    CHECK(env_log_export(idx, dump, 8, &needed) == ENV_LOG_ERR_SPACE && !dump[0] && needed > 8);
    CHECK(env_log_export(idx, dump, sizeof dump, &needed) > 0 && needed == strlen(dump) + 1u);
    CHECK(import(dump) == 6);
    env_log_result_t result;
    const char *bad = "(0.0) can0 123#11\n(0.1) can0 123#Z\n";
    CHECK(env_log_import("bad.log", bad, strlen(bad), &result) == ENV_LOG_ERR_FORMAT && result.error_line == 2 && result.frames == 1);
    CHECK(port_sd_file_count() == 7);
    const char *reverse = "(2.0) can0 123#\n(1.0) can0 123#\n";
    CHECK(env_log_import("bad.log", reverse, strlen(reverse), &result) == ENV_LOG_ERR_TIMESTAMP && result.error_line == 2);
    CHECK(env_log_import("../bad.log", trace, strlen(trace), &result) == ENV_LOG_ERR_ARGUMENT);
    static char huge[32768]; size_t used = 0;
    for (unsigned i = 0; i <= ENV_LOG_MAX_FRAMES; i++) used += (size_t)snprintf(huge + used, sizeof huge - used, "(%u.0) can0 123#AA\n", i);
    CHECK(env_log_import("too-many.log", huge, used, &result) == ENV_LOG_ERR_LIMIT && result.error_line == ENV_LOG_MAX_FRAMES + 1u);
    CHECK(port_sd_file_count() == 7);
    /* Two retained imports already exist. Fill the remaining configured sample
     * budget, rather than assuming the simulator only has four sample slots. */
    for (unsigned sample = 2; sample < ENV_LOG_MAX_FILES; sample++)
        CHECK(import(sample & 1u ? "(0.0) can1 123#BB\n" : "(0.0) can0 123#AA\n") == 5 + (int)sample);
    const int full_count = 5 + (int)ENV_LOG_MAX_FILES;
    CHECK(env_log_import("full.log", trace, strlen(trace), &result) == ENV_LOG_ERR_NO_SLOT && port_sd_file_count() == full_count);
    CHECK(port_sd_file_info(idx, &fi) && fi.sample_frames == 5 && fi.imported);
    CHECK(port_sd_read_frame(idx, 0, 1, &f) && f.id == 0x18FF50E5 && f.data[11] == 0xCC);
    CHECK(env_log_export(0, dump, sizeof dump, &needed) == ENV_LOG_ERR_NOT_AVAILABLE);
    request(EV_REQ_REC_START, 0, 0, 0); run(200);
    CHECK(rec_get()->state == REC_IDLE && port_sd_file_count() == full_count); /* all sample slots occupied */
    CHECK(port_sd_read_frame(idx, 0, 1, &f) && f.id == 0x18FF50E5 && f.data[11] == 0xCC);

    boot(); env_cmd("sd_fill", 0, 0);
    CHECK(env_log_import("large.log", huge, used, &result) == ENV_LOG_ERR_LIMIT && port_sd_file_count() == 5);
    boot(); port_sd_format();
    CHECK(env_log_import("format.log", trace, strlen(trace), &result) == ENV_LOG_ERR_BUSY);
    boot(); env_cmd("sd_unsupported", 1, 0);
    CHECK(env_log_import("bad-card.log", trace, strlen(trace), &result) == ENV_LOG_ERR_BUSY);
    sim_reset();
    CHECK(env_log_import("off.log", trace, strlen(trace), &result) == ENV_LOG_ERR_BUSY);
}

static void actual_replay(void)
{
    boot();
    int idx = import("(100.000000) can0 123#AA\n(100.100000) can1 456#BB\n"
                     "(100.200000) can0 18FF50E5##3112233445566778899AABBCC\n(100.800000) can0 321#R8\n");
    ring_reader_t reader; ring_reader_attach(&reader, false);
    start(0, idx); run(1);
    CHECK(play_get(0)->frames_total == 3 && play_get(0)->frames_sent == 1);
    const can_frame_t *echo = ring_peek(&reader);
    CHECK(echo && echo->id == 0x123 && echo->data[0] == 0xAA && echo->ch == 0 && (echo->flags & CANF_TX));
    if (echo) ring_advance(&reader);
    run(198); CHECK(play_get(0)->frames_sent == 1);
    run(1); CHECK(play_get(0)->frames_sent == 2);
    echo = ring_peek(&reader);
    CHECK(echo && echo->id == 0x18FF50E5 && echo->len == 12 && echo->data[11] == 0xCC && (echo->flags & CANF_BRS) && !(echo->flags & CANF_ESI));
    if (echo) ring_advance(&reader);
    run(599); CHECK(play_get(0)->frames_sent == 2);
    run(1); CHECK(play_get(0)->state == PL_IDLE && play_get(0)->frames_sent == 3);
    echo = ring_peek(&reader);
    CHECK(echo && echo->id == 0x321 && (echo->flags & CANF_RTR) && echo->len == 8);
    start(1, idx); run(99); CHECK(play_get(1)->frames_sent == 0);
    run(1); CHECK(play_get(1)->frames_sent == 1 && play_get(1)->state == PL_IDLE);

    boot(); cfg_mut()->fd[0] = false; env_cmd("bus_fd", 0, 0);
    idx = import("(0.0) can0 123#AA\n(0.1) can0 123##1AABB\n(0.2) can0 124#BB\n(0.3) can0 20000004#01\n");
    start(0, idx); run(300);
    CHECK(play_get(0)->state == PL_IDLE && play_get(0)->frames_sent == 2 && play_get(0)->fd_skipped == 1 && play_get(0)->error_skipped == 1);

    boot(); idx = import("(0.0) can0 123#AA\n(1.0) can0 124#BB\n(2.0) can0 125#CC\n");
    start(0, idx); request(EV_REQ_PLAY_CTRL, 0, PLAYCTL_PAUSE_TOGGLE, 0);
    request(EV_REQ_PLAY_CTRL, 0, PLAYCTL_STEP, 0);
    CHECK(play_get(0)->frames_sent == 2 && play_get(0)->vt_us == 1000000);
    request(EV_REQ_PLAY_CTRL, 0, PLAYCTL_PAUSE_TOGGLE, 0); run(999);
    CHECK(play_get(0)->frames_sent == 2);
    run(1); CHECK(play_get(0)->state == PL_IDLE && play_get(0)->frames_sent == 3);
}

static void retained_recording(void)
{
    boot();
    port_sd_prealloc(8, 1, 1024, 500000, 2000000, true);
    run(100);
    uint32_t active_count; uint64_t active_duration; can_frame_t active_frame;
    CHECK(!port_sd_replay_info(5, 0, &active_count, &active_duration));
    CHECK(!port_sd_read_frame(5, 0, 0, &active_frame));
    env_log_result_t result;
    const char *line = "(0.0) can0 123#AA\n";
    CHECK(env_log_import("busy.log", line, strlen(line), &result) == ENV_LOG_ERR_BUSY);
    for (unsigned i = 0; i < ENV_LOG_MAX_FRAMES + 5u; i++) {
        can_frame_t frame = { .id = 0x321, .ch = (uint8_t)(i & 1u), .len = 2,
            .ts_us = i < ENV_LOG_MAX_FRAMES ? (ENV_LOG_MAX_FRAMES - i) * 1000u : i * 1000u };
        frame.data[0] = (uint8_t)i; frame.data[1] = 0x5A;
        port_sd_record_frame(&frame);
    }
    file_info_t fi; CHECK(port_sd_file_info(5, &fi));
    CHECK(fi.frames == ENV_LOG_MAX_FRAMES + 5u && fi.sample_frames == ENV_LOG_MAX_FRAMES && fi.sample_dropped == 5 && fi.sample_truncated);
    CHECK(fi.sample_available && !fi.nbr && !fi.dbr); /* no cross-channel rate invention */
    static char dump[ENV_LOG_MAX_TEXT + 1u]; size_t needed;
    CHECK(env_log_export(5, dump, sizeof dump, &needed) == ENV_LOG_ERR_BUSY);
    port_sd_close(); run(200);
    CHECK(env_log_export(5, dump, sizeof dump, &needed) > 0);
    uint32_t count; uint64_t duration; can_frame_t f;
    CHECK(port_sd_replay_info(5, 0, &count, &duration) && count == ENV_LOG_MAX_FRAMES / 2u);
    CHECK(port_sd_read_frame(5, 1, 0, &f) && f.ts_us == 0 && f.data[1] == 0x5A);
    port_sd_delete_oldest(); /* original preset index 0, sample shifts from 5 to 4 */
    CHECK(port_sd_file_info(4, &fi) && fi.sample_frames == ENV_LOG_MAX_FRAMES);
    CHECK(env_log_export(4, dump, sizeof dump, &needed) > 0);
}

static void deletion_during_replay(void)
{
    boot();
    int idx = import("(0.0) can0 555#AA\n(1.0) can0 556#BB\n");
    start(0, idx);
    port_sd_delete_oldest(); /* deleting idx0 must preserve the active imported file */
    CHECK(play_get(0)->file == idx - 1 && play_get(0)->state == PL_PLAYING);
    run(1000); CHECK(play_get(0)->state == PL_IDLE && play_get(0)->frames_sent == 2);
    can_frame_t f; CHECK(port_sd_read_frame(idx - 1, 0, 1, &f) && f.id == 0x556 && f.data[0] == 0xBB);

    boot(); start(0, 0);
    int before = port_sd_file_count();
    port_sd_delete_oldest();
    CHECK(port_sd_file_count() == before && play_get(0)->file == 0 && play_get(0)->state == PL_PLAYING);
    /* A later closed session may be deleted; the in-use oldest one stays. */
    port_sd_prealloc(8, 1, 1024, 500000, 2000000, true); run(100);
    port_sd_close(); run(200); before = port_sd_file_count();
    port_sd_delete_oldest();
    CHECK(port_sd_file_count() == before - 1 && play_get(0)->file == 0 && play_get(0)->state == PL_PLAYING);
}

int main(void)
{
    codec(); recording_text_accounting(); bounded_import(); actual_replay(); retained_recording(); deletion_during_replay();
    printf("logfmt/storage/replay: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
