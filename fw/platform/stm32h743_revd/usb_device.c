/* SPDX-License-Identifier: MIT
 * TinyUSB CDC+MSC integration, main-loop callbacks only. */
#include "usb_device.h"
#include "usb_protocol.h"
#include "storage_sd.h"
#include "power_reve.h"
#if defined(CANFD_REVD_TARGET) || defined(CANFD_REVD_HOST_DIAG)
#include "can_ingress.h"
#include "board.h"
#include "display.h"
#endif
#include "stm32h7xx_hal.h"
#include "tusb.h"
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/bench/bench.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/frame_ring/frame_ring.h"
#include "services/logfmt/candump.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/tx_sched/tx_sm.h"
#include <stdio.h>
#include <string.h>
#ifdef REVD_FREERTOS
#include <stdatomic.h>
#include "rtos/bridges.h"
#include "rtos/runtime.h"
#undef tud_cdc_write_available
#undef tud_cdc_write
#undef tud_cdc_write_flush
#undef tud_cdc_available
#undef tud_cdc_read_char
#undef tud_cdc_connected
#define tud_cdc_write_available revd_usb_tx_available
#define tud_cdc_write revd_usb_write
#define tud_cdc_write_flush revd_usb_flush
#define tud_cdc_available revd_usb_rx_available
#define tud_cdc_read_char revd_usb_rx_char
#define tud_cdc_connected revd_usb_connected
#define tud_mounted revd_usb_mounted
#define tud_suspended revd_usb_suspended
#define REVD_FIRMWARE_ID "revd-freertos-20261024"
#else
#ifdef CANFD_REVE_TARGET
#define REVD_FIRMWARE_ID "reve-bare-reliability-20261104-dev"
#else
#define REVD_FIRMWARE_ID "revd-bare-ui-20261027-r1"
#endif
#endif

#define OUTPUT_CAPACITY 8192u
#ifdef REVD_FREERTOS
#define REPLY_RESERVE 6144u
#else
#define REPLY_RESERVE 4096u
#endif
#define SOF_TIMEOUT_MS 250u
#define SCSI_SYNCHRONIZE_CACHE_10 0x35u
static char output[OUTPUT_CAPACITY];
static uint32_t out_head, out_tail, reply_drops, stream_drops;
static char input[USB_COMMAND_MAX + 1u];
static size_t input_len;
static bool input_overflow, input_invalid, skip_lf;
#ifdef REVD_FREERTOS
static _Atomic bool initialized, media_shared, eject_pending;
static bool observed_bus, reported_bus, reported_host, dtr, stream_on, mounted_session;
#else
#ifdef CANFD_REVE_TARGET
static volatile bool initialized, vbus_attached;
static bool observed_bus, reported_bus, reported_host, dtr;
#else
static bool initialized, observed_bus, reported_bus, reported_host, dtr;
#endif
static bool stream_on, media_shared, mounted_session, eject_pending;
#endif
static uint32_t last_sof_ms, observed_boot_count;
static uint16_t last_usb_frame;
static bool transport_active, share_pending;
static uint32_t share_deadline;
static ring_reader_t stream_reader;
#ifdef REVD_FREERTOS
static void usb_callbacks_task(void);
#endif
/* No USB/SD DMA accesses this buffer; 32 alignment also permits a later SD DMA path. */
static uint8_t sector[512] __attribute__((aligned(32)));

static uint32_t output_space(void) { return OUTPUT_CAPACITY - (out_head - out_tail); }
static bool output_bytes(const char *bytes, size_t len)
{
    if (len > output_space()) { ++reply_drops; return false; }
    for (size_t i = 0; i < len; ++i) output[(out_head++) % OUTPUT_CAPACITY] = bytes[i];
    return true;
}
static void reply(const char *text) { (void)output_bytes(text, strlen(text)); }
static void output_flush(void)
{
    uint32_t avail = tud_cdc_write_available();
    uint32_t count = out_head - out_tail;
    if (count > avail) count = avail;
    uint32_t chunk = OUTPUT_CAPACITY - out_tail % OUTPUT_CAPACITY;
    if (count > chunk) count = chunk;
    if (count) out_tail += tud_cdc_write(&output[out_tail % OUTPUT_CAPACITY], count);
    (void)tud_cdc_write_flush();
}
static void reset_session(void)
{
    input_len = 0; input_overflow = false; input_invalid = false; skip_lf = false;
    out_head = out_tail = 0; stream_on = false;
}
uint32_t usb_device_stream_dropped(void) { return stream_drops; }
uint32_t usb_device_reply_dropped(void) { return reply_drops; }
bool usb_device_media_ready(void) { return media_shared && storage_sd_raw_ready(); }

