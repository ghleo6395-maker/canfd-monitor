/* Rev D LVGL 9.5 / ST7796S asynchronous SPI4 transport.
 * Init commands alone use short polling transfers before recording starts.
 * Runtime: LVGL copies into a shadow image, then releases its draw buffer;
 * main-loop DMA sends one stable 20-line snapshot. No SPI wire-time wait.
 * D-cache MUST remain disabled; both DMA buffers live in DMA1-accessible D2. */
#include "display.h"
#include "touch_ft6336.h"
#include "ui_lvgl.h"
#include "app/app.h"
#include "services/power/power_sm.h"
#ifdef REVD_FREERTOS
#include "rtos/bridges.h"
#include "rtos/runtime.h"
#define power_get revd_ui_power
#define app_touch revd_ui_touch
#endif
#include "stm32h7xx_hal.h"
#include "lvgl.h"
#include "src/drivers/display/st7796/lv_st7796.h"
#include "src/drivers/display/lcd/lv_lcd_generic_mipi.h"
#include <string.h>

#define LCD_CS GPIO_PIN_11
#define LCD_DC GPIO_PIN_10
#define LCD_RST GPIO_PIN_15
#define LCD_W 480u
#define LCD_H 320u
#define BAND_H 20u
#define BAND_COUNT (LCD_H / BAND_H)
#define DMA_DEADLINE_MS 100u
#ifdef CANFD_REVD_TARGET
#define DISPLAY_D2 __attribute__((section(".revd_d2_bss"), aligned(32)))
#else
#define DISPLAY_D2 __attribute__((aligned(32)))
#endif
static SPI_HandleTypeDef spi;
static DMA_HandleTypeDef spi_dma;
static TIM_HandleTypeDef pwm;
static lv_display_t *lcd;
static lv_indev_t *touch;
static bool lv_started;
static bool swallow_touch;
static bool buses_up;
static bool runtime_started;
static bool redraw_requested;
static uint8_t requested_bl;
static uint8_t draw_buffer[LCD_W * BAND_H * 2u] DISPLAY_D2;
static uint8_t dma_buffer[LCD_W * BAND_H * 2u] DISPLAY_D2;
static uint8_t dma_command[32] DISPLAY_D2;
/* CPU-owned, never DMA-read: draw callbacks may update this during a DMA. */
static uint8_t shadow[LCD_H][LCD_W * 2u];
static uint32_t dirty_bands;
static uint8_t next_band;
static uint16_t dma_y;
typedef enum { DMA_IDLE, DMA_COLUMN_CMD, DMA_COLUMN_DATA, DMA_ROW_CMD,
               DMA_ROW_DATA, DMA_MEMORY_CMD, DMA_PIXELS } dma_phase_t;
static volatile dma_phase_t dma_phase;
static volatile bool dma_done;
static volatile bool dma_error;
static uint32_t dma_started_at;
static display_diag_t diag;
static uint32_t service_last_ms;
static bool service_time_valid;
/* Foreground-only, reentrancy-guarded CAN/recorder/transport pump; it must not
 * enter LVGL, dispatch UI events, or call FatFs. */
extern void revd_io_wait_poll(void);

/* Quiesce without HAL_SPI_Abort's SPI wire-time polling. This is a fault or
 * shutdown path: invalidate ownership first, stop requests, reset SPI4, stop
 * this one dedicated DMA stream, clear both peripheral and NVIC pending IRQs.
 * No DMA1-wide reset: other streams can continue serving future peripherals. */
