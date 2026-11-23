/* SPDX-License-Identifier: MIT */
/*
 * Hardware-C 第 12 页第 3 版的硬件适配：把 winsim_hal_t 映射到两片 TCA9539 + PG2 + PC2_C。
 *
 *   U801 TCA9539 @ 0x74（A0 = A1 = GND），I2C4：PD12 = SCL，PD13 = SDA
 *     P00–P07 → 电阻阵 b0–b7          （输出寄存器 0x02）
 *     P10–P13 → 电阻阵 b8–b11         （输出寄存器 0x03 bit0–3）
 *     P14     → 0 Ω 直通 Q807         （0x03 bit4）
 *     P15     → 电阻阵 b12（100 Ω）   （0x03 bit5）
 *     P16/P17 → 通道地址 CH_A0/CH_A1  （0x03 bit6–7，K1–K4 = 0–3）
 *   U808 TCA9539 @ 0x75（A0 = +3V3），同一条 I2C4
 *     P00/P01 → AD5292 SPI 的 SCLK/DIN（位操作，SCLK 下降沿锁存）
 *     P02–P05 → U809–U812 的 SYNC（低有效，R837–R840 上拉）
 *     P06     → VPOT_EN（U817 升压，高有效，R843 下拉）
 *     P07     → 电阻阵 b13（47 Ω）
 *     P10–P13 → 空闲继电器 U813–U816 的 LED 阴极（低 = 接通）
 *     P14     → 电阻阵 b14（150 kΩ）；P15–P17 不用，保持输入
 *   RESET   ← PG2（WS_RST_N），R814 下拉；两片同时复位：全部引脚变输入，MOS 由下拉关断，
 *             SYNC 被拉高，继电器 LED 没有电流，升压关断；PG2 同时经 Q808B 反相成 CH_EN_N，
 *             低时 4 路通道继电器全断、回读开关关闭；AD5292 的 RESET 也接 WS_RST_N
 *   ADC     ← PC2_C（WS_ADC），所选通道 R815–R818 之一 / R819 = 300k/56k，折算系数 6.357，量程约 21 V
 *
 * set_oe(false) 拉低 PG2；set_oe(true) 释放复位后，每片先写输出寄存器再设为输出方向。
 * 芯片复位后输出寄存器默认 0xFF，顺序反了会让全部 MOS 瞬间导通。
 * 任何一次 I2C 写失败都会立刻拉低 PG2，回到全部关断。
 * AD5292 每次写码值前先写一次控制寄存器（命令 6，C1 = 1 允许改 RDAC），复位后也不需要另外处理。
 *
 * winsim_hal_t 没有用户指针，所以本适配是单实例的。
 */
#ifndef WIN_SIM_TCA9539_H
#define WIN_SIM_TCA9539_H

#include <stdbool.h>
#include <stdint.h>

#include "services/winsim/win_sim.h"

#define WINSIM_TCA9539_ADDR    0x74u   /* U801 */
#define WINSIM_TCA9539_ADDR_B  0x75u   /* U808 */
#define WINSIM_TCA9539_REG_OUT 0x02u   /* Output Port 0；0x03 = Port 1 */
#define WINSIM_TCA9539_REG_CFG 0x06u   /* Configuration 0；0x07 = Port 1，0 = 输出 */
#define WINSIM_TCA9539_P1_SHORT 0x10u  /* U801 P14 */
#define WINSIM_TCA9539_P1_B12   0x20u  /* U801 P15 */
#define WINSIM_TCA9539_P1_CH_SHIFT 6u  /* U801 P16/P17 */
#define WINSIM_TCA9539_P1_CH_MASK  0xC0u
/* U808 */
#define WINSIM_TCA9539_B0_SCLK  0x01u
#define WINSIM_TCA9539_B0_DIN   0x02u
#define WINSIM_TCA9539_B0_SYNC(ch) ((uint8_t)(0x04u << (ch)))
#define WINSIM_TCA9539_B0_SYNC_ALL 0x3Cu
#define WINSIM_TCA9539_B0_VPOT  0x40u
#define WINSIM_TCA9539_B0_B13   0x80u
#define WINSIM_TCA9539_B1_IDLE_N(ch) ((uint8_t)(0x01u << (ch)))
#define WINSIM_TCA9539_B1_IDLE_ALL 0x0Fu
#define WINSIM_TCA9539_B1_B14   0x10u
#define WINSIM_TCA9539_B1_CFG   0xE0u  /* P15–P17 保持输入 */
/* AD5292 命令字 */
#define WINSIM_AD5292_CTRL_WRITE_EN 0x1802u   /* 命令 6：C1 = 1 */
#define WINSIM_AD5292_RDAC(code) ((uint16_t)(0x0400u | ((code) & 0x03FFu)))

typedef struct {
    /* 从 reg 开始连续写 len 字节（芯片自动递增），收到 ACK 返回 true。 */
    bool     (*i2c_write)(uint8_t addr7, uint8_t reg, const uint8_t *data, uint8_t len);
    void     (*reset_pin)(bool high);   /* PG2：false = 拉低（复位），true = 释放 */
    uint16_t (*adc_read_raw)(void);     /* PC2_C 原始码值 */
    void     (*delay_ms)(uint32_t ms);
    uint8_t  addr7;                     /* U801 */
    uint8_t  addr7_b;                   /* U808 */
    uint16_t adc_full_scale;            /* 16 位 ADC 为 65535 */
    uint16_t adc_vref_mv;               /* VREF+ = +3V3A */
    float    divider_ratio;             /* (R815 + R819) / R819 */
} winsim_tca9539_port_t;

/* 地址、ADC 满量程、参考电压和分压比的默认值；四个回调需要调用方填写。 */
winsim_tca9539_port_t winsim_tca9539_default_port(void);

/* 绑定平台接口并输出 HAL（含空闲保持的三个回调）。绑定时立即拉低 PG2。 */
bool winsim_tca9539_bind(const winsim_tca9539_port_t *port, winsim_hal_t *hal_out);

/* 在 HardFault、看门狗预警等场合直接调用：拉低 PG2，全部开关关断。 */
void winsim_tca9539_emergency_off(void);

/* 累计 I2C 写失败次数。 */
uint32_t winsim_tca9539_error_count(void);

#endif /* WIN_SIM_TCA9539_H */