void port_usb_msc(bool share)
{
    if (share) {
        media_shared = observed_bus && storage_sd_media_give();
        share_pending = observed_bus && !media_shared;
        share_deadline = HAL_GetTick() + 5000u;
        if (!observed_bus) eject_pending = true;
    } else {
        /* Hide the LUN before any local remount. Failed sync leaves owner with
         * storage driver, so its mount path must refuse concurrent access. */
        share_pending = false;
        bool was_ready = storage_sd_raw_ready();
        media_shared = false;
        if (!was_ready || storage_sd_raw_sync()) storage_sd_media_reclaim();
        else (void)evt_post4(EV_SD_WRITE_ERR, 0, SRC_SYS, 1, 0);
    }
}
/* Poll the DWC2 received-SOF frame number, rather than enabling a 1 kHz
 * application SOF callback that could flood TinyUSB's event queue during an SD
 * write. This register is read-only to this module; TinyUSB owns the controller. */
static uint16_t received_frame_number(void)
{
    const USB_OTG_DeviceTypeDef *device = (const USB_OTG_DeviceTypeDef *)(uintptr_t)
        (USB2_OTG_FS_PERIPH_BASE + USB_OTG_DEVICE_BASE);
    return (uint16_t)((device->DSTS & USB_OTG_DSTS_FNSOF_Msk) >> USB_OTG_DSTS_FNSOF_Pos);
}
static void lifecycle_task(void)
{
    /* Local unmount is queued. A failed give can mean it has not finished yet;
     * expose NOT_READY and retry rather than reentering FatFs from this path. */
    if (share_pending) {
        if (observed_bus && storage_sd_media_give()) {
            share_pending = false;
            media_shared = true;
        } else if (!observed_bus || (int32_t)(HAL_GetTick()-share_deadline)>=0) {
            share_pending = false;
            eject_pending = true;
        }
    }
    if (eject_pending && evt_post4(EV_USB_EJECT, 0, SRC_SYS, 0, 0)) eject_pending = false;
    if (power_get()->boot_count != observed_boot_count) {
        observed_boot_count = power_get()->boot_count;
        port_usb_msc(false);
        reset_session();
        ring_reader_attach(&stream_reader, false);
        /* boot_reset() recreated storage_sm after USB may already have mounted. */
        reported_bus = false;
        reported_host = false;
    }
    if (!power_running()) stream_on = false;
    uint16_t frame = received_frame_number();
    if (frame != last_usb_frame) { last_usb_frame = frame; last_sof_ms = HAL_GetTick(); }
    bool active = mounted_session && tud_mounted() && !tud_suspended() &&
#ifdef CANFD_REVE_TARGET
                  vbus_attached &&
#endif
                  (uint32_t)(HAL_GetTick() - last_sof_ms) < SOF_TIMEOUT_MS;
    if (active != observed_bus) {
        observed_bus = active;
        if (!active) {
            port_usb_msc(false);
            reset_session();
        }
    }
    /* Retry queue submission on saturation. Device callbacks never access
     * application state from the interrupt handler. */
    if (reported_bus != observed_bus &&
        evt_post4(observed_bus ? EV_USB_ATTACH : EV_USB_DETACH, 0, SRC_SYS, 0, 0))
        reported_bus = observed_bus;
    bool host = observed_bus && dtr && tud_cdc_connected();
    if (reported_host != host && evt_post4(EV_HOST_LINK, 0, SRC_HOST, host, 0))
        reported_host = host;
}
static void status_reply(void)
{
    /* Main-loop owner only; avoid a 6 KiB temporary on the exception stack. */
#ifdef REVD_FREERTOS
    static char text[6144], rtos[1536];
#else
    static char text[4096];
#endif
    static char performance[2560];
    strcpy(performance, "{}");
    const char *boot_id = "";
#if defined(CANFD_REVD_TARGET) || defined(CANFD_REVD_HOST_DIAG)
    boot_id = board_boot_id();
    board_can_metrics_t cm[2] = {{0}};
    storage_sd_stats_t sd;
    (void)board_can_metrics(0, &cm[0]); (void)board_can_metrics(1, &cm[1]);
    storage_sd_stats(&sd);
#ifdef REVD_FREERTOS
    display_diag_t lcd_copy;
    revd_ui_diagnostics(&lcd_copy);
    const display_diag_t *lcd = &lcd_copy;
#else
    const display_diag_t *lcd = display_diagnostics();
#endif
    int pn = snprintf(performance, sizeof performance,
        "{\"can_capacity\":%u,\"sd_capacity\":%u,\"can\":[{\"q\":%lu,\"peak\":%lu,\"hw_lost_min\":%lu,\"irq_drops\":%lu,\"ring_drops\":%lu,\"echo_drops\":%lu,\"irq_read_errors\":%lu,\"max_service_gap_us\":%lu,\"max_service_us\":%lu},"
        "{\"q\":%lu,\"peak\":%lu,\"hw_lost_min\":%lu,\"irq_drops\":%lu,\"ring_drops\":%lu,\"echo_drops\":%lu,\"irq_read_errors\":%lu,\"max_service_gap_us\":%lu,\"max_service_us\":%lu}],"
        "\"sd\":{\"q\":%lu,\"peak\":%lu,\"rejected\":%lu,\"accepted\":%lu,\"encoded\":%lu,\"discarded\":%lu,\"dma\":%lu,\"errors\":%lu,\"timeouts\":%lu,"
        "\"card_busy_timeouts\":%lu,\"max_card_busy_ms\":%lu,\"max_dma_wait_ms\":%lu,\"write_calls\":%lu,\"sync_calls\":%lu,\"max_write_ms\":%lu,\"max_sync_ms\":%lu,\"max_task_ms\":%lu,"
        "\"segments_opened\":%lu,\"segments_closed\":%lu,\"command_high_water\":%lu,\"command_overflows\":%lu,\"catalog_limited\":%lu,\"catalog_omitted\":%lu,\"written_bytes\":%llu},"
        "\"lcd\":{\"dma\":%lu,\"bands\":%lu,\"dirty\":%lu,\"busy\":%u,\"timeouts\":%lu,\"spi_errors\":%lu,\"ui_snapshot_errors\":%lu,\"forced_redraws\":%lu,\"max_render_ms\":%lu,\"max_service_gap_us\":%lu}}",
        (unsigned)CAN_INGRESS_LEN, (unsigned)REVD_SD_RECORD_QUEUE_FRAMES,
        (unsigned long)cm[0].queued, (unsigned long)cm[0].high_water,
        (unsigned long)cm[0].hw_lost_events, (unsigned long)cm[0].ingress_drops,
        (unsigned long)cm[0].ring_drops, (unsigned long)cm[0].tx_echo_drops, (unsigned long)cm[0].irq_read_errors,
        (unsigned long)cm[0].max_service_gap_us, (unsigned long)cm[0].max_service_us,
        (unsigned long)cm[1].queued, (unsigned long)cm[1].high_water,
        (unsigned long)cm[1].hw_lost_events, (unsigned long)cm[1].ingress_drops,
        (unsigned long)cm[1].ring_drops, (unsigned long)cm[1].tx_echo_drops, (unsigned long)cm[1].irq_read_errors,
        (unsigned long)cm[1].max_service_gap_us, (unsigned long)cm[1].max_service_us,
        (unsigned long)sd.queued_frames, (unsigned long)sd.queue_high_water,
        (unsigned long)sd.queue_overflows, (unsigned long)sd.enqueued_frames,
        (unsigned long)sd.encoded_frames, (unsigned long)sd.discarded_frames,
        (unsigned long)sd.dma_transfers, (unsigned long)sd.dma_errors, (unsigned long)sd.dma_timeouts,
        (unsigned long)sd.card_busy_timeouts, (unsigned long)sd.max_card_busy_ms, (unsigned long)sd.max_dma_wait_ms,
        (unsigned long)sd.write_calls, (unsigned long)sd.sync_calls, (unsigned long)sd.max_write_ms,
        (unsigned long)sd.max_sync_ms, (unsigned long)sd.max_task_ms, (unsigned long)sd.segments_opened,
        (unsigned long)sd.segments_closed, (unsigned long)sd.command_high_water, (unsigned long)sd.command_overflows,
        (unsigned long)sd.catalog_limited, (unsigned long)sd.catalog_omitted, (unsigned long long)sd.written_bytes,
        (unsigned long)lcd->dma_transfers, (unsigned long)lcd->dma_bands, (unsigned long)lcd->dirty_bands,
        lcd->dma_busy, (unsigned long)lcd->dma_timeouts, (unsigned long)lcd->spi_errors, (unsigned long)lcd->ui_snapshot_errors,
        (unsigned long)lcd->forced_redraws, (unsigned long)lcd->max_render_ms, (unsigned long)lcd->max_service_gap_us);
    if (pn < 0 || (size_t)pn >= sizeof performance) { reply("ERR STATUS_OVERFLOW\r\n"); return; }
#endif
#ifdef REVD_FREERTOS
    revd_bridge_metrics_t bm; revd_bridge_metrics(&bm);
    revd_rtos_task_metrics_t tm[REVD_RTOS_TASK_COUNT] = {{0}};
    for (unsigned t = 0; t < REVD_RTOS_TASK_COUNT; ++t) (void)revd_rtos_task_metrics(t, &tm[t]);
    int rn = snprintf(rtos, sizeof rtos,
        "{\"scheduler\":\"freertos\",\"kernel\":\"11.3.2\",\"input_dropped\":%lu,\"usb_callback_dropped\":%lu,\"stale_touch_ignored\":%lu,\"max_input_latency_us\":%lu,\"max_snapshot_wait_us\":%lu,"
        "\"tasks\":[{\"name\":\"core\",\"runs\":%lu,\"max_step_us\":%lu,\"stack_free\":%lu,\"stack_size\":%lu},"
        "{\"name\":\"storage\",\"runs\":%lu,\"max_step_us\":%lu,\"stack_free\":%lu,\"stack_size\":%lu},"
        "{\"name\":\"usb\",\"runs\":%lu,\"max_step_us\":%lu,\"stack_free\":%lu,\"stack_size\":%lu},"
        "{\"name\":\"ui\",\"runs\":%lu,\"max_step_us\":%lu,\"stack_free\":%lu,\"stack_size\":%lu}]}",
        (unsigned long)bm.input_dropped, (unsigned long)bm.usb_callback_dropped, (unsigned long)bm.stale_touch_ignored,
        (unsigned long)bm.max_input_latency_us, (unsigned long)bm.max_snapshot_wait_us,
        (unsigned long)tm[0].runs, (unsigned long)tm[0].max_step_us, (unsigned long)tm[0].stack_min_free_bytes, (unsigned long)tm[0].stack_size_bytes,
        (unsigned long)tm[1].runs, (unsigned long)tm[1].max_step_us, (unsigned long)tm[1].stack_min_free_bytes, (unsigned long)tm[1].stack_size_bytes,
        (unsigned long)tm[2].runs, (unsigned long)tm[2].max_step_us, (unsigned long)tm[2].stack_min_free_bytes, (unsigned long)tm[2].stack_size_bytes,
        (unsigned long)tm[3].runs, (unsigned long)tm[3].max_step_us, (unsigned long)tm[3].stack_min_free_bytes, (unsigned long)tm[3].stack_size_bytes);
    if (rn < 0 || (size_t)rn >= sizeof rtos) { reply("ERR STATUS_OVERFLOW\r\n"); return; }
#endif
    const cfg_t *cfg = cfg_get();
    const rec_t *rec = rec_get();
    const bench_t *bench = bench_get();
    const chan_t *ch0 = chan_get(0), *ch1 = chan_get(1);
#ifdef CANFD_REVE_TARGET
    const reve_power_diag_t *pd=reve_power_diagnostics();
#endif
    int n = snprintf(text, sizeof text,
        "STATUS {\"protocol\":1,\"firmware\":\"" REVD_FIRMWARE_ID "\",\"boot_id\":\"%s\",\"diag_epoch\":%lu,\"uptime_ms\":%lu,\"usb\":%u,\"host\":%u,\"msc\":%u,"
        "\"storage\":%u,\"rec\":%u,\"frames\":%lu,\"bytes\":%llu,\"rec_session\":%lu,\"rec_segment\":%lu,\"rec_elapsed_ms\":%lu,\"rec_drops\":%lu,\"rec_result\":%u,\"rec_reason\":%u,"
        "\"ring_dropped\":%lu,\"stream_dropped\":%lu,\"reply_dropped\":%lu,"
        "\"event_dropped\":%lu,\"ask\":%d,\"channels\":["
        "{\"state\":%u,\"mode\":%u,\"nbr\":%lu,\"dbr\":%lu,\"fd\":%u},"
        "{\"state\":%u,\"mode\":%u,\"nbr\":%lu,\"dbr\":%lu,\"fd\":%u}],"
        "\"bench\":{\"enabled\":%u,\"segment_kib\":%lu,\"rotate_ms\":%lu,\"refresh_ms\":%lu,\"rotations\":%lu,\"redraws\":%lu,\"rejected\":%lu},\"performance\":%s"
#ifdef CANFD_REVE_TARGET
        ",\"power\":{\"input_mode\":%u,\"cell_mv\":%u,\"battery_ma\":%ld,\"monitor_valid\":%u,\"cc1_mv\":%u,\"cc2_mv\":%u,\"cc_valid\":%u,\"stale_fallbacks\":%lu,\"monitor_errors\":%lu}"
#endif
#ifdef REVD_FREERTOS
        ",\"rtos\":%s"
#endif
        "}\r\n",
        boot_id, (unsigned long)bench->diag_epoch, (unsigned long)HAL_GetTick(),
        observed_bus, reported_host, usb_device_media_ready(),
        storage_get()->state, rec->state, (unsigned long)rec->frames,
        (unsigned long long)rec->bytes, (unsigned long)rec->session, (unsigned long)rec->seg,
        (unsigned long)rec->elapsed_ms, (unsigned long)rec->drops,
        !(rec->state == REC_IDLE && rec->last_result == MSG_REC_STOPPED), rec->last_result,
        (unsigned long)ring_dropped(), (unsigned long)stream_drops, (unsigned long)reply_drops,
        (unsigned long)evt_dropped(), ui_dialog_ask(),
        ch0->state, ch0->mode, (unsigned long)cfg->nbr[0], (unsigned long)cfg->dbr[0], cfg->fd[0],
        ch1->state, ch1->mode, (unsigned long)cfg->nbr[1], (unsigned long)cfg->dbr[1], cfg->fd[1],
        bench->enabled, (unsigned long)bench->config.segment_kib, (unsigned long)bench->config.rotate_ms,
        (unsigned long)bench->config.refresh_ms, (unsigned long)bench->rotations_requested,
        (unsigned long)bench->redraws_requested, (unsigned long)bench->request_rejected, performance
#ifdef CANFD_REVE_TARGET
        , pd->mode, pd->cell_mv, (long)pd->battery_ma, pd->monitor_valid,
        pd->cc1_mv, pd->cc2_mv, pd->cc_valid, (unsigned long)pd->stale_fallbacks,
        (unsigned long)pd->monitor_errors
#endif
#ifdef REVD_FREERTOS
        , rtos
#endif
        );
    if (n >= 0 && (size_t)n < sizeof text) (void)output_bytes(text, (size_t)n);
    else reply("ERR STATUS_OVERFLOW\r\n");
}
static void execute_line(void)
{
    usb_command_t c;
    usb_parse_result_t result = usb_protocol_parse(input, input_len, &c);
    if (result != USB_PARSE_OK) {
        char text[48];
        (void)snprintf(text, sizeof text, "ERR %s\r\n", usb_protocol_error(result));
        reply(text); return;
    }
    if (c.kind != USB_CMD_EMPTY && c.kind != USB_CMD_HELP && c.kind != USB_CMD_STATUS && !power_running()) {
        reply("ERR DEVICE_NOT_READY\r\n"); return;
    }
    switch (c.kind) {
    case USB_CMD_EMPTY: break;
    case USB_CMD_HELP: reply(usb_protocol_help()); break;
    case USB_CMD_STATUS: status_reply(); break;
    case USB_CMD_EVENT:
        reply(evt_post(&c.event) ? "OK QUEUED DEVICE_CONFIRM_IF_REQUIRED\r\n" : "ERR EVENT_QUEUE_FULL\r\n");
        break;
    case USB_CMD_TX_SET: {
        tx_result_t tr = tx_entry_update(c.frame.ch, c.slot, c.frame.id, c.frame.flags,
                                       c.frame.len, c.frame.data, c.period_ms);
        if (tr == TX_OK) reply("OK TX_SET\r\n");
        else {
            char text[64];
            (void)snprintf(text, sizeof text, "ERR TX_%s\r\n", tx_result_name(tr));
            reply(text);
        }
        break;
    }
    case USB_CMD_BENCH:
        reply(bench_execute(c.bench_action, &c.bench_config));
        break;
    case USB_CMD_STREAM:
        stream_on = c.stream;
        if (stream_on) ring_reader_attach(&stream_reader, false);
        reply(stream_on ? "OK STREAM_ON\r\n" : "OK STREAM_OFF\r\n");
        break;
    }
}
static void input_task(void)
{
    /* Bound work and leave CDC FIFO unread when output is congested, allowing
     * endpoint backpressure. A reply always has room before its command runs. */
    for (unsigned i = 0; i < 128u && tud_cdc_available() && output_space() >= REPLY_RESERVE; ++i) {
        int value = tud_cdc_read_char();
        if (value < 0) break;
        char c = (char)value;
        if (c == '\n' && skip_lf) { skip_lf = false; continue; }
        skip_lf = c == '\r';
        if (c == '\r' || c == '\n') {
            if (input_overflow) reply("ERR LINE_TOO_LONG\r\n");
            else if (input_invalid) reply("ERR SYNTAX\r\n");
            else execute_line();
            input_len = 0; input_overflow = false; input_invalid = false;
        } else if ((unsigned char)c < 32u && c != '\t') input_invalid = true;
        else if ((unsigned char)c > 126u) input_invalid = true;
        else if (input_len < USB_COMMAND_MAX) input[input_len++] = c;
        else input_overflow = true;
    }
}
static void stream_task(void)
{
    if (!stream_on) return;
    for (unsigned i = 0; i < 8u; ++i) {
        uint32_t before = stream_reader.tail;
        const can_frame_t *p = ring_peek(&stream_reader);
        stream_drops += stream_reader.tail - before;
        if (!p) break;
        can_frame_t frame = *p;
        /* Always advance this lossy reader, even if USB is slow. */
        ring_advance(&stream_reader);
        char line[LOGFMT_MAX_LINE + 6u];
        memcpy(line, "FRAME ", 6);
        int n = logfmt_format_line(&frame, line + 6, sizeof line - 6u);
        if (n < 0 || output_space() < REPLY_RESERVE + (uint32_t)n + 6u) { ++stream_drops; continue; }
        (void)output_bytes(line, (size_t)n + 6u);
    }
}

