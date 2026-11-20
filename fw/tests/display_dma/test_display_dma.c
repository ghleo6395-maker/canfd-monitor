/* Real production display.c + real pinned LVGL; only hardware and app edges
 * are doubled. Include the source so ownership/data snapshots can be checked. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../platform/stm32h743_revd/display.c"
#include "../../platform/stm32h743_revd/can_ingress.h"
#include "services/frame_ring/frame_ring.h"
SPI_TypeDef test_spi4;
TIM_TypeDef test_tim1;
DMA_Stream_TypeDef test_stream0;
uint32_t test_compare;
static uint32_t tick, poll_calls, dma_calls, dma_cancels, irq_clears;
static uint32_t ui_cost_ms;
static uint32_t cs = LCD_CS, dc;
static bool pending, fail_start;
static const uint8_t *borrowed;
static uint16_t borrowed_len;
static SPI_HandleTypeDef *borrowed_handle;
static uint8_t wire[sizeof dma_buffer];
static power_t test_power = { .screen_on = true };
static lv_obj_t *label;
static unsigned checks;
static uint32_t yield_calls, captured_frames;
static bool yield_complete_dma;
static can_ingress_t ingress[2];
static ring_reader_t capture_reader;
static void complete(void);
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n", __LINE__, #x); abort(); } } while(0)
uint32_t HAL_GetTick(void) { return tick; }
void HAL_Delay(uint32_t ms) { tick += ms; }
void HAL_GPIO_WritePin(void *p, uint32_t pins, GPIO_PinState v) {
    (void)p;
    if (pins & LCD_CS) cs = v == GPIO_PIN_SET ? LCD_CS : 0;
    if (pins & LCD_DC) dc = v == GPIO_PIN_SET ? LCD_DC : 0;
}
void HAL_GPIO_Init(void *p, GPIO_InitTypeDef *g) { (void)p; (void)g; }
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t) {
    (void)h; (void)p; (void)n; (void)t; CHECK(!pending); poll_calls++; return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Transmit_DMA(SPI_HandleTypeDef *h, const uint8_t *p, uint16_t n) {
    CHECK(!pending); CHECK(cs == 0); dma_calls++;
    if (fail_start) { fail_start = false; return HAL_ERROR; }
    pending = true; borrowed = p; borrowed_len = n; borrowed_handle = h;
    return HAL_OK;
}
void HAL_SPI_IRQHandler(SPI_HandleTypeDef *h) { (void)h; }
HAL_StatusTypeDef HAL_DMA_Init(DMA_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_DMA_DeInit(DMA_HandleTypeDef *h) { (void)h; pending = false; dma_cancels++; return HAL_OK; }
void HAL_DMA_IRQHandler(DMA_HandleTypeDef *h) { (void)h; }
void HAL_NVIC_SetPriority(int i, unsigned p, unsigned s) { (void)i; (void)p; (void)s; }
void HAL_NVIC_ClearPendingIRQ(int i) { (void)i; irq_clears++; }
void HAL_NVIC_EnableIRQ(int i) { (void)i; }
void HAL_NVIC_DisableIRQ(int i) { (void)i; }
HAL_StatusTypeDef HAL_TIM_PWM_Init(TIM_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_PWM_ConfigChannel(TIM_HandleTypeDef *h, TIM_OC_InitTypeDef *o, uint32_t c) { (void)h; (void)o; (void)c; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *h, uint32_t c) { (void)h; (void)c; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef *h, uint32_t c) { (void)h; (void)c; return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_PWM_DeInit(TIM_HandleTypeDef *h) { (void)h; return HAL_OK; }
bool touch_ft6336_init(void) { return true; }
static bool mock_touch_pressed, mock_touch_error;
bool touch_ft6336_read(uint16_t *x, uint16_t *y, bool *p) {
    *x = *y = 25; *p = mock_touch_pressed && !mock_touch_error; return !mock_touch_error;
}
#ifdef CANFD_REVE_TARGET
bool touch_ft6336_service(uint32_t now) { (void)now; return !mock_touch_error; }
static unsigned touch_clicks;
static void click_seen(lv_event_t *e) { (void)e; touch_clicks++; }
#endif
void touch_ft6336_deinit(void) { }
const power_t *power_get(void) { return &test_power; }
void app_touch(ev_type_t e, int32_t a) { (void)e; (void)a; }
void ui_lvgl_init(lv_display_t *d) {
    label = lv_label_create(lv_display_get_screen_active(d));
    lv_label_set_text(label, "Rev D DMA ownership test");
}
void ui_lvgl_task(uint32_t ms) { (void)ms; tick += ui_cost_ms; }
uint32_t ui_lvgl_snapshot_errors(void) { return 0; }
/* The target hook has the same no-LVGL/no-FatFs contract. Feed actual
 * production ingress queues -> production frame ring while a render is active. */