static void dma_quiesce(void)
{
    bool was_busy = dma_phase != DMA_IDLE;
    dma_phase = DMA_IDLE;
    HAL_NVIC_DisableIRQ(SPI4_IRQn);
    HAL_NVIC_DisableIRQ(DMA1_Stream0_IRQn);
    if (spi.Instance) {
        spi.Instance->IER = 0;
        CLEAR_BIT(spi.Instance->CFG1, SPI_CFG1_TXDMAEN | SPI_CFG1_RXDMAEN);
        __HAL_RCC_SPI4_FORCE_RESET();
        __HAL_RCC_SPI4_RELEASE_RESET();
    }
    if (spi_dma.Instance) (void)HAL_DMA_DeInit(&spi_dma);
    HAL_NVIC_ClearPendingIRQ(SPI4_IRQn);
    HAL_NVIC_ClearPendingIRQ(DMA1_Stream0_IRQn);
    dma_done = false;
    dma_error = false;
    if (buses_up) HAL_GPIO_WritePin(GPIOE, LCD_CS, GPIO_PIN_SET);
    if (was_busy) diag.dma_cancelled++;
    diag.dma_busy = false;
}
static void dma_fault(bool timeout)
{
    diag.spi_errors++;
    if (timeout) diag.dma_timeouts++;
    diag.active = false;
    service_time_valid = false;
    requested_bl = 0;
    if (pwm.Instance) __HAL_TIM_SET_COMPARE(&pwm, TIM_CHANNEL_1, 0);
    dma_quiesce();
    dirty_bands = 0;
    diag.dirty_bands = 0;
}
static bool tx_init(const uint8_t *bytes, size_t n)
{
    if (!buses_up || runtime_started || n > UINT16_MAX) return false;
    if (HAL_SPI_Transmit(&spi, (uint8_t *)(uintptr_t)bytes, (uint16_t)n, 20) != HAL_OK) {
        dma_fault(false);
        return false;
    }
    return true;
}
static void command_cb(lv_display_t *disp, const uint8_t *cmd, size_t cmd_len,
                       const uint8_t *param, size_t param_len)
{
    (void)disp;
    if (!diag.active) return;
    /* Runtime rotation/other raw MIPI writes are unsupported: never let a
     * command alter CS/DC while a snapshot is being transmitted. */
    if (runtime_started || dma_phase != DMA_IDLE) { dma_fault(false); return; }
    HAL_GPIO_WritePin(GPIOE, LCD_CS | LCD_DC, GPIO_PIN_RESET);
    bool ok = tx_init(cmd, cmd_len);
    HAL_GPIO_WritePin(GPIOE, LCD_DC, GPIO_PIN_SET);
    if (ok && param_len) (void)tx_init(param, param_len);
    HAL_GPIO_WritePin(GPIOE, LCD_CS, GPIO_PIN_SET);
}
static void color_cb(lv_display_t *disp, const uint8_t *cmd, size_t cmd_len,
                     uint8_t *pixels, size_t n)
{
    /* The generic MIPI driver's runtime flush is replaced below; retain a
     * safe release for an unexpected direct use instead of deadlocking LVGL. */
    (void)cmd; (void)cmd_len; (void)pixels; (void)n;
    dma_fault(false);
    lv_display_flush_ready(disp);
}
static void shadow_flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *pixels)
{
    if (diag.active && a->x1 >= 0 && a->y1 >= 0 && a->x2 < (int32_t)LCD_W &&
        a->y2 < (int32_t)LCD_H && a->x2 >= a->x1 && a->y2 >= a->y1) {
        size_t bytes = (size_t)(a->x2 - a->x1 + 1) * 2u;
        for (int32_t y = a->y1; y <= a->y2; y++) {
            memcpy(&shadow[y][(size_t)a->x1 * 2u], pixels, bytes);
            pixels += bytes;
        }
        uint32_t bits = 0;
        for (uint32_t b = (uint32_t)a->y1 / BAND_H;
             b <= (uint32_t)a->y2 / BAND_H; b++) bits |= 1u << b;
        if (dirty_bands & bits) diag.coalesced_updates++;
        dirty_bands |= bits;
        diag.dirty_bands = dirty_bands;
        diag.shadow_flushes++;
    }
    /* LVGL is released after the COPY, never while DMA references its buffer.
     * Hence its internal single-buffer wait observes ready immediately. */
    lv_display_flush_ready(disp);
    /* A full redraw emits many partial blocks in one lv_timer_handler call.
     * Give dual-CAN capture a chance after EACH copied block, without waiting
     * for DMA and without re-entering the LVGL renderer. */
    revd_io_wait_poll();
}
static bool dma_start_phase(uint32_t now_ms)
{
    const uint8_t *source = dma_command;
    uint16_t n = 1;
    bool data = false;
    switch (dma_phase) {
    case DMA_COLUMN_CMD: dma_command[0] = 0x2a; break;
    case DMA_COLUMN_DATA:
        dma_command[0] = 0; dma_command[1] = 0;
        dma_command[2] = (LCD_W - 1u) >> 8; dma_command[3] = (LCD_W - 1u) & 255;
        n = 4; data = true; break;
    case DMA_ROW_CMD: dma_command[0] = 0x2b; break;
    case DMA_ROW_DATA:
        dma_command[0] = dma_y >> 8; dma_command[1] = dma_y & 255;
        dma_command[2] = (dma_y + BAND_H - 1u) >> 8;
        dma_command[3] = (dma_y + BAND_H - 1u) & 255;
        n = 4; data = true; break;
    case DMA_MEMORY_CMD: dma_command[0] = 0x2c; break;
    case DMA_PIXELS: source = dma_buffer; n = sizeof dma_buffer; data = true; break;
    default: return false;
    }
    HAL_GPIO_WritePin(GPIOE, LCD_DC, data ? GPIO_PIN_SET : GPIO_PIN_RESET);
    dma_done = false;
    dma_started_at = now_ms;
    diag.dma_busy = true;
#ifdef CANFD_REVD_TARGET
    /* Publish all CPU snapshot/command stores before DMA1 can read them. */
    __DMB();
#endif
    if (HAL_SPI_Transmit_DMA(&spi, source, n) != HAL_OK) {
        dma_fault(false); return false;
    }
    diag.dma_transfers++;
    return true;
}
void display_dma_poll(uint32_t now_ms)
{
    if (!diag.active || !buses_up) return;
    if (dma_phase != DMA_IDLE) {
        if (dma_error) { dma_fault(false); return; }
        if (!dma_done) {
            if ((uint32_t)(now_ms - dma_started_at) >= DMA_DEADLINE_MS) dma_fault(true);
            return;
        }
        dma_done = false;
        if (dma_phase != DMA_PIXELS) {
            dma_phase = (dma_phase_t)(dma_phase + 1);
            (void)dma_start_phase(now_ms);
            return;
        }
        HAL_GPIO_WritePin(GPIOE, LCD_CS, GPIO_PIN_SET);
        dma_phase = DMA_IDLE;
        diag.dma_busy = false;
        diag.dma_bands++;
    }
    if (!dirty_bands || !power_get()->screen_on) return;
    for (uint32_t i = 0; i < BAND_COUNT; i++) {
        uint8_t band = (uint8_t)((next_band + i) % BAND_COUNT);
        if (!(dirty_bands & (1u << band))) continue;
        dma_y = band * BAND_H;
        memcpy(dma_buffer, shadow[dma_y], sizeof dma_buffer);
        lv_draw_sw_rgb565_swap(dma_buffer, sizeof dma_buffer / 2u);
        /* Clear before DMA starts. A newer LVGL copy sets this bit again and
         * completion must NOT clear it, so ongoing updates cannot disappear. */
        dirty_bands &= ~(1u << band);
        diag.dirty_bands = dirty_bands;
        next_band = (uint8_t)((band + 1u) % BAND_COUNT);
        dma_phase = DMA_COLUMN_CMD;
        HAL_GPIO_WritePin(GPIOE, LCD_CS, GPIO_PIN_RESET);
        (void)dma_start_phase(now_ms);
        return;
    }
}
void DMA1_Stream0_IRQHandler(void) { HAL_DMA_IRQHandler(&spi_dma); }
void SPI4_IRQHandler(void) { HAL_SPI_IRQHandler(&spi); }
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *handle)
{
    if (handle == &spi && dma_phase != DMA_IDLE) {
        dma_done = true;
#ifdef REVD_FREERTOS
        revd_rtos_notify_ui_from_isr();
#endif
    }
}
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *handle)
{
    if (handle == &spi && dma_phase != DMA_IDLE) {
        dma_error = true;
#ifdef REVD_FREERTOS
        revd_rtos_notify_ui_from_isr();
#endif
    }
}
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x = 0, y = 0;
    bool pressed = false;
    bool ok = diag.active && touch_ft6336_read(&x, &y, &pressed);
    data->state = LV_INDEV_STATE_RELEASED;
