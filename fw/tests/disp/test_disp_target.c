/* Exercise the actual board storage layout: SRAM4 FD tails, independent bus /
 * direction identities and stable paused snapshots, under ASan and UBSan. */
#ifndef CANFD_REVD_TARGET
#error This test must exercise the board display path.
#endif
#include "services/disp/disp_pipe.h"
#include "services/frame_ring/frame_ring.h"
#include "services/core/app_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static bool independent_capture_clock;
static uint64_t capture_clock_us;
uint64_t port_capture_time_us(void)
{return independent_capture_clock?capture_clock_us:app_now_us();}
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)

static can_frame_t make_frame(unsigned number)
{
    can_frame_t f={.id=0x123,.flags=CANF_FD|CANF_BRS,
                   .len=64,.ch=(uint8_t)(number&1u),.ts_us=(uint64_t)number*1000u};
    if(number&2u) f.flags|=CANF_TX;
    for(unsigned b=0;b<64;b++)f.data[b]=(uint8_t)(number*7u+b*3u);
    return f;
}
static void push(can_frame_t f)
{
    CHECK(ring_push(&f));
    disp_tick();
}
static void reset(void)
{
    tmr_init();ring_init();disp_init();
}
int main(void)
{
    _Static_assert(DISP_DATA==64,"board details must expose all 64 bytes");
    reset();
    /* Wrap the 1,024-entry history and compare complete retained frames. */
    for(unsigned i=0;i<1600;i++)push(make_frame(i));
    CHECK(disp_rows_available()==DISP_ROWS);
    CHECK(disp_id_count()==4);
    for(unsigned back=0;back<DISP_ROWS;back++) {
        can_frame_t expected=make_frame(1599u-back);disp_row_t actual;
        CHECK(disp_row(back,&actual));
        CHECK(actual.ts_us==expected.ts_us&&actual.id==expected.id);
        CHECK(actual.ch==expected.ch&&actual.flags==expected.flags&&actual.len==64);
        CHECK(memcmp(actual.data,expected.data,64)==0);
        uint32_t sequence,found;CHECK(disp_row_seq(back,&sequence));
        CHECK(disp_row_back_for_seq(sequence,&found)&&found==back);
    }
    disp_row_t last;CHECK(!disp_row(DISP_ROWS,&last));
    uint32_t old_sequence;CHECK(disp_row_seq(DISP_ROWS-1,&old_sequence));
    push(make_frame(1600));uint32_t absent;CHECK(!disp_row_back_for_seq(old_sequence,&absent));

    /* Pause holds both history and ID detail data while the reader drains. */
    app_clock_advance(1234);disp_pause(true);
    uint64_t frozen_time=disp_view_time_us();disp_id_t frozen[DISP_IDS];
    int n=disp_id_count();for(int i=0;i<n;i++)frozen[i]=*disp_id_at(i);
    disp_row_t paused_top;CHECK(disp_row(0,&paused_top));
    ring_reader_t recorder;ring_reader_attach(&recorder,true);
    for(unsigned i=1601;i<1801;i++) {
        push(make_frame(i));CHECK(ring_peek(&recorder)!=NULL);ring_advance(&recorder);
    }
    app_clock_advance(2000);disp_tick();
    CHECK(disp_hidden_while_paused()==200&&disp_view_time_us()==frozen_time);
    CHECK(disp_id_count()==n);
    for(int i=0;i<n;i++)CHECK(memcmp(&frozen[i],disp_id_at(i),sizeof frozen[i])==0);
    CHECK(disp_row(0,&last)&&memcmp(&last,&paused_top,sizeof last)==0);
    CHECK(ring_dropped()==0&&ring_level()==0);
    disp_pause(false);CHECK(disp_hidden_while_paused()==0&&disp_view_time_us()!=frozen_time);
    bool advanced=false;for(int i=0;i<n;i++)advanced|=disp_id_at(i)->count>frozen[i].count;
    CHECK(advanced);

    /* Short frames sharing a wrapped slot cannot expose an old FD tail. */
    reset();push(make_frame(0));
    can_frame_t short_frame=make_frame(1);short_frame.len=8;short_frame.flags=0;
    for(unsigned i=0;i<DISP_ROWS;i++)push(short_frame);
    CHECK(disp_row(0,&last)&&last.len==8&&memcmp(last.data,short_frame.data,8)==0);
    for(unsigned b=8;b<64;b++)CHECK(last.data[b]==0);

    /* Malformed length is never allowed to overrun tail storage. Hardware
     * normally rejects this before the ring; memory safety still holds. */
    can_frame_t malformed=make_frame(9);malformed.len=255;push(malformed);
    CHECK(disp_row(0,&last)&&memcmp(last.data,malformed.data,64)==0);

    /* A debugger warm boot can advance the gesture clock by an artificial
     * power-hold second. Display age must use the CAN timestamp epoch. */
    reset();independent_capture_clock=true;capture_clock_us=12000;
    app_clock_advance(10000);can_frame_t physical=make_frame(0);
    physical.ts_us=capture_clock_us;push(physical);
    capture_clock_us+=1000;physical.ts_us=capture_clock_us;physical.data[63]^=0xFF;push(physical);
    CHECK(disp_view_time_us()==capture_clock_us);
    CHECK(disp_id_byte_changed(disp_id_at(0),63));
    disp_pause(true);uint64_t captured_pause=disp_view_time_us();
    capture_clock_us+=1600000;app_clock_advance(1);disp_tick();
    CHECK(disp_view_time_us()==captured_pause&&disp_id_byte_changed(disp_id_at(0),63));
    disp_pause(false);CHECK(disp_view_time_us()==capture_clock_us);
    CHECK(!disp_id_byte_changed(disp_id_at(0),63));
    printf("PASS: board 64-byte display history/pause, %u checks\n",checks);
    return 0;
}