uint32_t tusb_time_millis_api(void) { return HAL_GetTick(); }
bool usb_device_init(void)
{
#ifdef CANFD_REVE_TARGET
    /* Do not initialize a controller that would assert D+ on an unpowered
     * cable. Foreground retries once the physical VBUS detector is active. */
    if(!reve_power_vbus()) return true;
#endif
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USB2_OTG_FS_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_AF_PP; gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH; gpio.Alternate = 10u;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_PWREx_EnableUSBVoltageDetector();
    NVIC_SetPriority(OTG_FS_IRQn, 7u);
    reset_session();
    transport_active = share_pending = false;
    reply_drops = stream_drops = 0;
    observed_bus = reported_bus = reported_host = dtr = media_shared = mounted_session = eject_pending = false;
    observed_boot_count = power_get()->boot_count;
    ring_reader_attach(&stream_reader, false);
    const tud_configure_param_t config = {.dwc2 = {.bm_double_buffered = 0, .vbus_sensing = false}};
    const tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    initialized = tud_configure(0, TUD_CFGID_DWC2, &config) && tusb_init(0, &init);
#ifdef CANFD_REVE_TARGET
    vbus_attached=initialized;
    usb_device_vbus_tick();
#endif
    return initialized;
}
void usb_device_poll_transport(void)
{
#ifdef REVD_FREERTOS
    /* Core/control never enters TinyUSB. A dedicated task owns it. */
    revd_rtos_notify_usb();
    return;
#endif
    if (!initialized || transport_active) return;
    transport_active = true;
    tud_task_ext(0, false);
    transport_active = false;
}
void usb_device_task(void)
{
#ifdef CANFD_REVE_TARGET
    if(!initialized && reve_power_vbus() && !usb_device_init()) return;
    if(initialized && !vbus_attached && reve_power_vbus()) {
        uint32_t mask=__get_PRIMASK(); __disable_irq();
        if(reve_power_vbus()) { tud_connect(); vbus_attached=true; }
        __set_PRIMASK(mask);
    }
#endif
#ifdef REVD_FREERTOS
    if (!initialized) return;
    usb_callbacks_task();
#else
    if (!initialized || transport_active) return;
    usb_device_poll_transport();
#endif
    lifecycle_task();
    if (!observed_bus || !dtr || !tud_cdc_connected() || !reported_host) return;
    output_flush(); input_task(); stream_task(); output_flush();
}
#ifdef CANFD_REVE_TARGET
void usb_device_vbus_tick(void)
{
    if(initialized && vbus_attached && !reve_power_vbus()) {
        /* This pinned TinyUSB DWC2 implementation only sets DCTL.SDIS;
         * no queue, allocation or filesystem work runs from SysTick. */
        tud_disconnect(); vbus_attached=false;
        reve_power_usb_reset();
    }
}
#endif
void OTG_FS_IRQHandler(void) {
#ifdef CANFD_REVE_TARGET
    /* Take the electrical current limit down before a queued USB callback.
     * It must work even when foreground is occupied by a slow SD write. */
    uint32_t pending=USB_OTG_FS->GINTSTS & USB_OTG_FS->GINTMSK;
    if(pending & USB_OTG_GINTSTS_USBRST) reve_power_usb_reset();
    if(pending & USB_OTG_GINTSTS_USBSUSP) reve_power_usb_suspend(true);
    if(pending & USB_OTG_GINTSTS_WKUINT) reve_power_usb_suspend(false);
#endif
    tud_int_handler(0);
}
#ifdef REVD_FREERTOS
static void usb_callbacks_task(void)
{
    revd_usb_callback_kind_t kind; bool value;
    for (unsigned i = 0; i < 16 && revd_usb_callback_pop(&kind, &value); ++i) {
        switch (kind) {
        case REVD_USB_MOUNT:
            mounted_session = true; last_sof_ms = HAL_GetTick();
            last_usb_frame = received_frame_number(); break;
        case REVD_USB_UNMOUNT: mounted_session = false; dtr = false; reset_session(); break;
        case REVD_USB_RESUME: last_sof_ms = HAL_GetTick(); break;
        case REVD_USB_DTR:
            dtr = value; reset_session();
            if (dtr) reply("CANFD-REVD CDC 1 READY; HELP\r\n");
            break;
        }
    }
}
#endif
void tud_mount_cb(void)
{
#ifdef CANFD_REVE_TARGET
    reve_power_usb_configured(reve_power_vbus());
#endif
#ifdef REVD_FREERTOS
    revd_usb_callback(REVD_USB_MOUNT, true);
#else
    mounted_session = true; last_sof_ms = HAL_GetTick();
    last_usb_frame = received_frame_number();
#endif
}
void tud_umount_cb(void) {
#ifdef CANFD_REVE_TARGET
    reve_power_usb_configured(false);
#endif
#ifdef REVD_FREERTOS
    revd_usb_callback(REVD_USB_UNMOUNT, false);
#else
    mounted_session = false; dtr = false;
#endif
}
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en;
#ifdef CANFD_REVE_TARGET
    reve_power_usb_suspend(true);