void revd_io_wait_poll(void) {
    yield_calls++;
    for (unsigned ch = 0; ch < 2; ch++) {
        can_frame_t in = { .id = 0x123, .ch = ch, .len = 64, .flags = CANF_FD };
        memset(in.data, (uint8_t)captured_frames, sizeof in.data);
        can_frame_t out;
        CHECK(can_ingress_push(&ingress[ch], &in));
        CHECK(can_ingress_peek(&ingress[ch], &out));
        CHECK(ring_push(&out)); can_ingress_advance(&ingress[ch]);
        const can_frame_t *recorded = ring_peek(&capture_reader);
        CHECK(recorded && recorded->ch == ch && recorded->len == 64);
        CHECK(recorded->data[63] == (uint8_t)captured_frames);
        ring_advance(&capture_reader); captured_frames++;
    }
    if (yield_complete_dma && pending) complete();
    display_dma_poll(tick);
}
static void complete(void) {
    CHECK(pending); memcpy(wire, borrowed, borrowed_len);
    pending = false; HAL_SPI_TxCpltCallback(borrowed_handle);
}
static void step_complete(void) { complete(); tick++; display_task(tick); }
static void init_clean(void) {
    CHECK(display_init()); display_set_backlight(50); CHECK(test_compare == 500);
    /* Let actual LVGL render and copy every initial partial area. DMA callbacks
     * are deliberately withheld: lv_timer_handler must still return. */
    tick += 101; display_task(tick);
    CHECK(diag.shadow_flushes >= 16); CHECK(pending);
}
static void drain(void) {
    unsigned guard = 200;
    while ((pending || dirty_bands) && guard--) {
        if (pending) complete(); tick++; display_task(tick);
    }
    CHECK(guard > 0); CHECK(!pending); CHECK(dirty_bands == 0);
}
int main(void) {
    for (unsigned ch = 0; ch < 2; ch++) can_ingress_init(&ingress[ch]);
    ring_init(); ring_reader_attach(&capture_reader, true);
    init_clean();
    CHECK(yield_calls == diag.shadow_flushes);
    CHECK(captured_frames == yield_calls * 2u);
    uint32_t poll_before = poll_calls;
    CHECK(dma_phase == DMA_COLUMN_CMD && borrowed_len == 1 && borrowed[0] == 0x2a && dc == 0);
    for (unsigned i = 0; i < 20; i++) { tick++; display_task(tick); }
    CHECK(poll_calls == poll_before); CHECK(dma_phase == DMA_COLUMN_CMD);
    step_complete(); CHECK(dma_phase == DMA_COLUMN_DATA && borrowed_len == 4 && dc != 0);
    CHECK(borrowed[0] == 0 && borrowed[1] == 0 && borrowed[2] == 1 && borrowed[3] == 0xdf);
    step_complete(); CHECK(dma_phase == DMA_ROW_CMD && borrowed[0] == 0x2b && dc == 0);
    step_complete(); CHECK(dma_phase == DMA_ROW_DATA && borrowed_len == 4 && borrowed[3] == 19);
    step_complete(); CHECK(dma_phase == DMA_MEMORY_CMD && borrowed[0] == 0x2c && dc == 0);
    step_complete(); CHECK(dma_phase == DMA_PIXELS && borrowed_len == sizeof dma_buffer && dc != 0);
    uint8_t stable[sizeof dma_buffer]; memcpy(stable, borrowed, sizeof stable);
    uint32_t flushes = diag.shadow_flushes;
    lv_label_set_text(label, "Real LVGL redraw while pixel DMA is delayed");
    lv_timer_ready(lv_display_get_refr_timer(lcd));
    tick++; display_task(tick);
    CHECK(pending && dma_phase == DMA_PIXELS);
    CHECK(diag.shadow_flushes > flushes);
    CHECK(memcmp(stable, borrowed, sizeof stable) == 0);
    CHECK(poll_calls == poll_before);
    uint8_t pixel[] = {0x34, 0x12}; lv_area_t one = { .x1=0, .y1=0, .x2=0, .y2=0 };
    shadow_flush_cb(lcd, &one, pixel);
    CHECK(dirty_bands & 1u); CHECK(memcmp(stable, borrowed, sizeof stable) == 0);
    /* Only shadow changed; the current DMA owns an immutable copy. */
    drain(); CHECK(diag.dma_bands >= 17); CHECK(poll_calls == poll_before);
    CHECK(shadow[0][0] == 0x34 && shadow[0][1] == 0x12);
    /* Isolate and transmit a fresh band to verify panel RGB565 wire byte order. */
    shadow_flush_cb(lcd, &one, pixel); tick++; display_task(tick);
    CHECK(dma_phase == DMA_COLUMN_CMD); CHECK(dma_buffer[0] == 0x12 && dma_buffer[1] == 0x34);
    for (unsigned i = 0; i < 5; i++) step_complete();
    CHECK(dma_phase == DMA_PIXELS); complete(); tick++; display_task(tick);
    CHECK(wire[0] == 0x12 && wire[1] == 0x34); CHECK(!pending);
    /* Full real LVGL redraw yields once per partial copy. The hook accepts
     * dual-channel frames and advances delayed DMA without recursive render. */
    uint32_t calls_before = dma_calls, yields_before = yield_calls;
    uint32_t captured_before = captured_frames, flushes_before = diag.shadow_flushes;
    uint32_t forced_before = diag.forced_redraws;
    port_screen_redraw();
    CHECK(redraw_requested && diag.forced_redraws == forced_before);
    lv_timer_ready(lv_display_get_refr_timer(lcd));
    yield_complete_dma = true; tick++; display_task(tick); yield_complete_dma = false;
    CHECK(!redraw_requested && diag.forced_redraws == forced_before + 1);
    CHECK(diag.shadow_flushes - flushes_before >= 16);
    CHECK(yield_calls - yields_before == diag.shadow_flushes - flushes_before);
    CHECK(captured_frames - captured_before == (yield_calls - yields_before) * 2u);
    CHECK(dma_calls > calls_before + 1);
    CHECK(can_ingress_dropped(&ingress[0]) == 0 && can_ingress_dropped(&ingress[1]) == 0);
    CHECK(ring_dropped() == 0);
    drain();
    /* Driver errors are latched, DMA and stale NVIC state are cancelled. */
    shadow_flush_cb(lcd, &one, pixel); tick++; display_task(tick);
    uint32_t errors = diag.spi_errors, clears = irq_clears;
    HAL_SPI_ErrorCallback(&spi); tick++; display_task(tick);
    CHECK(!diag.active && !pending && diag.spi_errors == errors + 1);
    CHECK(test_compare == 0 && irq_clears >= clears + 2);
    HAL_SPI_TxCpltCallback(&spi); CHECK(!dma_done);
    display_poweroff();
    /* Timed-out DMA, including tick wrap, never waits in a driver call. */
    init_clean(); tick = UINT32_MAX - 10; dma_started_at = tick;
    tick += DMA_DEADLINE_MS; display_task(tick);
    CHECK(!diag.active && !pending && diag.dma_timeouts >= 1);
    display_poweroff();
    /* Immediate DMA start failure releases LVGL ownership and disables panel. */
    CHECK(display_init()); fail_start = true; tick += 101; display_task(tick);
    CHECK(!diag.active && !pending && !diag.dma_busy);
    display_poweroff();
    /* Poweroff during a live DMA cancels it before GPIO high impedance. */
    init_clean(); CHECK(pending);
    for (unsigned i = 0; i < 5; i++) step_complete();
    CHECK(dma_phase == DMA_PIXELS);
    uint32_t cancelled = diag.dma_cancelled;
    display_poweroff(); CHECK(!pending && !diag.active && !diag.dma_busy);
    CHECK(diag.dma_cancelled == cancelled + 1 && dirty_bands == 0);
    HAL_SPI_TxCpltCallback(&spi); HAL_SPI_ErrorCallback(&spi);
    CHECK(!dma_done && !dma_error);
    /* A new initialization works after every fault, with all LVGL objects
     * replaced in foreground; no stale transfer is mistaken for new work. */
    init_clean(); drain(); CHECK(diag.active && !pending);
    /* UI/snapshot/LVGL timing is foreground wall time, including tick wrap;
     * DMA wire time is outside this measurement. */
    tick = UINT32_MAX - 3u; ui_cost_ms = 7;
    display_task(tick); ui_cost_ms = 0;
    CHECK(diag.max_render_ms == 7);
#ifdef CANFD_REVE_TARGET
    /* Actual display read callback + actual LVGL: a failed read while a button
     * is pressed must cancel it, never turn into a release/click. */
    lv_obj_t *button = lv_button_create(lv_screen_active());
    lv_obj_set_pos(button, 10, 10); lv_obj_set_size(button, 100, 50);
    lv_obj_add_event_cb(button, click_seen, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(button);
    mock_touch_pressed = true; lv_indev_read(touch);
    mock_touch_error = true; lv_indev_read(touch); CHECK(touch_clicks == 0);
    tick++; display_task(tick); CHECK(!diag.touch_present);
    mock_touch_pressed = false; mock_touch_error = false; lv_indev_read(touch);
    tick++; display_task(tick); CHECK(diag.touch_present);
    mock_touch_pressed = true; lv_indev_read(touch);
    mock_touch_pressed = false; lv_indev_read(touch); CHECK(touch_clicks == 1);
#endif
    display_poweroff(); CHECK(dma_cancels > 0);
    printf("display DMA production + real LVGL: %u checks passed\n", checks);
    return 0;
}