#ifdef CANFD_REVE_TARGET
    /* A transfer failure is cancellation, never a user's release/click. */
    if (!ok) lv_indev_reset(indev, NULL);
#endif
    if (!ok || !pressed) { swallow_touch = false; return; }
    /* Consume the complete first contact after sleep; it must not confirm a
     * still-visible transmission/format dialog as it wakes the backlight. */
    if (!power_get()->screen_on) {
        app_touch(EV_NONE, 0);
        swallow_touch = true;
    }
    if (swallow_touch) return;
    data->point.x = x; data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

static bool spi_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOE_CLK_ENABLE(); __HAL_RCC_SPI4_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOE, LCD_CS, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOE, LCD_DC | LCD_RST, GPIO_PIN_RESET);
    g.Pin = LCD_CS | LCD_DC | LCD_RST;
    g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH; HAL_GPIO_Init(GPIOE, &g);
    g.Pin = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14;
    g.Mode = GPIO_MODE_AF_PP; g.Alternate = GPIO_AF5_SPI4;
    HAL_GPIO_Init(GPIOE, &g);
    spi.Instance = SPI4;
    spi.Init.Mode = SPI_MODE_MASTER; spi.Init.Direction = SPI_DIRECTION_2LINES;
    spi.Init.DataSize = SPI_DATASIZE_8BIT;
    spi.Init.CLKPolarity = SPI_POLARITY_LOW; spi.Init.CLKPhase = SPI_PHASE_1EDGE;
    spi.Init.NSS = SPI_NSS_SOFT; spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    spi.Init.FirstBit = SPI_FIRSTBIT_MSB; spi.Init.TIMode = SPI_TIMODE_DISABLE;
    spi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE; spi.Init.CRCPolynomial = 0;
    spi.Init.NSSPMode = SPI_NSS_PULSE_DISABLE; spi.Init.NSSPolarity = SPI_NSS_POLARITY_LOW;
    spi.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    spi.Init.TxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    spi.Init.RxCRCInitializationPattern = SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;
    spi.Init.MasterSSIdleness = SPI_MASTER_SS_IDLENESS_00CYCLE;
    spi.Init.MasterInterDataIdleness = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
    spi.Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_DISABLE;
    spi.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_DISABLE;
    spi.Init.IOSwap = SPI_IO_SWAP_DISABLE;
    if (HAL_SPI_Init(&spi) != HAL_OK) return false;
    __HAL_RCC_DMA1_CLK_ENABLE();
    spi_dma.Instance = DMA1_Stream0;
    spi_dma.Init.Request = DMA_REQUEST_SPI4_TX;
    spi_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    spi_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    spi_dma.Init.MemInc = DMA_MINC_ENABLE;
    spi_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    spi_dma.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    spi_dma.Init.Mode = DMA_NORMAL;
    spi_dma.Init.Priority = DMA_PRIORITY_LOW;
    spi_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    spi_dma.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
    spi_dma.Init.MemBurst = DMA_MBURST_SINGLE;
    spi_dma.Init.PeriphBurst = DMA_PBURST_SINGLE;
    if (HAL_DMA_Init(&spi_dma) != HAL_OK) return false;
    __HAL_LINKDMA(&spi, hdmatx, spi_dma);
    /* Lower urgency than both CAN RX IRQs and the SDMMC completion IRQ. */
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 7, 0);
    HAL_NVIC_SetPriority(SPI4_IRQn, 7, 0);
    HAL_NVIC_ClearPendingIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_ClearPendingIRQ(SPI4_IRQn);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_EnableIRQ(SPI4_IRQn);
    return true;
}
static bool backlight_init(void)
{
    __HAL_RCC_TIM1_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_9; g.Mode = GPIO_MODE_AF_PP; g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW; g.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOE, &g);
    pwm.Instance = TIM1;
    pwm.Init.Prescaler = 199; /* APB2 timer 200 MHz -> 1 MHz */
    pwm.Init.CounterMode = TIM_COUNTERMODE_UP; pwm.Init.Period = 999;
    pwm.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    pwm.Init.RepetitionCounter = 0; pwm.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM1; oc.Pulse = 0;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH; oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    oc.OCIdleState = TIM_OCIDLESTATE_RESET; oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    return HAL_TIM_PWM_Init(&pwm) == HAL_OK &&
           HAL_TIM_PWM_ConfigChannel(&pwm, &oc, TIM_CHANNEL_1) == HAL_OK &&
           HAL_TIM_PWM_Start(&pwm, TIM_CHANNEL_1) == HAL_OK;
}

