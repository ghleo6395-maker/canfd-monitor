/* Rev D ST7796S + LVGL. All calls are main-loop only, never from an ISR. */
#ifndef REVD_DISPLAY_H
#define REVD_DISPLAY_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    bool touch_present;       /* FT6336 family ID 0x64 acknowledged on I2C1. */
    bool panel_id_verified;   /* false: SPI panel readback is not a reliable presence test. */
    uint32_t spi_errors;
    uint32_t ui_snapshot_errors;
    uint32_t shadow_flushes;
    uint32_t forced_redraws;  /* Explicit bench requests consumed by foreground LVGL. */
    uint32_t max_render_ms;
    uint32_t max_service_gap_us; /* Active display task entry gap, HAL ms resolution * 1000. */   /* Foreground UI snapshot + LVGL timer handler wall time; excludes DMA polling. */
    uint32_t coalesced_updates;
    uint32_t dma_transfers;
    uint32_t dma_bands;
    uint32_t dma_timeouts;
    uint32_t dma_cancelled;
    uint32_t dirty_bands;     /* Bit i: band i has a newer CPU shadow snapshot. */
    bool dma_busy;
} display_diag_t;

/* MAIN5V must already be stable. Rev D: missing touch selects headless mode.
 * Rev E: LCD may stay available and reports the missing touch module. */
bool display_init(void);
void display_task(uint32_t now_ms);
#ifdef REVD_FREERTOS
void display_request_redraw(void); /* UI owner only; core uses port_screen_redraw. */
#endif
/* Foreground I/O pump: no LVGL, touch, app events, or callbacks into UI.
 * Safe from the SDMMC cooperative wait as well as the normal main loop. */
void display_dma_poll(uint32_t now_ms);
/* Cancels pending display DMA, stops SPI/I2C/PWM and makes pins safe before MAIN5V drops.
 * LVGL objects are retained until next init to avoid deletion in a UI callback. */
void display_poweroff(void);
void display_set_backlight(uint8_t percent);
const display_diag_t *display_diagnostics(void);
#endif
