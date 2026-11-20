/* Recorder deadlines may outlive the asynchronous SD operation they guarded.
 * Deliberately withhold platform completions and dispatch queued old deadlines.
 */
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    fprintf(stderr,"rec lifecycle line %d: %s\n",__LINE__,#condition); } } while (0)

static void send_rec(ev_type_t type, int a)
{
    app_evt_t event={.type=type,.src=SRC_UI,.a=a,.b=1};
    rec_handle(&event);
}

static void discard_events(void)
{
    app_evt_t event;
    while(evt_pop(&event)) {}
}

static void boot(void)
{
    sim_reset();
    sim_key(KEY_POWER,1);
    for(unsigned i=0;i<150;i++)sim_tick(10);
    sim_key(KEY_POWER,0);
    CHECK(storage_get()->state==ST_READY);
    discard_events();
}

static app_evt_t expire_guard(void)
{
    app_clock_advance(3000);
    tmr_tick();
    app_evt_t event,guard={0};
    while(evt_pop(&event))
        if(event.type==EV_TIMER&&event.a==TMR_REC_GUARD)guard=event;
    CHECK(guard.type==EV_TIMER);
    return guard;
}

static void prepare_timeout_closes_late_success(void)
{
    boot();
    send_rec(EV_REQ_REC_START,0);
    CHECK(rec_get()->state==REC_PREPARING);
    app_evt_t deadline=expire_guard();
    rec_handle(&deadline);
    CHECK(rec_get()->state==REC_STOPPING);
    CHECK(rec_get()->last_result==MSG_SD_ERROR);
    /* A delayed successful preallocation must never re-enable recording. */
    send_rec(EV_SD_PREALLOC_DONE,SD_OK);
    CHECK(rec_get()->state==REC_STOPPING);
    send_rec(EV_REQ_REC_START,0);
    CHECK(rec_get()->state==REC_STOPPING);
    /* This comes from the simulator only if the timeout requested a close. */
    app_clock_advance(200);
    env_tick(0);
    app_evt_t event;
    unsigned closes=0;
    while(evt_pop(&event)) {
        if(event.type==EV_SD_CLOSE_DONE)closes++;
        rec_handle(&event);
    }
    CHECK(closes==1);
    CHECK(rec_get()->state==REC_IDLE&&rec_get()->last_result==MSG_SD_ERROR);
    can_frame_t frame={.id=0x123,.len=8};
    CHECK(!port_sd_record_frame(&frame));
    send_rec(EV_REQ_REC_START,0);
    send_rec(EV_SD_PREALLOC_DONE,SD_OK);
    CHECK(rec_get()->state==REC_RUNNING);
}

static void stale_prepare_deadline_does_not_expire_close(void)
{
    boot();
    send_rec(EV_REQ_REC_START,0);
    app_evt_t old=expire_guard();
    send_rec(EV_REQ_REC_STOP,0);
    rec_handle(&old);
    CHECK(rec_get()->state==REC_STOPPING&&rec_get()->last_result==MSG_REC_STOPPED);
    app_evt_t close_deadline=expire_guard();
    rec_handle(&close_deadline);
    CHECK(rec_get()->state==REC_STOPPING&&rec_get()->last_result==MSG_REC_WRITE_FAIL);
    send_rec(EV_REQ_REC_START,0);
    CHECK(rec_get()->state==REC_STOPPING);
    send_rec(EV_SD_CLOSE_DONE,SD_OK);
    CHECK(rec_get()->state==REC_IDLE&&rec_get()->last_result==MSG_REC_WRITE_FAIL);
    send_rec(EV_REQ_REC_START,0);
    send_rec(EV_SD_PREALLOC_DONE,SD_OK);
    CHECK(rec_get()->state==REC_RUNNING);
}

static void stale_close_deadline_does_not_expire_new_session(void)
{
    boot();
    send_rec(EV_REQ_REC_START,0);
    send_rec(EV_SD_PREALLOC_DONE,SD_OK);
    send_rec(EV_REQ_REC_STOP,0);
    app_evt_t old=expire_guard();
    send_rec(EV_SD_CLOSE_DONE,SD_OK);
    send_rec(EV_REQ_REC_START,0);
    rec_handle(&old);
    CHECK(rec_get()->state==REC_PREPARING);
    send_rec(EV_SD_PREALLOC_DONE,SD_OK);
    CHECK(rec_get()->state==REC_RUNNING);
}

static void late_write_failure_survives_successful_close(void)
{
    boot();
    send_rec(EV_REQ_REC_START,0);
    send_rec(EV_SD_PREALLOC_DONE,SD_OK);
    send_rec(EV_REQ_REC_STOP,0);
    send_rec(EV_SD_WRITE_ERR,2);
    send_rec(EV_SD_CLOSE_DONE,SD_OK);
    CHECK(rec_get()->state==REC_IDLE&&rec_get()->last_result==MSG_REC_WRITE_FAIL);
}

static void removal_finishes_timeout_cleanup(void)
{
    boot();
    send_rec(EV_REQ_REC_START,0);
    app_evt_t deadline=expire_guard();
    rec_handle(&deadline);
    send_rec(EV_NTF_STORAGE_LOST,0);
    CHECK(rec_get()->state==REC_IDLE&&rec_get()->last_result==MSG_REC_SD_REMOVED);
    send_rec(EV_SD_CLOSE_DONE,SD_OK);
    CHECK(rec_get()->last_result==MSG_REC_SD_REMOVED);
}

static void guard_deadline_crosses_millisecond_wrap(void)
{
    boot();
    app_clock_advance(UINT32_MAX - app_now_ms() - 999u);
    send_rec(EV_REQ_REC_START,0);
    app_clock_advance(2999);
    tmr_tick();
    app_evt_t event;
    unsigned expired=0;
    while(evt_pop(&event))
        if(event.type==EV_TIMER&&event.a==TMR_REC_GUARD)expired++;
    CHECK(expired==0&&tmr_active(TMR_REC_GUARD));
    /* An old queued timer is also harmless at this wrap-crossing boundary. */
    send_rec(EV_TIMER,TMR_REC_GUARD);
    CHECK(rec_get()->state==REC_PREPARING);
    app_clock_advance(1);
    tmr_tick();
    while(evt_pop(&event)) {
        if(event.type==EV_TIMER&&event.a==TMR_REC_GUARD)expired++;
        rec_handle(&event);
    }
    CHECK(expired==1&&rec_get()->state==REC_STOPPING);
    CHECK(rec_get()->last_result==MSG_SD_ERROR);
}

int main(void)
{
    prepare_timeout_closes_late_success();
    stale_prepare_deadline_does_not_expire_close();
    stale_close_deadline_does_not_expire_new_session();
    late_write_failure_survives_successful_close();
    removal_finishes_timeout_cleanup();
    guard_deadline_crosses_millisecond_wrap();
    printf("recorder lifecycle: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
