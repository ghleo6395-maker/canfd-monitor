/* SPDX-License-Identifier: MIT */
#include "board.h"
#include "display.h"
#include "power_reve.h"
#ifdef REVD_FREERTOS
#include "rtos/bridges.h"
#endif
#include "services/core/port.h"
#include "services/core/board_profile.h"
#include "services/core/app_evt.h"
#include "app/app.h"
#include "services/power/power_sm.h"
#include "services/sys/led.h"

static bool last_keys[KEY_NUM], keys_ready;
static uint32_t last_batt;
static ADC_HandleTypeDef adc1, adc3;
static bool adc_ok;
static bool main5v;
static uint8_t backlight;
static char boot_identity[17];
volatile uint32_t revd_fault_code;

const char *board_boot_id(void) { return boot_identity; }
uint64_t port_capture_time_us(void) { return board_time_us(); }

static void boot_identity_init(void)
{
    /* RNG is used only during boot, never in the recording path. Do not fall
     * back to the immutable UID: that would hide a reset from a bench report. */
    __HAL_RCC_RNG_CLK_ENABLE();
    RNG_HandleTypeDef rng = {.Instance = RNG, .Init.ClockErrorDetection = RNG_CED_ENABLE};
    uint32_t words[2];
    if (HAL_RNG_Init(&rng) == HAL_OK &&
        HAL_RNG_GenerateRandomNumber(&rng, &words[0]) == HAL_OK &&
        HAL_RNG_GenerateRandomNumber(&rng, &words[1]) == HAL_OK) {
        static const char hex[] = "0123456789abcdef";
        for (unsigned w = 0; w < 2; ++w)
            for (unsigned n = 0; n < 8; ++n)
                boot_identity[w * 8u + n] = hex[(words[w] >> (28u - 4u * n)) & 15u];
    }
    (void)HAL_RNG_DeInit(&rng);
    __HAL_RCC_RNG_CLK_DISABLE();
}

void board_fatal(uint32_t code)
{
    revd_fault_code = code;
    /* Keep power and SWD accessible on a clock/initialization error. */
    for (;;) { __NOP(); }
}

void board_clock_init(void)
{
    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}
    RCC_OscInitTypeDef o = {0};
    o.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI48;
    o.HSEState = RCC_HSE_ON;
    o.HSI48State = RCC_HSI48_ON;
    o.PLL.PLLState = RCC_PLL_ON;
    o.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM = 5; o.PLL.PLLN = 160; o.PLL.PLLP = 2;
    o.PLL.PLLQ = 10; o.PLL.PLLR = 2;
    o.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
    o.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    if (HAL_RCC_OscConfig(&o) != HAL_OK) board_fatal(1);
    RCC_ClkInitTypeDef c = {0};
    c.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
       RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1;
    c.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    c.SYSCLKDivider = RCC_SYSCLK_DIV1;
    c.AHBCLKDivider = RCC_HCLK_DIV2;
    c.APB3CLKDivider = RCC_APB3_DIV2;
    c.APB1CLKDivider = RCC_APB1_DIV2;
    c.APB2CLKDivider = RCC_APB2_DIV2;
    c.APB4CLKDivider = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_4) != HAL_OK) board_fatal(2);
    RCC_PeriphCLKInitTypeDef p = {0};
    p.PeriphClockSelection = RCC_PERIPHCLK_SDMMC | RCC_PERIPHCLK_FDCAN |
      RCC_PERIPHCLK_SPI4 | RCC_PERIPHCLK_I2C1 | RCC_PERIPHCLK_USB | RCC_PERIPHCLK_ADC | RCC_PERIPHCLK_RNG;
    p.PLL2.PLL2M = 5; p.PLL2.PLL2N = 40; p.PLL2.PLL2P = 2;
    p.PLL2.PLL2Q = 2; p.PLL2.PLL2R = 2;
    p.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_2; p.PLL2.PLL2VCOSEL = RCC_PLL2VCOMEDIUM;
    p.SdmmcClockSelection = RCC_SDMMCCLKSOURCE_PLL2;
    p.FdcanClockSelection = RCC_FDCANCLKSOURCE_PLL;
    p.Spi45ClockSelection = RCC_SPI45CLKSOURCE_D2PCLK1;
    p.I2c123ClockSelection = RCC_I2C123CLKSOURCE_D2PCLK1;
    p.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
    p.RngClockSelection = RCC_RNGCLKSOURCE_HSI48;
    p.AdcClockSelection = RCC_ADCCLKSOURCE_PLL2;
