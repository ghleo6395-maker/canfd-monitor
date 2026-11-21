# win_sim：车窗开关模拟器驱动

APM 售后测试扩展模块。监控器在 APM 的车窗开关输入上模拟按键电阻，同时用 CAN 读回 APM 识别出的开关状态，用来测出各档位的电压门限。驱动按 APM 车窗开关模拟规范（apm.md）实现，并针对 Hardware-C 新增的第 12 页电路做了调整。C99，不用动态内存，裸机和 RTOS 都能用。

**硬件状态：第 12 页还只是提案。** 它尚未加入仓库里 11 页的 `Hardware-C-schematic.pdf`，现有固件也还没有移植到 Hardware-C。因此本目录只提供与平台无关的驱动和主机测试，没有接入 `platform/` 和主循环。

## 第 12 页电路

| 功能 | 器件和引脚 |
| --- | --- |
| I/O 扩展 | U801 TCA9539PWR，地址 0x74（A0 = A1 = GND），挂在 I2C4（PD12 SCL / PD13 SDA），与 U111 TUSB320、J302 共用总线 |
| 复位 | PG2 = WS_RST_N，经 R814 10 kΩ 下拉。低电平时全部引脚为输入，MOS 由 100 kΩ 下拉关断 |
| 电阻阵 | R801–R812 = 200、390、820、1.6k、3.3k、6.2k、13k、24k、47k、91k、180k、360k Ω（1%），由 Q801–Q806 BSS138DW 开关；P00–P07 对应 bit0–7，P10–P13 对应 bit8–11 |
| 0 Ω 直通 | Q807 AO3400A，由 P14 控制；P15–P17 未用，设为输出低 |
| 保护 | F801 50 mA PTC，D801 SMF15CA |
| 回读 | PC2_C = WS_ADC，R815/R816 = 300k/100k（0.1%）分压 1:4，取在 PTC 外侧；C802 10 nF |
| 接口 | J801 KF2EDGR-3.81-2P |

`win_sim_tca9539.h` 的注释里有完整的位映射和上电顺序。

## 文件

| 文件 | 内容 |
| --- | --- |
| `win_sim.h` / `win_sim.c` | 核心驱动：查找表、控制字求解、中值回读、过压锁定、自动扫描 |
| `win_sim_tca9539.h` / `.c` | 第 12 页硬件适配：HAL 到 TCA9539 寄存器、PG2 复位、PC2_C 折算 |
| `../../examples/winsim/win_sim_can_example.c` | 集成示例：CAN 唤醒 → 探测空载电压 → 4800→0 mV 扫描 → 打印电压与开关状态对应表 |
| `../../tests/test_win_sim.c` | 主机单元测试，Mock HAL 按物理模型仿真 APM 输入和 TCA9539 寄存器 |

## 运行

从仓库根目录执行：

```sh
make -C fw test-winsim
make -C fw winsim-example
```

`make -C fw test` 也会运行 `test_win_sim`。`services/winsim/*.c` 由 `services/*/*.c` 通配编进主机测试、模拟器动态库和 ARM 固件。ARM 固件里目前没有调用方，`--gc-sections` 会把它们丢掉。示例依赖集成方实现的 CAN 回调，所以放在 `fw/examples/winsim/`，不放在 `services/` 下。`run_ui_host.py` 和 `generate_ui_font.py` 会递归扫描 `services/` 下的全部 `.c`。

## 与 apm.md 的差异

| apm.md | 本实现 | 原因 |
| --- | --- | --- |
| 8 位电阻阵，`uint8_t mask`，`r_lut[256]` | 12 位，`uint16_t mask`，`r_lut[4096]`（16 KB） | 8 位达不到规范要求的 25 mV 精度（24 个测试点有 11 个超差） |
| 74HCT595，`/OE` 上拉 | TCA9539，RESET 下拉到 PG2 | 主控只剩 PG2、PC2_C 两个空闲脚；HAL 名称不变，映射见 `win_sim_tca9539.h` |
| 回读 1:1，负载 200 kΩ | 1:4，负载 400 kΩ，取在 PTC 外侧 | 误接 12 V 时 ADC 脚不超过 3.3 V |
| `winsim_state_t.mask_595` | `mask`，另加 `target_mv` | 控制字不再来自 595；回调里需要知道目标值 |
| 扫描中按目标值校验 | 扫描中按理论值校验 | 0–220 mV 之间没有档位，按目标值校验会在 50/100/150 mV 误报 |
| — | `r_series_ohm`、`short_rds_on_ohm`、`r_bit_ohm[]` 可配置 | 填入实测值可以提高精度 |
| — | `winsim_clear_ovp()` | 规范要求的显式解锁接口 |

`set_resistance()` 的目标阻值是模拟的按键电阻，不含 400 kΩ 回读负载；`set_voltage()` 和 `set_ratio()` 的模型包含这个负载。

## 接入 Hardware-C 固件

以下内容要等 Hardware-C 板级移植完成后再做。

1. 在板级代码里实现 `winsim_tca9539_port_t` 的四个回调，再用 `winsim_tca9539_bind()` 得到 `winsim_hal_t`：
   ```c
   static bool ws_i2c_write(uint8_t addr7, uint8_t reg, const uint8_t *d, uint8_t n)
   {
       return HAL_I2C_Mem_Write(&hi2c4, (uint16_t)(addr7 << 1), reg, I2C_MEMADD_SIZE_8BIT,
                                (uint8_t *)d, n, 5) == HAL_OK;
   }
   static void ws_reset_pin(bool high)
   {
       HAL_GPIO_WritePin(GPIOG, GPIO_PIN_2, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
   }
   ```
   `adc_read_raw` 读 ADC3 上 PC2_C 对应的通道，通道号要按 ST 数据手册核对。
2. PG2 配置为推挽输出，初始低电平。PC2_C 保持模拟输入，SYSCFG 模拟开关保持默认的断开状态即可。
3. 在 `HardFault_Handler` 和致命错误处理里先调用 `winsim_tca9539_emergency_off()`。
4. I2C4 上还有 U111 TUSB320。两边都只能在前台循环里访问，不要在中断里调用本模块。
5. **阻塞时间：** 每次设置约等待 `settle_delay_ms`（10 ms）。从 4800 到 0 mV、步长 50 mV、每档保持 30 ms 的扫描约需 4 s。在裸机主循环里，`delay_ms` 回调必须在等待期间继续处理 CAN 采集、USB 和显示，做法和现有 SD 协作等待相同，否则会丢帧。FreeRTOS 版本可以放进独立任务。
6. `winsim_ctx_t` 约 16.5 KB，要放在静态区（AXI SRAM 或 DTCM），不要放在栈上。

## 已验证和未验证

- **已验证：** 主机上 13 组测试共 1241 项检查全部通过，在 ASan/UBSan 下运行。`make -C fw test lib` 的其余测试也全部通过。示例扫描还原出了仿真 APM 设定的 5 个门限。
- **精度（仿真）：** 0–4600 mV 每 200 mV 一个测试点，最差误差 20 mV。换成 20 组 1% 误差的电阻、导通电阻偏离模型 2 Ω 时，最差仍是 20 mV。
- **ARM 编译：** 用 Arm GNU Toolchain 15.2.Rel1 跑 `make -C fw -f Makefile.arm BOARD=reve check` 通过，两个文件都没有告警。目前没有调用方，链接后不留任何 winsim 符号，BIN 和 main 分支逐字节相同（SHA256 `e1178da6…`）。函数栈帧最大 160 B（`winsim_init`）。
- **未验证：** STM32 平台回调、实板 I2C 和 ADC、与 APM 的联调。