#endif
}
void tud_resume_cb(void) {
#ifdef CANFD_REVE_TARGET
    reve_power_usb_suspend(false);
#endif
#ifdef REVD_FREERTOS
    revd_usb_callback(REVD_USB_RESUME, true);
#else
    last_sof_ms = HAL_GetTick();
#endif
}
void tud_cdc_line_state_cb(uint8_t itf, bool dtr_state, bool rts_state)
{
    (void)itf; (void)rts_state;
#ifdef REVD_FREERTOS
    revd_usb_callback(REVD_USB_DTR, dtr_state);
#else
    dtr = dtr_state;
    reset_session();
    if (dtr) reply("CANFD-REVD CDC 1 READY; HELP\r\n");
#endif
}

static bool media_check(uint8_t lun)
{
    if (lun == 0u && usb_device_media_ready()) return true;
    (void)tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
    return false;
}
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "REVDDEV ", 8); memcpy(product_id, "EXCLUSIVE SD    ", 16); memcpy(product_rev, "0001", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) { return media_check(lun); }
void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *block_size)
{
    *block_size = 512; *count = media_check(lun) ? storage_sd_raw_block_count() : 0u;
}
bool tud_msc_is_writable_cb(uint8_t lun) { return media_check(lun); }
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t size)
{
    if (!media_check(lun)) return -1;
    if (offset >= 512u || size > 512u - offset || lba >= storage_sd_raw_block_count()) {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0); return -1;
    }
    if (!storage_sd_raw_read(lba, sector, 1)) {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0); return -1;
    }
    memcpy(buffer, sector + offset, size);
    return (int32_t)size;
}
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size)
{
    if (!media_check(lun)) return -1;
    if (offset >= 512u || size > 512u - offset || lba >= storage_sd_raw_block_count()) {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0); return -1;
    }
    if ((offset != 0u || size != 512u) && !storage_sd_raw_read(lba, sector, 1)) {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0); return -1;
    }
    memcpy(sector + offset, buffer, size);
    if (!storage_sd_raw_write(lba, sector, 1)) {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0c, 0); return -1;
    }
    return (int32_t)size;
}
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    if (lun != 0u) return false;
    if (load_eject && !start) {
        if (media_shared && !storage_sd_raw_sync()) {
            (void)tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0c, 0); return false;
        }
        /* Hide immediately; service performs exclusive ownership reclaim. */
        media_shared = false;
        eject_pending = !evt_post4(EV_USB_EJECT, 0, SRC_SYS, 0, 0);
        return true;
    }
    /* Host START cannot grant itself storage access. */
    return !start || media_check(lun);
}
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t command[16], void *buffer, uint16_t size)
{
    (void)buffer; (void)size;
    if (command[0] == SCSI_SYNCHRONIZE_CACHE_10) {
        if (!media_check(lun)) return -1;
        if (storage_sd_raw_sync()) return 0;
        (void)tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0c, 0); return -1;
    }
    (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0); return -1;
}

