/* FT6336U register protocol, implemented locally; no third-party driver copied.
 * Hardware: PB8/9 AF4 OPEN DRAIN and NO PULL (external module has 5 V pulls),
 * PD7 reset, PE1 interrupt. Do not use push-pull or enable internal pull-ups. */
#include "touch_ft6336.h"
#include "stm32h7xx_hal.h"

#define FT_ADDR (0x38u << 1)
#define FT_TIMEOUT_MS 3u
/* 100 MHz I2C123 kernel, analog filter on, digital filter 0. ~99 kHz:
 * PRESC=9 (100 ns), SCLDEL=4, SDADEL=2, SCLH=48, SCLL=49.
 * Recheck rise/fall and setup/hold with the actual 5 V module/panel cable. */
#define FT_I2C_TIMING 0x90423031u
#ifndef REVD_TOUCH_SWAP_XY
#define REVD_TOUCH_SWAP_XY 1
#endif
#ifndef REVD_TOUCH_MIRROR_X
#define REVD_TOUCH_MIRROR_X 0
#endif
#ifndef REVD_TOUCH_MIRROR_Y
#define REVD_TOUCH_MIRROR_Y 1
#endif

static I2C_HandleTypeDef i2c;
static bool ready;
static uint8_t chip_id;
static uint32_t errors;
#ifdef CANFD_REVE_TARGET
enum recovery_stage { TOUCH_IDLE, TOUCH_BACKOFF, TOUCH_RESET_LOW, TOUCH_BOOT_WAIT,
                      TOUCH_PROBE, TOUCH_ID };
static enum recovery_stage recovery;
static uint32_t recovery_due;
static uint8_t failure_streak;
static bool recovery_enabled, release_required;
static bool read_failure(void)
{
    errors++;
    release_required = true;
    if (++failure_streak >= 3u) {
        ready = false;
        recovery = TOUCH_BACKOFF;
        recovery_due = HAL_GetTick() + 250u;
        release_required = true;
    }
    return false;
}
#define READ_FAILURE() return read_failure()
#else
#define READ_FAILURE() do { errors++; return false; } while (0)
#endif

void touch_ft6336_deinit(void)
{
    ready = false;
#ifdef CANFD_REVE_TARGET
    recovery_enabled = false;
    recovery = TOUCH_IDLE;
#endif
    if (i2c.Instance) HAL_I2C_DeInit(&i2c);
    __HAL_RCC_I2C1_CLK_DISABLE();
    GPIO_InitTypeDef g = {0};
    g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_NOPULL;
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9; HAL_GPIO_Init(GPIOB, &g);
    g.Pin = GPIO_PIN_7; HAL_GPIO_Init(GPIOD, &g);
    g.Pin = GPIO_PIN_1; HAL_GPIO_Init(GPIOE, &g);
}

bool touch_ft6336_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_GPIOD_CLK_ENABLE(); __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_7, GPIO_PIN_RESET);
    g.Pin = GPIO_PIN_7; g.Mode = GPIO_MODE_OUTPUT_PP; g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW; HAL_GPIO_Init(GPIOD, &g);
    HAL_Delay(10);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_7, GPIO_PIN_SET);
    HAL_Delay(200);
    g.Pin = GPIO_PIN_1; g.Mode = GPIO_MODE_INPUT; HAL_GPIO_Init(GPIOE, &g);
    g.Pin = GPIO_PIN_8 | GPIO_PIN_9; g.Mode = GPIO_MODE_AF_OD;
    g.Speed = GPIO_SPEED_FREQ_LOW; g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);
    i2c.Instance = I2C1;
    i2c.Init.Timing = FT_I2C_TIMING;
    i2c.Init.OwnAddress1 = 0; i2c.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    i2c.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    i2c.Init.OwnAddress2 = 0; i2c.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    i2c.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    i2c.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    ready = HAL_I2C_Init(&i2c) == HAL_OK &&
            HAL_I2CEx_ConfigAnalogFilter(&i2c, I2C_ANALOGFILTER_ENABLE) == HAL_OK &&
            HAL_I2CEx_ConfigDigitalFilter(&i2c, 0) == HAL_OK &&
            HAL_I2C_IsDeviceReady(&i2c, FT_ADDR, 1, FT_TIMEOUT_MS) == HAL_OK &&
            HAL_I2C_Mem_Read(&i2c, FT_ADDR, 0xA3, I2C_MEMADD_SIZE_8BIT,
                             &chip_id, 1, FT_TIMEOUT_MS) == HAL_OK && chip_id == 0x64;
    if (!ready) { errors++; touch_ft6336_deinit(); }
#ifdef CANFD_REVE_TARGET
    recovery_enabled = true;
    failure_streak = 0;
    release_required = !ready;
    recovery = ready ? TOUCH_IDLE : TOUCH_BACKOFF;
    recovery_due = HAL_GetTick() + 2000u;
#endif
    return ready;
}

#ifdef CANFD_REVE_TARGET
static void recovery_failed(uint32_t now)
{
    errors++;
    touch_ft6336_deinit();
    recovery_enabled = true;
    recovery = TOUCH_BACKOFF;
    recovery_due = now + 2000u;
}
/* Called only by the foreground display task. Reset delays are deadlines, not
 * HAL_Delay: recording, CAN service and USB input policy keep progressing.
 * No LCD rail power cycle and no GPIO bit-banged clock on the 5 V pulled bus. */