#ifdef CANFD_REVE_TARGET
    p.PeriphClockSelection |= RCC_PERIPHCLK_I2C4;
    p.I2c4ClockSelection = RCC_I2C4CLKSOURCE_D3PCLK1;
#endif
    if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK) board_fatal(3);
    HAL_PWREx_EnableUSBVoltageDetector();
    __HAL_RCC_CRS_CLK_ENABLE();
    /* USB SOF disciplines HSI48; retain internal trim before enumeration. */
    CRS->CFGR = (CRS->CFGR & ~(CRS_CFGR_SYNCSRC | CRS_CFGR_RELOAD | CRS_CFGR_FELIM)) |
      RCC_CRS_SYNC_SOURCE_USB2 | (47999U << CRS_CFGR_RELOAD_Pos) | (34U << CRS_CFGR_FELIM_Pos);
    CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;
    __HAL_RCC_TIM2_CLK_ENABLE();
    TIM2->PSC = 199; TIM2->ARR = 0xFFFFFFFFu;
    TIM2->EGR = TIM_EGR_UG; TIM2->CR1 = TIM_CR1_CEN;
    boot_identity_init();
}

uint64_t board_time_us(void)
{
    static uint32_t prev;
    static uint64_t high;
    /* Several RTOS owners use this clock. Keep the low-word wrap extension
     * atomic across task preemption; also works before scheduler startup.
     * CAN IRQ uses TIM2->CNT directly, never this 64-bit extension. */
    uint32_t mask=__get_PRIMASK();
    __disable_irq();
    uint32_t n = TIM2->CNT;
    if (n < prev) high += UINT64_C(1) << 32;
    prev = n;
    uint64_t value=high | n;
    __set_PRIMASK(mask);
    return value;
}

static void input(GPIO_TypeDef *g, uint32_t pins)
{
    GPIO_InitTypeDef p = {.Pin=pins,.Mode=GPIO_MODE_INPUT,.Pull=GPIO_PULLUP};
    HAL_GPIO_Init(g, &p);
}
void board_gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_GPIOD_CLK_ENABLE(); __HAL_RCC_GPIOE_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_2, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3|GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_3, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
    GPIO_InitTypeDef p = {.Mode=GPIO_MODE_OUTPUT_PP,.Pull=GPIO_NOPULL,.Speed=GPIO_SPEED_FREQ_LOW};
    p.Pin=GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4; HAL_GPIO_Init(GPIOE,&p);
    p.Pin=GPIO_PIN_3|GPIO_PIN_15; HAL_GPIO_Init(GPIOD,&p);
    p.Pin=GPIO_PIN_14; HAL_GPIO_Init(GPIOB,&p);
    p.Pin=GPIO_PIN_6; HAL_GPIO_Init(GPIOC,&p);
    input(GPIOA, GPIO_PIN_0);
#ifdef CANFD_REVE_TARGET
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11, GPIO_PIN_RESET);
    p.Pin=GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11; HAL_GPIO_Init(GPIOD,&p);
    input(GPIOD, GPIO_PIN_12);
    input(GPIOE, GPIO_PIN_0);
    input(GPIOD, GPIO_PIN_4|GPIO_PIN_13|GPIO_PIN_14);
#else
    input(GPIOD, GPIO_PIN_4|GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11|GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14);
#endif
    input(GPIOE, GPIO_PIN_5|GPIO_PIN_6);
    p.Pin=GPIO_PIN_0;
#ifdef CANFD_REVE_TARGET
    p.Pin|=GPIO_PIN_4|GPIO_PIN_5;
