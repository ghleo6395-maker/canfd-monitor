/* Real simulated storage and recorder, no browser/transport mock. */
#include "app/app.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

static void run(unsigned ms)
{
    while (ms) { unsigned step = ms > 10 ? 10 : ms; sim_tick((int)step); ms -= step; }
}

static void request(ev_type_t event, int a, int b)
{
    evt_post4(event, 0, SRC_UI, a, b);
    app_tick(0);
}

static void boot(void)
{
    sim_reset(); sim_key(KEY_POWER, 1); run(1200); sim_key(KEY_POWER, 0); run(520);
    env_cmd("bus_fps", 0, 0); env_cmd("bus_fps", 1, 0);
    CHECK(power_get()->state == PWR_RUNNING);
}

static int import_trace(const char *name)
{
    const char *trace = "(0.000000) can0 123#AA\n(1.000000) can1 456##1AABB\n";
    env_log_result_t result;
    return env_log_import(name, trace, strlen(trace), &result);
}

static void prefix_and_names(void)
{
    sim_reset();
    CHECK(env_rec_name_set("road") == ENV_LOG_ERR_BUSY);
    time_t before = time(NULL);
    boot();
    CHECK(!strcmp(env_rec_name_get(), ""));
    const char *invalid[] = {"../road", "a/b", "a\\b", "a:b", "a\nb", "a?b", "con", "COM1", "nul.log", " road", "road ", "road.", ".hidden", "a\x7F", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE4\xB8", "a\xE2\x80\xAE"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        CHECK(env_rec_name_set(invalid[i]) < 0);
        CHECK(!strcmp(env_rec_name_get(), ""));
    }
    CHECK(env_rec_name_set("1234567890123456789") == ENV_LOG_ERR_LIMIT);
    CHECK(env_rec_name_set("道路测试样例") == ENV_LOG_OK); /* six Chinese chars, 18 bytes */
    request(EV_REQ_REC_START, 0, 0); run(200);
    CHECK(rec_get()->state == REC_RUNNING);
    file_info_t file;
    CHECK(port_sd_file_info(5, &file));
    CHECK(strlen(file.name) == ENV_FILE_NAME_MAX_BYTES);
    CHECK(strncmp(file.name, "道路测试样例_", 19) == 0);
    CHECK(strstr(file.name, "_00000008.log") != NULL);
    CHECK(file.writing);
    const char *stamp = file.name + 19;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    CHECK(sscanf(stamp, "%4d%2d%2d-%2d%2d%2d", &year, &month, &day, &hour, &minute, &second) == 6);
    struct tm calendar = {0};
    calendar.tm_year = year - 1900; calendar.tm_mon = month - 1; calendar.tm_mday = day;
    calendar.tm_hour = hour; calendar.tm_min = minute; calendar.tm_sec = second; calendar.tm_isdst = -1;
    time_t stamped = mktime(&calendar);
    CHECK(stamped >= before && stamped <= time(NULL) + 3);
    CHECK(env_rec_name_set("next") == ENV_LOG_ERR_BUSY);
    uint32_t token = env_file_token(5);
    CHECK(token != 0);
    CHECK(env_log_rename(5, token, "new.log") == ENV_LOG_ERR_BUSY);
    request(EV_REQ_REC_STOP, 0, 1);
    CHECK(rec_get()->state == REC_STOPPING);
    CHECK(env_rec_name_set("next") == ENV_LOG_ERR_BUSY);
    run(200); CHECK(rec_get()->state == REC_IDLE);
    CHECK(env_rec_name_set("") == ENV_LOG_OK);
    CHECK(!strcmp(env_rec_name_get(), ""));
    env_cmd("rtc_lost", 0, 0);
    request(EV_REQ_REC_START, 0, 0); run(200);
    CHECK(rec_get()->state == REC_RUNNING && rec_get()->session == 9);
    CHECK(port_sd_file_info(6, &file));
    CHECK(!strcmp(file.name, "REC_NOTIME_00000009.log"));
    CHECK(env_file_token(6) != token);
    request(EV_REQ_REC_STOP, 0, 1); run(200);
    CHECK(rec_get()->state == REC_IDLE);
    CHECK(port_sd_file_info(5, &file) && !file.writing);
    CHECK(strstr(file.name, "_00000008.log") != NULL);
}