bool touch_ft6336_service(uint32_t now)
{
    if (!recovery_enabled || ready) return ready;
    if ((int32_t)(now - recovery_due) < 0) return false;
    GPIO_InitTypeDef g = {0};
    switch (recovery) {
    case TOUCH_BACKOFF:
        touch_ft6336_deinit();
        recovery_enabled = true;
        HAL_GPIO_WritePin(GPIOD, GPIO_PIN_7, GPIO_PIN_RESET);
        g.Pin = GPIO_PIN_7; g.Mode = GPIO_MODE_OUTPUT_PP;
        g.Pull = GPIO_NOPULL; g.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOD, &g);
        recovery = TOUCH_RESET_LOW; recovery_due = now + 10u;
        break;
    case TOUCH_RESET_LOW:
        HAL_GPIO_WritePin(GPIOD, GPIO_PIN_7, GPIO_PIN_SET);
        recovery = TOUCH_BOOT_WAIT; recovery_due = now + 200u;
        break;
    case TOUCH_BOOT_WAIT:
        __HAL_RCC_I2C1_CLK_ENABLE();
        g.Pin = GPIO_PIN_1; g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOE, &g);
        g.Pin = GPIO_PIN_8 | GPIO_PIN_9; g.Mode = GPIO_MODE_AF_OD;
        g.Speed = GPIO_SPEED_FREQ_LOW; g.Alternate = GPIO_AF4_I2C1;
        HAL_GPIO_Init(GPIOB, &g);
        /* Instance and timing were set by the startup initialization. */
        if (HAL_I2C_Init(&i2c) != HAL_OK ||
            HAL_I2CEx_ConfigAnalogFilter(&i2c, I2C_ANALOGFILTER_ENABLE) != HAL_OK ||
            HAL_I2CEx_ConfigDigitalFilter(&i2c, 0) != HAL_OK) {
            recovery_failed(now); break;
        }
        recovery = TOUCH_PROBE;
        break;
    case TOUCH_PROBE:
        if (__HAL_I2C_GET_FLAG(&i2c, I2C_FLAG_BUSY) != RESET ||
            HAL_I2C_IsDeviceReady(&i2c, FT_ADDR, 1, FT_TIMEOUT_MS) != HAL_OK) {
            recovery_failed(now); break;
        }
        recovery = TOUCH_ID;
        break;
    case TOUCH_ID:
        if (__HAL_I2C_GET_FLAG(&i2c, I2C_FLAG_BUSY) != RESET ||
            HAL_I2C_Mem_Read(&i2c, FT_ADDR, 0xA3, I2C_MEMADD_SIZE_8BIT,
                             &chip_id, 1, FT_TIMEOUT_MS) != HAL_OK || chip_id != 0x64) {
            recovery_failed(now); break;
        }
        ready = true; failure_streak = 0; release_required = true;
        recovery = TOUCH_IDLE;
        break;
    case TOUCH_IDLE: break;
    }
    return ready;
}
#endif

bool touch_ft6336_read(uint16_t *x, uint16_t *y, bool *pressed)
{
    uint8_t p[5];
    *pressed = false;
    if (!ready) return false;
    /* ST HAL has an internal 25 ms initial BUSY wait independent of our
     * timeout argument. Reject an already-stuck bus before entering it. */
    if (__HAL_I2C_GET_FLAG(&i2c, I2C_FLAG_BUSY) != RESET) { READ_FAILURE(); }
    /* Polling register 0x02 also works on modules where INT stays asserted. */
    if (HAL_I2C_Mem_Read(&i2c, FT_ADDR, 0x02, I2C_MEMADD_SIZE_8BIT,
                         p, sizeof p, FT_TIMEOUT_MS) != HAL_OK) { READ_FAILURE(); }
    uint8_t count = p[0] & 0x0f;
    if (!count) {
#ifdef CANFD_REVE_TARGET
        failure_streak = 0; release_required = false;
#endif
        return true;
    }
    if (count > 2 || ((p[1] >> 6) & 3u) == 1u) return true;
    uint16_t rx = (uint16_t)(((p[1] & 15u) << 8) | p[2]);
    uint16_t ry = (uint16_t)(((p[3] & 15u) << 8) | p[4]);
    if (rx >= 320 || ry >= 480) { READ_FAILURE(); }
#if REVD_TOUCH_SWAP_XY
    *x = ry; *y = rx;
#else
    *x = rx; *y = ry;
#endif
#if REVD_TOUCH_MIRROR_X
    *x = (uint16_t)(479u - *x);
#endif
#if REVD_TOUCH_MIRROR_Y
    *y = (uint16_t)(319u - *y);
#endif
    if (*x >= 480 || *y >= 320) { READ_FAILURE(); }
#ifdef CANFD_REVE_TARGET
    failure_streak = 0;
    if (release_required) return true;
#endif
    *pressed = true;
    return true;
}
uint8_t touch_ft6336_chip_id(void) { return chip_id; }
uint32_t touch_ft6336_errors(void) { return errors; }
