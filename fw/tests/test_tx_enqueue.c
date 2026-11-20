/* A controller enqueue rejection is not a transmitted frame or a NoACK.
 * Fill the simulated hardware queue independently to exercise that contract. */
#include "app/app.h"
#include "services/can_core/chan_sm.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/sim_api.h"
#include <stdio.h>
#include <string.h>

static unsigned checks,fails;
#define CHECK(condition) do { checks++; if(!(condition)) { fails++; \
    printf("FAIL line %d: %s\n",__LINE__,#condition); } } while(0)

static void boot_normal(int ch)
{
    sim_reset();sim_key(KEY_POWER,1);
    for(unsigned i=0;i<170;i++)sim_tick(10);
    sim_key(KEY_POWER,0);
    evt_post4(EV_REQ_CH_MODE,(uint8_t)ch,SRC_UI,CAN_MODE_NORMAL,2);
    app_tick(0);
    CHECK(chan_can_tx(ch));
}

static void test_rejected_enqueue(int ch)
{
    boot_normal(ch);
    const tx_entry_t before=tx_get()->e[ch][0];
    CHECK(before.dyn.cnt_bits!=0);
    for(unsigned i=0;i<TX_FIFO_LEN;i++)port_can_tx(ch,&before.f);
    CHECK(!tx_entry_submit(ch,0,TXSRC_MANUAL));
    CHECK(tx_last_result(ch)==TX_ERR_FIFO);
    CHECK(tx_get()->pending[ch]==0);
    CHECK(tx_get()->e[ch][0].sent==before.sent);
    CHECK(memcmp(&tx_get()->e[ch][0].dyn,&before.dyn,sizeof before.dyn)==0);
    CHECK(!tmr_active(ch?TMR_TX_DWELL1:TMR_TX_DWELL0));
    /* No deadline may be invented for a frame rejected before transmission. */
    app_realtime_poll(TX_DWELL_MS+1u);
    CHECK(tx_get()->noack_count[ch]==0);
    port_can_tx_abort(ch);
    CHECK(tx_entry_submit(ch,0,TXSRC_MANUAL));
    CHECK(tx_get()->pending[ch]==1);
    CHECK(tx_get()->e[ch][0].sent==before.sent+1);
    CHECK(tx_get()->e[ch][0].dyn.cnt_val!=before.dyn.cnt_val);
    CHECK(tmr_active(ch?TMR_TX_DWELL1:TMR_TX_DWELL0));
}

static void test_cyclic_deadline(int ch,uint32_t late,bool wrap,bool full)
{
    boot_normal(ch);
    const uint32_t period=100;
    const uint8_t payload=0x5a;
    CHECK(tx_entry_update(ch,0,0x123,0,1,&payload,period)==TX_OK);
    if(wrap)app_clock_advance(UINT32_MAX-50u-app_now_ms());
    const uint32_t start=app_now_ms();
    const app_evt_t enable={.type=EV_REQ_TX_CYC,.ch=(uint8_t)ch,
        .src=SRC_UI,.a=0,.b=3};
    tx_handle(&enable);
    CHECK(tx_get()->e[ch][0].active);
    if(full)for(unsigned i=0;i<TX_FIFO_LEN;i++)
        CHECK(port_can_tx(ch,&tx_get()->e[ch][0].f));
    app_clock_advance(late);
    tx_tick();
    const tx_entry_t *entry=&tx_get()->e[ch][0];
    CHECK(entry->sent==(full?0u:1u));
    CHECK(entry->missed==late/period+(full?1u:0u));
    CHECK(entry->next_due==start+(late/period+1u)*period);
    /* Repeated foreground/cooperative polls in one millisecond must not
     * replay an expired slot, including when the first enqueue was rejected. */
    tx_tick();tx_tick();
    CHECK(entry->sent==(full?0u:1u));
    CHECK(entry->missed==late/period+(full?1u:0u));
    port_can_tx_abort(ch);
    /* Inspect the next scheduled boundary without invoking unrelated timers. */
    app_clock_advance(entry->next_due-app_now_ms()-1u);
    tx_tick();CHECK(entry->sent==(full?0u:1u));
    app_clock_advance(1);
    tx_tick();CHECK(entry->sent==(full?1u:2u));
}

int main(void)
{
    test_rejected_enqueue(0);test_rejected_enqueue(1);
    const uint32_t delays[]={0,1,99,100,101,1000};
    for(int ch=0;ch<CH_NUM;ch++)for(unsigned i=0;i<sizeof delays/sizeof delays[0];i++)
        for(unsigned wrap=0;wrap<2;wrap++)for(unsigned full=0;full<2;full++)
            test_cyclic_deadline(ch,delays[i],wrap!=0,full!=0);
    printf("TX enqueue and deadlines: %u checks, %u failures\n",checks,fails);
    return fails?1:0;
}
