/* Rev D board data only. This header neither initializes GPIO nor provides HAL.
 * Checked against hw/pinmap.md, README.md section 15 and
 * docs/software/research/01_硬件约束.md on 2026-10-24.
 * Port numbers are symbolic IDs, NOT STM32 register bases or HAL GPIO masks.
 */
#ifndef CANFD_STM32H743_REVD_BOARD_CONTRACT_H
#define CANFD_STM32H743_REVD_BOARD_CONTRACT_H

#include <stdint.h>

#define REVD_MCU_PART "STM32H743VIT6"
#define REVD_BOARD_REVISION "D"

enum revd_gpio_port {
    REVD_PORT_A, REVD_PORT_B, REVD_PORT_C, REVD_PORT_D,
    REVD_PORT_E, REVD_PORT_F, REVD_PORT_G, REVD_PORT_H
};
enum { REVD_AF_GPIO = 255, REVD_AF_ANALOG = 254 };
enum { REVD_ACTIVE_LOW = 0, REVD_ACTIVE_HIGH = 1, REVD_ACTIVE_NA = 255 };
/* External device availability, NOT GPIO voltage. All MCU GPIO use 3.3 V logic.
 * TOUCH5V has module-side 5 V I2C pull-ups on the 5 V tolerant PB8/PB9 pins.
 */
enum revd_power_dependency { REVD_CORE, REVD_MAIN5V, REVD_TOUCH5V };

