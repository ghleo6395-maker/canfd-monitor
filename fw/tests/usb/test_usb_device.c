/* SPDX-License-Identifier: MIT
 * Host transport/ownership tests. No real USB or disk is accessed. */
#include "platform/stm32h743_revd/usb_device.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/config/cfg.h"
#include "services/can_core/chan_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/frame_ring/frame_ring.h"
#include "services/power/power_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "services/bench/bench.h"
#include "platform/stm32h743_revd/can_ingress.h"
#include "platform/stm32h743_revd/display.h"
#include "platform/stm32h743_revd/storage_sd.h"
#include "stm32h7xx_hal.h"
#include "tusb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
USB_OTG_DeviceTypeDef test_usb_regs;
static uint32_t now_ms;
static bool mounted, suspended, cdc_dtr, storage_give_ok = true, owner, sync_ok = true;
static unsigned raw_reads, raw_writes, syncs, reclaims, irq_calls;
static unsigned transport_depth, transport_peak;
static bool nested_poll;
static uint8_t disk[4][512];
static char rx[8192], tx[32768];
static size_t rx_pos, rx_len, tx_len;
static uint32_t usb_room = 512;
static cfg_t cfg;
static rec_t rec;
static chan_t chan[2];
static storage_t storage;
static power_t power;
static bench_t bench;
static bool max_metrics;
static unsigned bench_calls;
const bench_t *bench_get(void) { return &bench; }
const char *bench_execute(bench_action_t action, const bench_config_t *config) {
    ++bench_calls;
    if (action == BENCH_CONFIG) CHECK(config->segment_kib == 64 && config->rotate_ms == 250 && config->refresh_ms == 50);
    return "OK BENCH_TEST\r\n";
}
const char *board_boot_id(void) { return "0123456789abcdef"; }
bool board_can_metrics(unsigned ch, board_can_metrics_t *out) {
    CHECK(ch < 2); memset(out, max_metrics ? 0xff : 0, sizeof *out); return true;
}
void storage_sd_stats(storage_sd_stats_t *out) { memset(out, max_metrics ? 0xff : 0, sizeof *out); }
const display_diag_t *display_diagnostics(void) {
    static display_diag_t lcd;
    lcd = (display_diag_t){.dma_busy = max_metrics, .dma_transfers = max_metrics ? UINT32_MAX : 0,
        .dma_bands = max_metrics ? UINT32_MAX : 0, .dirty_bands = max_metrics ? UINT32_MAX : 0,
        .dma_timeouts = max_metrics ? UINT32_MAX : 0, .spi_errors = max_metrics ? UINT32_MAX : 0,
        .ui_snapshot_errors = max_metrics ? UINT32_MAX : 0,
        .forced_redraws = max_metrics ? UINT32_MAX : 0, .max_render_ms = max_metrics ? UINT32_MAX : 0};
    return &lcd;
}
uint32_t HAL_GetTick(void) { return now_ms; }
void HAL_GPIO_Init(void *port, GPIO_InitTypeDef *gpio) { (void)port; CHECK(gpio->Pin == (GPIO_PIN_11|GPIO_PIN_12)); CHECK(gpio->Alternate == 10); }
void HAL_PWREx_EnableUSBVoltageDetector(void) {}
void NVIC_SetPriority(int irq, unsigned pri) { CHECK(irq == OTG_FS_IRQn && pri == 7); }
bool tud_mounted(void) { return mounted; }
bool tud_suspended(void) { return suspended; }
bool tud_cdc_connected(void) { return cdc_dtr; }
uint32_t tud_cdc_write_available(void) { return usb_room; }
uint32_t tud_cdc_write(const void *bytes, uint32_t len) { CHECK(tx_len + len < sizeof tx); memcpy(tx + tx_len, bytes, len); tx_len += len; tx[tx_len] = 0; return len; }
uint32_t tud_cdc_write_flush(void) { return 0; }
uint32_t tud_cdc_available(void) { return (uint32_t)(rx_len-rx_pos); }
int32_t tud_cdc_read_char(void) { return rx_pos < rx_len ? (unsigned char)rx[rx_pos++] : -1; }
bool tud_configure(uint8_t port, uint32_t id, const void *config) { const tud_configure_param_t *c=config; CHECK(port==0 && id==100 && !c->dwc2.vbus_sensing); return true; }
bool tusb_init(uint8_t port,const tusb_rhport_init_t *config) { CHECK(port==0 && config->role==TUSB_ROLE_DEVICE && config->speed==TUSB_SPEED_FULL); return true; }
void tud_task_ext(uint32_t timeout,bool in_isr) {
    CHECK(timeout==0 && !in_isr); ++transport_depth;
    CHECK(transport_depth==1);
    if(transport_depth>transport_peak) transport_peak=transport_depth;
    if(nested_poll) { nested_poll=false; usb_device_poll_transport(); usb_device_task(); }
    --transport_depth;
}
void tud_int_handler(uint8_t port) { CHECK(port==0); ++irq_calls; }
bool tud_msc_set_sense(uint8_t lun,uint8_t key,uint8_t asc,uint8_t ascq) { (void)lun;(void)key;(void)asc;(void)ascq; return true; }
bool storage_sd_media_give(void) { if (!storage_give_ok) return false; owner=true; return true; }
void storage_sd_media_reclaim(void) { owner=false; ++reclaims; }
bool storage_sd_raw_ready(void) { return owner; }
bool storage_sd_raw_sync(void) { ++syncs; return owner && sync_ok; }
uint32_t storage_sd_raw_block_count(void) { return owner ? 4 : 0; }
bool storage_sd_raw_read(uint32_t lba,uint8_t *data,uint32_t count) { CHECK(owner && lba<4 && count==1); ++raw_reads; memcpy(data,disk[lba],512); return true; }
bool storage_sd_raw_write(uint32_t lba,const uint8_t *data,uint32_t count) { CHECK(owner && lba<4 && count==1); ++raw_writes; memcpy(disk[lba],data,512); return true; }
const power_t *power_get(void) { return &power; }
bool power_running(void) { return power.state==PWR_RUNNING; }
const cfg_t *cfg_get(void) { return &cfg; }
const rec_t *rec_get(void) { return &rec; }
const chan_t *chan_get(int ch) { return &chan[ch]; }
const storage_t *storage_get(void) { return &storage; }
int ui_dialog_ask(void) { return ASK_TX_FIRST; }
tx_result_t tx_entry_update(int ch,int idx,uint32_t id,uint8_t flags,uint8_t len,const uint8_t *data,uint32_t period) { (void)flags;(void)data; CHECK(ch==0 && idx==0 && id==0x123 && len==2 && period==100); return TX_OK; }
const char *tx_result_name(tx_result_t result) { (void)result; return "MOCK"; }