#ifdef REVD_FREERTOS
/* These are the real TinyUSB calls. CDC convenience macro names above refer
 * to bridge FIFOs only in core/control code; transport uses explicit n APIs. */
#undef tud_mounted
#undef tud_suspended
void revd_usb_transport_step(void)
{
    if (!revd_usb_core_ready() || !initialized) return;
    configASSERT(xTaskGetCurrentTaskHandle() == revd_rtos_task_handle(REVD_RTOS_USB));
    tud_task_ext(0, false);
    bool mounted = tud_mounted(), suspended = tud_suspended();
    bool connected = tud_cdc_n_connected(0);
    revd_usb_publish_link(mounted, suspended, connected);
    if (!mounted || suspended || !connected) return;
    uint8_t bytes[128];
    uint32_t space = revd_usb_transport_read_space();
    if (space > sizeof bytes) space = sizeof bytes;
    if (space && tud_cdc_n_available(0)) {
        uint32_t n = tud_cdc_n_read(0, bytes, space);
        (void)revd_usb_transport_receive(bytes, n);
    }
    uint32_t room = tud_cdc_n_write_available(0);
    if (room > sizeof bytes) room = sizeof bytes;
    uint32_t n = revd_usb_transport_send_peek(bytes, room);
    if (n) revd_usb_transport_send_advance(tud_cdc_n_write(0, bytes, n));
    (void)tud_cdc_n_write_flush(0);
}
#endif