/* name, port, GPIO bit, LQFP100 lead, alternate function, active level, power */
#define REVD_PIN_CONTRACT(X) \
    X(PWR_KEY,    A,  0, 22, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(UART1_TX,   A,  9, 68, 7,              REVD_ACTIVE_NA,   REVD_CORE) \
    X(UART1_RX,   A, 10, 69, 7,              REVD_ACTIVE_NA,   REVD_CORE) \
    X(USB_DM,     A, 11, 70, 10,             REVD_ACTIVE_NA,   REVD_CORE) \
    X(USB_DP,     A, 12, 71, 10,             REVD_ACTIVE_NA,   REVD_CORE) \
    X(EXP_SCL,    B,  6, 92, 6,              REVD_ACTIVE_NA,   REVD_CORE) \
    X(EXP_SDA,    B,  7, 93, 6,              REVD_ACTIVE_NA,   REVD_CORE) \
    X(CTP_SCL,    B,  8, 95, 4,              REVD_ACTIVE_NA,   REVD_TOUCH5V) \
    X(CTP_SDA,    B,  9, 96, 4,              REVD_ACTIVE_NA,   REVD_TOUCH5V) \
    X(UART3_TX,   B, 10, 46, 7,              REVD_ACTIVE_NA,   REVD_CORE) \
    X(UART3_RX,   B, 11, 47, 7,              REVD_ACTIVE_NA,   REVD_CORE) \
    X(CAN2_RX,    B, 12, 51, 9,              REVD_ACTIVE_NA,   REVD_MAIN5V) \
    X(CAN2_TX,    B, 13, 52, 9,              REVD_ACTIVE_NA,   REVD_MAIN5V) \
    X(CAN2_STB,   B, 14, 53, REVD_AF_GPIO,   REVD_ACTIVE_HIGH, REVD_MAIN5V) \
    X(VBAT_ADC,   C,  0, 15, REVD_AF_ANALOG, REVD_ACTIVE_NA,   REVD_CORE) \
    X(CC1_SENSE,  C,  4, 32, REVD_AF_ANALOG, REVD_ACTIVE_NA,   REVD_CORE) \
    X(CC2_SENSE,  C,  5, 33, REVD_AF_ANALOG, REVD_ACTIVE_NA,   REVD_CORE) \
    X(LED_REC,    C,  6, 63, REVD_AF_GPIO,   REVD_ACTIVE_HIGH, REVD_CORE) \
    X(SD_D0,      C,  8, 65, 12,             REVD_ACTIVE_NA,   REVD_CORE) \
    X(SD_D1,      C,  9, 66, 12,             REVD_ACTIVE_NA,   REVD_CORE) \
    X(SD_D2,      C, 10, 78, 12,             REVD_ACTIVE_NA,   REVD_CORE) \
    X(SD_D3,      C, 11, 79, 12,             REVD_ACTIVE_NA,   REVD_CORE) \
    X(SD_CLK,     C, 12, 80, 12,             REVD_ACTIVE_NA,   REVD_CORE) \
    X(CAN1_RX,    D,  0, 81, 9,              REVD_ACTIVE_NA,   REVD_MAIN5V) \
    X(CAN1_TX,    D,  1, 82, 9,              REVD_ACTIVE_NA,   REVD_MAIN5V) \
    X(SD_CMD,     D,  2, 83, 12,             REVD_ACTIVE_NA,   REVD_CORE) \
    X(CAN1_STB,   D,  3, 84, REVD_AF_GPIO,   REVD_ACTIVE_HIGH, REVD_MAIN5V) \
    X(SD_DET,     D,  4, 85, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(CTP_RST,    D,  7, 88, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_TOUCH5V) \
    X(KEY_UP,     D,  8, 55, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(KEY_DOWN,   D,  9, 56, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(KEY_LEFT,   D, 10, 57, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(KEY_RIGHT,  D, 11, 58, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(KEY_OK,     D, 12, 59, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(KEY_BACK,   D, 13, 60, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(KEY_REC,    D, 14, 61, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(LED_STAT,   D, 15, 62, REVD_AF_GPIO,   REVD_ACTIVE_HIGH, REVD_CORE) \
    X(CTP_INT,    E,  1, 98, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_TOUCH5V) \
    X(PWR_HOLD,   E,  2,  1, REVD_AF_GPIO,   REVD_ACTIVE_HIGH, REVD_CORE) \
    X(MAIN5V_EN,  E,  3,  2, REVD_AF_GPIO,   REVD_ACTIVE_HIGH, REVD_CORE) \
    X(CHG_HI,     E,  4,  3, REVD_AF_GPIO,   REVD_ACTIVE_HIGH, REVD_CORE) \
    X(CHG_STAT,   E,  5,  4, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(PGOOD_STAT, E,  6,  5, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_CORE) \
    X(LCD_BL,     E,  9, 39, 1,              REVD_ACTIVE_HIGH, REVD_MAIN5V) \
    X(LCD_DC,     E, 10, 40, REVD_AF_GPIO,   REVD_ACTIVE_HIGH, REVD_MAIN5V) \
    X(LCD_CS,     E, 11, 41, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_MAIN5V) \
    X(LCD_SCK,    E, 12, 42, 5,              REVD_ACTIVE_NA,   REVD_MAIN5V) \
    X(LCD_MISO,   E, 13, 43, 5,              REVD_ACTIVE_NA,   REVD_MAIN5V) \
    X(LCD_MOSI,   E, 14, 44, 5,              REVD_ACTIVE_NA,   REVD_MAIN5V) \
    X(LCD_RST,    E, 15, 45, REVD_AF_GPIO,   REVD_ACTIVE_LOW,  REVD_MAIN5V)

#define REVD_DECLARE_PIN(name, port, pin, lead, af, active, power) \
    enum { REVD_##name##_PORT = REVD_PORT_##port, REVD_##name##_PIN = pin, \
           REVD_##name##_LEAD = lead, REVD_##name##_AF = af, \
           REVD_##name##_ACTIVE = active, REVD_##name##_POWER = power };
REVD_PIN_CONTRACT(REVD_DECLARE_PIN)
#undef REVD_DECLARE_PIN

typedef struct {
    const char *name;
    uint8_t port, pin, package_lead, af, active_level, power_dependency;
} revd_pin_contract_t;
#define REVD_PIN_ROW(name, port, pin, lead, af, active, power) \
    { #name, REVD_PORT_##port, pin, lead, af, active, power },
static const revd_pin_contract_t revd_pin_contract[] = {
    REVD_PIN_CONTRACT(REVD_PIN_ROW)
};
#undef REVD_PIN_ROW

enum {
    REVD_MCU_FLASH_KIB = 2048,
    REVD_HSE_HZ = 25000000,
    REVD_LSE_HZ = 32768,
    REVD_FDCAN_KERNEL_HZ_PROPOSAL = 80000000,
    REVD_CAN_CHANNELS = 2,
    REVD_CAN1_CONTROLLER = 1,
    REVD_CAN2_CONTROLLER = 2,
    REVD_FDCAN_SHARED_RAM_WORDS = 2560,
    /* Proposed allocation only: std32 + ext16*2 + fifo0 48*18 +
     * fifo1 8*18 + tx8*18 + tx_event16*2 = 1248 words per controller. */
    REVD_CAN_RAM_WORDS_PER_CHANNEL_PROPOSAL = 1248,
    REVD_CAN1_RAM_OFFSET_WORDS_PROPOSAL = 0,
    REVD_CAN2_RAM_OFFSET_WORDS_PROPOSAL = 1248,
    REVD_CAN_DATA_BITRATE_MAX = 5000000,
    REVD_LCD_SPI_INSTANCE = 4,
    REVD_LCD_WIDTH_LANDSCAPE = 480,
    REVD_LCD_HEIGHT_LANDSCAPE = 320,
    REVD_TOUCH_I2C_INSTANCE = 1,
    REVD_TOUCH_I2C_START_HZ = 100000,
    REVD_SDMMC_INSTANCE = 1,
    REVD_SDMMC_BUS_WIDTH = 4,
    REVD_USB_FS_RAW_BITRATE = 12000000,
    REVD_USB_VBUS_SENSE_WIRED = 0,
    REVD_USB_EXTERNAL_HS_PHY_PRESENT = 0,
    REVD_TOUCH_INTERNAL_PULL_ALLOWED = 0,
    REVD_SOFTWARE_TERMINATION_CONTROL = 0,
    REVD_SOFTWARE_TERMINATION_SENSE = 0,
    REVD_SD_POWER_SWITCH_PRESENT = 0,
    REVD_MAIN5V_SHARED_BY_LCD_AND_CAN = 1,
    REVD_PGOOD_DISTINGUISHES_USB_FROM_12V = 0,
    REVD_STANDBY_RETAINS_POWER_HOLD = 0
};

#if defined(__cplusplus) && __cplusplus >= 201103L
#define REVD_STATIC_ASSERT(c, message) static_assert(c, message)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define REVD_STATIC_ASSERT(c, message) _Static_assert(c, message)
#else
#error "board_contract.h requires C11 or C++11 static assertions"
#endif
REVD_STATIC_ASSERT(REVD_CTP_RST_PORT == REVD_PORT_D && REVD_CTP_RST_PIN == 7 && REVD_CTP_RST_LEAD == 88,
                   "Rev D touch reset is PD7/88, not PE0/97");
REVD_STATIC_ASSERT(REVD_PWR_HOLD_PORT == REVD_PORT_E && REVD_PWR_HOLD_PIN == 2,
                   "early power hold must use PE2");
REVD_STATIC_ASSERT(REVD_MAIN5V_EN_PORT == REVD_PORT_E && REVD_MAIN5V_EN_PIN == 3,
                   "shared 5 V enable must use PE3");
REVD_STATIC_ASSERT(REVD_CAN2_RAM_OFFSET_WORDS_PROPOSAL >=
                   REVD_CAN1_RAM_OFFSET_WORDS_PROPOSAL + REVD_CAN_RAM_WORDS_PER_CHANNEL_PROPOSAL,
                   "proposed FDCAN RAM regions overlap");
REVD_STATIC_ASSERT(REVD_CAN2_RAM_OFFSET_WORDS_PROPOSAL + REVD_CAN_RAM_WORDS_PER_CHANNEL_PROPOSAL <=
                   REVD_FDCAN_SHARED_RAM_WORDS, "proposed FDCAN RAM allocation exceeds shared RAM");
REVD_STATIC_ASSERT(REVD_USB_VBUS_SENSE_WIRED == 0 && REVD_TOUCH_INTERNAL_PULL_ALLOWED == 0,
                   "Rev D USB/touch power-domain constraints changed");
#undef REVD_STATIC_ASSERT

#endif