bool display_init(void)
{
    if (buses_up) display_poweroff();
#ifdef CANFD_REVE_TARGET
    /* TPS22919: keep every interface high impedance until its rail settles. */
    display_poweroff();
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(5);
#endif
    diag.active = false;
    requested_bl = 0;
    runtime_started = false;
    dma_phase = DMA_IDLE;
    dma_done = false; dma_error = false;
    dirty_bands = 0; next_band = 0;
    memset(shadow, 0, sizeof shadow);
    /* Only init happens outside LVGL dispatch. Poweroff retains objects safely. */
    if (!lv_started) {
        lv_init();
        lv_tick_set_cb(HAL_GetTick);
        lv_delay_set_cb(HAL_Delay);
        lv_started = true;
    }
    if (touch) { lv_indev_delete(touch); touch = NULL; }
    if (lcd) { lv_display_delete(lcd); lcd = NULL; }
    diag.touch_present = touch_ft6336_init();
    diag.panel_id_verified = false;
#ifndef CANFD_REVE_TARGET
    if (!diag.touch_present) return false;
#endif
    if (!spi_init() || !backlight_init()) { display_poweroff(); return false; }
    buses_up = true;
    HAL_GPIO_WritePin(GPIOE, LCD_RST, GPIO_PIN_RESET); HAL_Delay(20);
    HAL_GPIO_WritePin(GPIOE, LCD_RST, GPIO_PIN_SET); HAL_Delay(120);
    diag.active = true;
    lcd = lv_st7796_create(480, 320, LV_LCD_FLAG_BGR, command_cb, color_cb);
    if (!lcd || !diag.active) { display_poweroff(); return false; }
    lv_lcd_generic_mipi_set_address_mode(lcd, true, false, true, true);
    lv_display_set_color_format(lcd, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(lcd, shadow_flush_cb);
    runtime_started = true;
    lv_display_set_buffers(lcd, draw_buffer, NULL, sizeof draw_buffer, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ui_lvgl_init(lcd);
    touch = lv_indev_create();
    if (!touch) { display_poweroff(); return false; }
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch, lcd);
    lv_indev_set_read_cb(touch, touch_read_cb);
    swallow_touch = false;
    /* Backlight is enabled by port_screen, after the application has started. */
    return diag.active;
}
void display_task(uint32_t now_ms)
{
    if (!diag.active) { service_time_valid = false; return; }
#ifdef CANFD_REVE_TARGET
    diag.touch_present = touch_ft6336_service(now_ms);
#endif
    if (service_time_valid) {
        uint32_t gap = (uint32_t)(now_ms - service_last_ms);
        /* Report milliseconds in microsecond units; sub-ms gaps quantize to 0. */
        uint32_t us = gap > UINT32_MAX / 1000u ? UINT32_MAX : gap * 1000u;
        if (us > diag.max_service_gap_us) diag.max_service_gap_us = us;
    }
    service_time_valid = true; service_last_ms = now_ms;
    if (redraw_requested && power_get()->screen_on) {
        redraw_requested = false;
        lv_obj_invalidate(lv_screen_active());
        diag.forced_redraws++;
    }
    display_dma_poll(now_ms);
    if (!diag.active) return;
    uint32_t render_started = HAL_GetTick();
    diag.ui_snapshot_errors = ui_lvgl_snapshot_errors();
    if (power_get()->screen_on) ui_lvgl_task(now_ms);
    (void)lv_timer_handler();
    uint32_t render_ms = (uint32_t)(HAL_GetTick() - render_started);
    if (render_ms > diag.max_render_ms) diag.max_render_ms = render_ms;
    /* One short command phase or stable pixel DMA is launched per iteration. */
    if (diag.active && dma_phase == DMA_IDLE) display_dma_poll(now_ms);
}
#ifdef REVD_FREERTOS
void display_request_redraw(void) { redraw_requested = true; }
void port_screen_redraw(void) { revd_ui_request_redraw(); }
#else
void port_screen_redraw(void) { redraw_requested = true; }
#endif
void display_set_backlight(uint8_t percent)
{
    requested_bl = percent > 100 ? 100 : percent;
    if (buses_up) __HAL_TIM_SET_COMPARE(&pwm, TIM_CHANNEL_1,
                                      diag.active ? requested_bl * 10u : 0u);
}
void display_poweroff(void)
{
    redraw_requested = false;
    diag.active = false;
    dma_quiesce();
    dirty_bands = 0;
    diag.dirty_bands = 0;
    runtime_started = false;
    requested_bl = 0;
    if (pwm.Instance) {
        __HAL_TIM_SET_COMPARE(&pwm, TIM_CHANNEL_1, 0);
        HAL_TIM_PWM_Stop(&pwm, TIM_CHANNEL_1);
        HAL_TIM_PWM_DeInit(&pwm);
    }
    if (spi.Instance) HAL_SPI_DeInit(&spi);
    buses_up = false;
    __HAL_RCC_SPI4_CLK_DISABLE(); __HAL_RCC_TIM1_CLK_DISABLE();
    touch_ft6336_deinit();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    /* High impedance, no pull: do not drive unpowered panel inputs. */
    GPIO_InitTypeDef g = {0};
    g.Pin = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
            GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOE, &g);
#ifdef CANFD_REVE_TARGET
    /* Only after SPI, PWM, CTP reset and I2C are released may QOD discharge. */
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_8, GPIO_PIN_RESET);
#endif
}
const display_diag_t *display_diagnostics(void) { return &diag; }