static void rename_and_identity(void)
{
    boot();
    int index = import_trace("sample.log"); CHECK(index == 5);
    uint32_t token = env_file_token(index); CHECK(token != 0);
    CHECK(env_log_rename(index, token + 1, "new.log") == ENV_LOG_ERR_CHANGED);
    CHECK(env_log_rename(index, token, "road.txt") == ENV_LOG_OK);
    file_info_t file; CHECK(port_sd_file_info(index, &file) && !strcmp(file.name, "road.txt"));
    char exported[256]; size_t needed;
    CHECK(env_log_export(index, exported, sizeof exported, &needed) > 0);
    CHECK(strstr(exported, "123#AA") != NULL);
    CHECK(env_log_rename(index, token, "道路.log") == ENV_LOG_OK);
    CHECK(import_trace("道路.log") == ENV_LOG_ERR_EXISTS);
    CHECK(import_trace("trace.ASC") == ENV_LOG_ERR_EXTENSION);
    CHECK(import_trace("table.csv") == ENV_LOG_ERR_EXTENSION);
    CHECK(import_trace("../unsafe.log") == ENV_LOG_ERR_ARGUMENT);
    CHECK(import_trace("bad\nname.log") == ENV_LOG_ERR_ARGUMENT);
    CHECK(import_trace("CON.log") == ENV_LOG_ERR_ARGUMENT);
    CHECK(import_trace("truncated-\xE4\xB8.log") == ENV_LOG_ERR_ARGUMENT);
    CHECK(env_log_rename(index, token, "bad\nname.log") == ENV_LOG_ERR_NAME);
    CHECK(env_log_rename(index, token, "trace.asc") == ENV_LOG_ERR_EXTENSION);
    CHECK(env_log_rename(index, token, "ROAD.LOG") == ENV_LOG_OK);
    CHECK(import_trace("road.log") == ENV_LOG_ERR_EXISTS);
    int other = import_trace("other.log"); CHECK(other == 6);
    CHECK(env_log_rename(other, env_file_token(other), "road.LOG") == ENV_LOG_ERR_EXISTS);
    CHECK(port_sd_file_info(other, &file) && !strcmp(file.name, "other.log"));
    port_sd_protect_toggle(index);
    CHECK(env_log_rename(index, token, "protected.log") == ENV_LOG_ERR_READONLY);
    port_sd_protect_toggle(index);
    CHECK(env_log_rename(0, env_file_token(0), "fixture.log") == ENV_LOG_ERR_READONLY);
    request(EV_REQ_PLAY_OPEN, index, 0);
    CHECK(play_file_in_use(index));
    CHECK(env_log_rename(index, token, "playing.log") == ENV_LOG_ERR_BUSY);
    request(EV_REQ_PLAY_CTRL, PLAYCTL_STOP, 0);
    CHECK(!play_file_in_use(index));
    port_sd_delete_oldest(); /* removes fixture 0, compacts imported indices */
    CHECK(env_file_token(index - 1) == token);
    CHECK(env_file_token(index) != token);
    CHECK(env_log_rename(index, token, "wrong.log") == ENV_LOG_ERR_CHANGED);
    CHECK(env_log_rename(index - 1, token, "shifted.log") == ENV_LOG_OK);
    CHECK(port_sd_file_info(index - 1, &file) && !strcmp(file.name, "shifted.log"));
    uint32_t maximum = env_file_token(other - 1);
    port_sd_format(); run(600);
    CHECK(env_file_token(index - 1) == 0);
    int fresh = import_trace("fresh.log"); CHECK(fresh == 0);
    CHECK(env_file_token(fresh) > maximum);
    CHECK(env_log_rename(fresh, token, "stale.log") == ENV_LOG_ERR_CHANGED);
    CHECK(env_log_rename(fresh, env_file_token(fresh), "fine.LOG") == ENV_LOG_OK);
}

int main(void)
{
    prefix_and_names();
    rename_and_identity();
    printf("Sim file naming / repeat recording: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