extern void tud_mount_cb(void);
extern void tud_umount_cb(void);
extern void tud_cdc_line_state_cb(uint8_t,bool,bool);
extern bool tud_msc_test_unit_ready_cb(uint8_t);
extern bool tud_msc_start_stop_cb(uint8_t,uint8_t,bool,bool);
extern void tud_msc_capacity_cb(uint8_t,uint32_t*,uint16_t*);
extern int32_t tud_msc_read10_cb(uint8_t,uint32_t,uint32_t,void*,uint32_t);
extern int32_t tud_msc_write10_cb(uint8_t,uint32_t,uint32_t,uint8_t*,uint32_t);
extern int32_t tud_msc_scsi_cb(uint8_t,const uint8_t[16],void*,uint16_t);
extern void OTG_FS_IRQHandler(void);
static void send(const char *s)
{
    rx_len=strlen(s); rx_pos=0; memcpy(rx,s,rx_len);
    while(rx_pos<rx_len) usb_device_task();
    usb_device_task();
}
static bool pop_type(ev_type_t type,app_evt_t *found)
{
    app_evt_t e;
    while(evt_pop(&e)) if(e.type==type) { if(found) *found=e; return true; }
    return false;
}
static void init(void)
{
    evt_init(); ring_init(); mounted=suspended=cdc_dtr=owner=false;
    storage_give_ok=sync_ok=true; power=(power_t){.state=PWR_RUNNING}; now_ms=0; test_usb_regs.DSTS=0;
    rx_len=rx_pos=tx_len=0; tx[0]=0; usb_room=512;
    CHECK(usb_device_init());
    max_metrics = false; bench_calls = 0;
}
static void attach(bool with_cdc)
{
    mounted=true; tud_mount_cb();
    cdc_dtr=with_cdc;
    if(with_cdc) tud_cdc_line_state_cb(0,true,false);
    usb_device_task();
}
static void test_lifecycle_and_protocol(void)
{
    init();
    CHECK(!tud_msc_test_unit_ready_cb(0));
    CHECK(!tud_msc_start_stop_cb(0,0,true,true));
    attach(true);
    CHECK(pop_type(EV_USB_ATTACH,NULL));
    app_evt_t e;
    CHECK(pop_type(EV_HOST_LINK,&e) && e.a==1);
    send("CH 1 MODE NORMAL\r\nTX 2 CYC 3 ON\nREC STOP\n");
    CHECK(pop_type(EV_REQ_CH_MODE,&e) && e.src==SRC_HOST && e.b==0);
    CHECK(pop_type(EV_REQ_TX_CYC,&e) && e.src==SRC_HOST && e.b==1);
    CHECK(pop_type(EV_REQ_REC_STOP,&e) && e.src==SRC_HOST && e.b==0);
    send("TX SET 1 0 100 123#0102\nSTATUS\n");
    CHECK(strstr(tx,"OK TX_SET") && strstr(tx,"\"ask\":4"));
    CHECK(strstr(tx,"\"boot_id\":\"0123456789abcdef\"") && strstr(tx,"\"irq_read_errors\":0"));
    send("BENCH CONFIG 64 250 50\nBENCH ON\n"); CHECK(bench_calls == 2);
    size_t status_start = tx_len;
    max_metrics = true; send("STATUS\n"); max_metrics = false;
    for (unsigned i = 0; i < 16; ++i) usb_device_task();
    CHECK(strstr(tx + status_start,"\"written_bytes\":18446744073709551615"));
    CHECK(strstr(tx + status_start,"\"ui_snapshot_errors\":4294967295"));
    CHECK(strstr(tx + status_start,"}}}\r\n"));
    CHECK(!strstr(tx + status_start,"STATUS_OVERFLOW"));
    char over[300]; memset(over,'A',sizeof over); over[sizeof over-2]='\n'; over[sizeof over-1]=0;
    send(over); CHECK(strstr(tx,"ERR LINE_TOO_LONG"));
    /* Closing CDC does not reclaim an MSC volume. */
    port_usb_msc(true); CHECK(owner && tud_msc_test_unit_ready_cb(0));
    cdc_dtr=false; tud_cdc_line_state_cb(0,false,false); usb_device_task();
    CHECK(pop_type(EV_HOST_LINK,&e) && e.a==0 && owner);
    /* Host suspend is conservatively treated as detach on no-VBUS Rev D. */
    suspended=true; usb_device_task();
    CHECK(!owner && !usb_device_media_ready());
    CHECK(pop_type(EV_USB_DETACH,NULL));
    suspended=false; ++now_ms; test_usb_regs.DSTS=1u<<8; usb_device_task();
    CHECK(pop_type(EV_USB_ATTACH,NULL));
    CHECK(!usb_device_media_ready());
    port_usb_msc(true); CHECK(owner);
    now_ms+=251; usb_device_task();
    CHECK(!owner && pop_type(EV_USB_DETACH,NULL));
    unsigned before=irq_calls; OTG_FS_IRQHandler(); CHECK(irq_calls==before+1);
}
static void test_msc_bounds_and_sync(void)
{
    init(); attach(false); evt_init();
    uint32_t count; uint16_t block_size;
    tud_msc_capacity_cb(0,&count,&block_size); CHECK(count==0 && block_size==512);
    uint8_t buffer[512]; memset(buffer,0xaa,512);
    CHECK(tud_msc_write10_cb(0,0,0,buffer,512)==-1);
    port_usb_msc(true); CHECK(owner);
    tud_msc_capacity_cb(0,&count,&block_size); CHECK(count==4 && block_size==512);
    CHECK(tud_msc_write10_cb(0,1,0,buffer,512)==512);
    CHECK(disk[1][511]==0xaa);
    memset(buffer,0xbb,4);
    CHECK(tud_msc_write10_cb(0,1,8,buffer,4)==4);
    CHECK(disk[1][7]==0xaa && disk[1][8]==0xbb && disk[1][12]==0xaa);
    memset(buffer,0,512);
    CHECK(tud_msc_read10_cb(0,1,8,buffer,4)==4 && buffer[0]==0xbb);
    unsigned reads=raw_reads,writes=raw_writes;
    CHECK(tud_msc_read10_cb(0,4,0,buffer,1)==-1);
    CHECK(tud_msc_write10_cb(0,1,511,buffer,2)==-1);
    CHECK(tud_msc_write10_cb(0,1,UINT32_MAX,buffer,1)==-1);
    CHECK(tud_msc_read10_cb(1,0,0,buffer,1)==-1);
    CHECK(raw_reads==reads && raw_writes==writes);
    uint8_t scsi[16]={0x35};
    CHECK(tud_msc_scsi_cb(0,scsi,NULL,0)==0);
    scsi[0]=0xff; CHECK(tud_msc_scsi_cb(0,scsi,NULL,0)==-1);
    sync_ok=false;
    CHECK(!tud_msc_start_stop_cb(0,0,false,true)); CHECK(owner && usb_device_media_ready());
    port_usb_msc(false); CHECK(owner && !usb_device_media_ready());
    /* A second retract must not bypass the failed sync and reclaim unsafely. */
    port_usb_msc(false); CHECK(owner);
    sync_ok=true; port_usb_msc(false); CHECK(!owner);
    port_usb_msc(true); CHECK(owner);
    CHECK(tud_msc_start_stop_cb(0,0,false,true));
    CHECK(!usb_device_media_ready() && owner && pop_type(EV_USB_EJECT,NULL));
    port_usb_msc(false); CHECK(!owner);
    storage_give_ok=false; port_usb_msc(true);
    CHECK(!owner && !usb_device_media_ready() && !pop_type(EV_USB_EJECT,NULL));
    storage_give_ok=true; usb_device_task(); CHECK(owner && usb_device_media_ready());
    port_usb_msc(false); storage_give_ok=false; port_usb_msc(true);
    /* Keep receiving SOF while waiting for an asynchronous unmount failure. */
    now_ms+=5001; test_usb_regs.DSTS+=1u<<8; usb_device_task();
    CHECK(!owner && !usb_device_media_ready() && pop_type(EV_USB_EJECT,NULL));
}
static void test_boot_reset_and_saturated_eject(void)
{
    init(); attach(true); evt_init(); send("STREAM ON\n");
    for (unsigned i=0;i<20;++i) { can_frame_t f={.id=0x123,.len=1}; CHECK(ring_push(&f)); }
    usb_device_task();
    ring_init(); ++power.boot_count; usb_device_task();
    CHECK(pop_type(EV_USB_ATTACH,NULL));
    CHECK(pop_type(EV_HOST_LINK,NULL));
    CHECK(usb_device_stream_dropped()==0);
    power.state=PWR_OFF; send("REC START\n");
    CHECK(strstr(tx,"ERR DEVICE_NOT_READY") && !pop_type(EV_REQ_REC_START,NULL));
    power.state=PWR_RUNNING; evt_init(); port_usb_msc(true); CHECK(owner);
    while(evt_post4(EV_NONE,0,SRC_SYS,0,0)) {}
    CHECK(tud_msc_start_stop_cb(0,0,false,true));
    CHECK(!usb_device_media_ready() && owner);
    evt_init(); usb_device_task();
    CHECK(pop_type(EV_USB_EJECT,NULL));
    port_usb_msc(false); CHECK(!owner);
}
static void test_transport_wait_isolation(void)
{
    init(); attach(true); evt_init();
    const char *cmd="REC START\n";
    rx_len=strlen(cmd); rx_pos=0; memcpy(rx,cmd,rx_len);
    nested_poll=true; usb_device_poll_transport();
    CHECK(transport_peak==1 && rx_pos==0 && !pop_type(EV_REQ_REC_START,NULL));
    usb_device_task(); CHECK(pop_type(EV_REQ_REC_START,NULL));
}
static void test_stream_backpressure(void)
{
    init(); attach(true); evt_init(); send("STREAM ON\n"); usb_room=0;
    /* Push/drain a sustained source with a stopped host. Its lossy USB reader
     * must never register as lossless or fill the recorder producer. */
    for (unsigned i=0;i<2000;++i) {
        can_frame_t f={.id=0x123,.len=8,.ts_us=i*1000u};
        CHECK(ring_push(&f));
        usb_device_task();
    }
    CHECK(usb_device_stream_dropped()>0 && ring_level()==0 && ring_dropped()==0);
    CHECK(usb_device_reply_dropped()==0);
    usb_room=512;
    for (unsigned i=0;i<30;++i) usb_device_task();
    send("STATUS\n"); CHECK(strstr(tx,"STATUS {")!=NULL);
}
int main(void)
{
    test_lifecycle_and_protocol(); test_msc_bounds_and_sync(); test_boot_reset_and_saturated_eject(); test_stream_backpressure(); test_transport_wait_isolation();
    printf("USB device ownership: %u checks passed\n",checks);
    return 0;
}