#endif
    p.Mode=GPIO_MODE_ANALOG; HAL_GPIO_Init(GPIOC,&p);
    __HAL_RCC_ADC12_CLK_ENABLE(); __HAL_RCC_ADC3_CLK_ENABLE();
    adc1.Instance=ADC1; adc3.Instance=ADC3;
    ADC_InitTypeDef a = {0};
    a.ClockPrescaler=ADC_CLOCK_ASYNC_DIV4; a.Resolution=ADC_RESOLUTION_16B;
    a.ScanConvMode=ADC_SCAN_DISABLE; a.EOCSelection=ADC_EOC_SINGLE_CONV;
    a.NbrOfConversion=1; a.ExternalTrigConv=ADC_SOFTWARE_START;
    a.ExternalTrigConvEdge=ADC_EXTERNALTRIGCONVEDGE_NONE;
    a.ConversionDataManagement=ADC_CONVERSIONDATA_DR; a.Overrun=ADC_OVR_DATA_OVERWRITTEN;
    adc1.Init=a; adc3.Init=a;
    adc_ok=HAL_ADC_Init(&adc1)==HAL_OK && HAL_ADC_Init(&adc3)==HAL_OK &&
      HAL_ADCEx_Calibration_Start(&adc1,ADC_CALIB_OFFSET_LINEARITY,ADC_SINGLE_ENDED)==HAL_OK &&
      HAL_ADCEx_Calibration_Start(&adc3,ADC_CALIB_OFFSET_LINEARITY,ADC_SINGLE_ENDED)==HAL_OK;
    ADC_ChannelConfTypeDef ch={.Channel=ADC_CHANNEL_10,.Rank=ADC_REGULAR_RANK_1,
      .SamplingTime=ADC_SAMPLETIME_810CYCLES_5,.SingleDiff=ADC_SINGLE_ENDED,.OffsetNumber=ADC_OFFSET_NONE};
    if(HAL_ADC_ConfigChannel(&adc1,&ch)!=HAL_OK) adc_ok=false;
    ch.Channel=ADC_CHANNEL_VREFINT;
    if(HAL_ADC_ConfigChannel(&adc3,&ch)!=HAL_OK) adc_ok=false;
#ifdef CANFD_REVE_TARGET
    reve_power_init();
