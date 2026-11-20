/* Error timeouts must measure wall-clock progress through long SD waits. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include <stdio.h>

static unsigned checks,failures;
#define CHECK(condition) do { checks++; if(!(condition)) { failures++; \
    fprintf(stderr,"recording recovery line %d: %s\n",__LINE__,#condition); } } while(0)

static void send(ev_type_t type,int a)
{
    CHECK(evt_post4(type,0,SRC_UI,a,1));
    app_tick(0);
}

static void recording(void)
{
    sim_reset();sim_key(KEY_POWER,1);
    for(unsigned i=0;i<150;i++)sim_tick(10);
    sim_key(KEY_POWER,0);
    send(EV_REQ_REC_START,0);
    for(unsigned i=0;i<30;i++)sim_tick(10);
    CHECK(rec_get()->state==REC_RUNNING);
    /* Finish the simulator's last RX batch before injecting timed faults;
     * otherwise that batch can also generate its own simulated write error. */
    app_capture_poll();
}

static void persistent_slow_errors_stop(void)
{
    recording();
    CHECK(env_cmd("sd_werr",2,0)==0);
    for(unsigned i=0;i<3;i++) {
        app_clock_advance(2100);
        send(EV_SD_WRITE_ERR,1);
        if(i<2)CHECK(rec_get()->state==REC_RUNNING);
    }
    CHECK(rec_get()->state==REC_STOPPING);
    CHECK(rec_get()->last_result==MSG_REC_WRITE_FAIL);
}

static void successful_recovery_resets_consecutive_faults(void)
{
    recording();
    CHECK(env_cmd("sd_werr",2,0)==0);
    CHECK(env_cmd("sd_busy",1,0)==0);
    send(EV_SD_WRITE_ERR,1);
    app_clock_advance(2100);
    send(EV_SD_WRITE_ERR,1);
    CHECK(rec_get()->state==REC_RUNNING&&rec_get()->write_errs==2);
    /* A busy frame queue is not a fault: explicit recovery resets the count
     * even while the generic platform busy indication remains asserted. */
    CHECK(env_cmd("sd_werr",0,0)==0);
    port_sd_reinit();
    app_tick(0);
    CHECK(rec_get()->write_errs==0);
    CHECK(env_cmd("sd_werr",2,0)==0);
    app_clock_advance(2100);
    send(EV_SD_WRITE_ERR,1);
    CHECK(rec_get()->state==REC_RUNNING&&rec_get()->write_errs==1);
}

static void shutdown_uses_elapsed_time(bool wrap)
{
    recording();
    if(wrap)app_clock_advance(UINT32_MAX-app_now_ms()-1499u);
    send(EV_REQ_SHUTDOWN,0);
    CHECK(power_get()->state==PWR_SHUTDOWN&&rec_get()->state==REC_STOPPING);
    /* Withhold the close completion, as during a multi-second card stall. */
    app_clock_advance(2999);
    app_tick(0);
    CHECK(power_get()->state==PWR_SHUTDOWN);
    CHECK(power_get()->shwait_ms==2999);
    app_clock_advance(50);
    app_tick(0);
    CHECK(power_get()->state==PWR_OFF);
    CHECK(power_get()->shwait_ms==3049);
}

static void removal_during_close_does_not_prompt_for_name(void)
{
    recording();
    CHECK(ui_review_navigate(PG_LIVE)==0);
    CHECK(sim_rec_action(1)==0);
    CHECK(rec_get()->state==REC_STOPPING&&ui_draft_open());
    /* The real FatFs test separately verifies removal emits REMOVE without a
     * fabricated CLOSE_DONE. Route that contract through the complete app. */
    CHECK(env_cmd("sd_remove",0,0)==0);
    app_tick(0);
    CHECK(storage_get()->state==ST_NO_CARD);
    CHECK(rec_get()->state==REC_IDLE&&rec_get()->last_result==MSG_REC_SD_REMOVED);
    CHECK(!ui_draft_open());
    for(unsigned i=0;i<30;i++)sim_tick(10);
    CHECK(rec_get()->last_result==MSG_REC_SD_REMOVED&&!ui_draft_open());
    /* A late completion after removal has already dispatched is harmless. */
    send(EV_SD_CLOSE_DONE,SD_OK);
    CHECK(rec_get()->last_result==MSG_REC_SD_REMOVED&&!ui_draft_open());
}

int main(void)
{
    persistent_slow_errors_stop();
    successful_recovery_resets_consecutive_faults();
    shutdown_uses_elapsed_time(false);
    shutdown_uses_elapsed_time(true);
    removal_during_close_does_not_prompt_for_name();
    printf("recording recovery: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