#endif
}
#ifdef CANFD_REVE_TARGET
bool board_reve_adc_mv(uint32_t channel, uint16_t *mv)
{
    ADC_ChannelConfTypeDef ch={.Channel=channel,.Rank=ADC_REGULAR_RANK_1,
      .SamplingTime=ADC_SAMPLETIME_810CYCLES_5,.SingleDiff=ADC_SINGLE_ENDED,.OffsetNumber=ADC_OFFSET_NONE};
    if(!adc_ok || HAL_ADC_ConfigChannel(&adc1,&ch)!=HAL_OK) return false;
    bool ok=HAL_ADC_Start(&adc3)==HAL_OK && HAL_ADC_PollForConversion(&adc3,2)==HAL_OK;
    uint32_t ref=ok?HAL_ADC_GetValue(&adc3):0; HAL_ADC_Stop(&adc3);
    ok=ok && ref && HAL_ADC_Start(&adc1)==HAL_OK && HAL_ADC_PollForConversion(&adc1,2)==HAL_OK;
    uint32_t raw=ok?HAL_ADC_GetValue(&adc1):0; HAL_ADC_Stop(&adc1);
    if(ok) {
        uint32_t vdda=__HAL_ADC_CALC_VREFANALOG_VOLTAGE(ref,ADC_RESOLUTION_16B);
        ok=vdda>=3000u && vdda<=3600u;
        *mv=(uint16_t)__HAL_ADC_CALC_DATA_TO_VOLTAGE(vdda,raw,ADC_RESOLUTION_16B);
    }
    ch.Channel=ADC_CHANNEL_10;
    if(HAL_ADC_ConfigChannel(&adc1,&ch)!=HAL_OK) return false;
    return ok;
}
#endif
bool port_key_down(key_id_t k)
{
#ifdef CANFD_REVE_TARGET
    if(!board_has_physical_key(k)) return false;
#endif
    if(k==KEY_POWER) return HAL_GPIO_ReadPin(GPIOA,GPIO_PIN_0)==GPIO_PIN_RESET;
    if((unsigned)k>=KEY_POWER) return false;
    return HAL_GPIO_ReadPin(GPIOD,(uint16_t)(GPIO_PIN_8 << k))==GPIO_PIN_RESET;
}
void port_pwr_hold(bool on) { HAL_GPIO_WritePin(GPIOE,GPIO_PIN_2,on?GPIO_PIN_SET:GPIO_PIN_RESET); }
void port_5v(bool on)
{
    if(!on) { port_can_standby(0); port_can_standby(1);
#ifdef REVD_FREERTOS
      revd_ui_stop_sync();
#else
      display_poweroff();
#endif
    }
    main5v=on;
    HAL_GPIO_WritePin(GPIOE,GPIO_PIN_3,on?GPIO_PIN_SET:GPIO_PIN_RESET);
}
void port_backlight(uint8_t pct) { backlight=pct;
#ifdef REVD_FREERTOS
    revd_ui_backlight(pct);
#else
    display_set_backlight(pct);
#endif
}
/* PGOOD includes 12V input. Do not infer a Type-C current contract. */
void port_chg_hi(bool on) {
    (void)on;
#ifndef CANFD_REVE_TARGET
    HAL_GPIO_WritePin(GPIOE,GPIO_PIN_4,GPIO_PIN_RESET);
#endif
    /* Rev E input policy owns EN1/EN2; PGOOD alone never raises USB current. */
}
bool port_screen_present(void)
{
#ifdef REVD_FREERTOS
    bool ok=main5v && revd_ui_init_sync();
    revd_ui_backlight(backlight);
#else
    bool ok=main5v && display_init();
    display_set_backlight(backlight);
#endif
    return ok;
}
char port_chip_rev(void) { return HAL_GetREVID()==0x2003U?'V':'Y'; }
uint32_t port_serial(void) { return HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2(); }
bool port_rtc_valid(void) { return false; }
void port_rtc_mark_synced(void) { /* RTC write support is not provided in baseline. */ }
void board_io_task(void)
{
#ifdef CANFD_REVE_TARGET
    reve_power_task();
#endif
    for(int k=0;k<KEY_NUM;k++) {
        bool now=port_key_down((key_id_t)k);
        if(!keys_ready || now!=last_keys[k]) {last_keys[k]=now;app_key_raw((key_id_t)k,now);}
    }
    keys_ready=true;
    HAL_GPIO_WritePin(GPIOC,GPIO_PIN_6,led_rec_on()?GPIO_PIN_SET:GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOD,GPIO_PIN_15,led_stat_on()?GPIO_PIN_SET:GPIO_PIN_RESET);
    uint32_t now=HAL_GetTick();
    if(now-last_batt>=1000) {
        last_batt=now;
        if(adc_ok && HAL_ADC_Start(&adc3)==HAL_OK && HAL_ADC_PollForConversion(&adc3,2)==HAL_OK) {
            uint32_t ref=HAL_ADC_GetValue(&adc3);
            HAL_ADC_Stop(&adc3);
            if(ref && HAL_ADC_Start(&adc1)==HAL_OK && HAL_ADC_PollForConversion(&adc1,2)==HAL_OK) {
                uint32_t raw=HAL_ADC_GetValue(&adc1); HAL_ADC_Stop(&adc1);
                uint32_t vdda=__HAL_ADC_CALC_VREFANALOG_VOLTAGE(ref,ADC_RESOLUTION_16B);
                uint32_t mv=__HAL_ADC_CALC_DATA_TO_VOLTAGE(vdda,raw,ADC_RESOLUTION_16B)*2u;
#ifdef CANFD_REVE_TARGET
                if(mv>=1800 && mv<=5000) evt_post4(EV_BATT,0,SRC_SYS,(int32_t)mv,
#else
                if(mv>=2500 && mv<=5000) evt_post4(EV_BATT,0,SRC_SYS,(int32_t)mv,
#endif
                   HAL_GPIO_ReadPin(GPIOE,GPIO_PIN_6)==GPIO_PIN_RESET);
            }
        }
    }
}
