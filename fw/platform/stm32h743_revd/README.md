# STM32H743VIT6 Rev D 固件

本目录现为真实板级实现，能够生成 `build/revd/canfd-revd.{elf,bin,hex,map}`。当前通过 ARM 编译、主机驱动模型与共享状态机测试；尚未连接 Rev D 实板验证。

当前交付版本 `revd-bare-ui-20261027-r1`，按用户决定仅开发裸机。最新操作、真实 SD 方案 / 停录命名、64 字节显示、验证与烧录说明见[裸机交付与上板验证](../../../docs/software/16_裸机交付与上板验证.md)。FreeRTOS 小节为此前对照版本记录。

从 `fw/` 运行：

```sh
make firmware
make -f Makefile.arm check
make test test-web
```

需要 GNU Make、Python3 和 `arm-none-eabi-gcc/objcopy/size/readelf/nm/objdump`；已把所需上游源代码放在 `vendor/`。不要求 CubeMX、CMake 或联网下载。测试过 Arm GNU Toolchain 15.2.Rel1，Cortex-M7/FPv5-D16/hard float，GNU11。

## 实现范围

- `startup_stm32h743xx.s`：基于 ST 官方启动文件，Reset_Handler 先开 GPIOE、读回时钟、PE2 置高后设输出，再进入 SystemInit 与 C 初始化。
- `board.c/main.c`：25 MHz HSE，CPU 400 MHz，HCLK 200 MHz，APB 100 MHz；PLL1Q 给 FDCAN 80 MHz，PLL2 给 SDMMC/ADC 100 MHz，HSI48 + USB2 FS SOF CRS 校准。全部服务在主循环运行；USB IRQ 仅调用 TinyUSB。D-cache 关闭，I-cache 开启。
- `can_fdcan.c`：两路实际 FDCAN，默认只听、分离 Message RAM 0/1248 字偏移；IRQ 收入每路 512 帧 SPSC，前台统一合并 RX/TX 回显、无 ACK 超时和分类丢帧通知。接收时间为 IRQ 读取时的 TIM2 微秒值，尚非总线边沿硬件时间戳。自动识别每档观察 180 ms。未达到双路满载验证标准。
- `storage_sd.c/diskio_sd.c`：HAL SDMMC1 4 位 IDMA + FatFs R0.16（官方 patch1/patch2），真实 candump 分段、读回、保护、删旧、发送方案和整次会话命名。FAT12/16/32、UTF-8 长文件名；不支持 exFAT、CFR/ASC/CSV。录制只入 1024 帧 DTCM 队列，FatFs 等待协作处理采集与已启动发送，完成/切段/关闭可靠通知；不会自动格式化。新文件路径见交付说明，旧驱动设计见 [存储驱动说明](../../../docs/storage-driver.md)。
- `usb_device.c/usb_descriptors.c`：TinyUSB 0.21.0 CDC + MSC；电脑请求危险动作仍须设备确认；MSC 独占 SD，退出同步后重新挂载。FS 12 Mbit/s，未实现 GVRET/SuperCAN 协议。开发 VID/PID 仅用于本地测试。详情见 `USB.md`。
- `display.c/touch_ft6336.c/ui_lvgl.c`：LVGL 9.5.0 内置 ST7796 驱动，SPI4 12.5 MHz DMA1 Stream0，480×320 RGB565 CPU 影子图加稳定条带 DMA 缓冲，FT6336U 100 kHz，PB8/PB9 开漏无内部上拉，复位 PD7。同一 UI 状态机的页面/菜单/对话框，专用简体中文字库。详情见 `DISPLAY.md`。

## 电源约束

PE3 同时供 LCD 和两路收发器；息屏只关背光，不关 PE3。关机先结束录制/文件关闭、停 CAN 与屏触摸总线，后关 5 V，等 POWER 松开再释放 PE2；无看门狗与 Standby。SD 使用常开 3.3 V，不能断电恢复。

PE4 CHG_HI 固定低档，PGOOD 只作为外部电源状态。尚未完成 CC 电流分档、枚举前电流控制与无电池 USB 启动功率预算。电池 ADC 使用 ADC3 内部参考及出厂 VREF 校准，再按 PC0 的 1:2 分压换算；仍须实测校准电阻/参考误差。若 ADC 异常，本版不会用虚构低电值自动关机。

正常冷启动须按住 POWER 至少 1 秒。MCU 复位但 POWER 已松开时，软件按普通启动恢复。BACK+REC 开机组合键的“升级模式”仍是原服务占位状态，尚未实现 SD IAP/应用内 DFU；请使用下面的 SWD 烧录路径。

## 首次烧录

镜像链接在 **0x08000000**，2 MiB Flash，初始栈为 0x20020000。用 ST-Link 接 SWDIO/PA13、SWCLK/PA14、NRST、GND 和目标电压参考。空白芯片和 ROM bootloader 不会运行 PE2 保持代码，首次烧录期间须持续按住 POWER，并确保电源稳定。

STM32CubeProgrammer GUI 选择 ST-LINK、Connect under reset，下载 HEX 并 Verify；HEX 自带地址。BIN 必须指定 0x08000000。CLI 可运行：

```sh
make -f Makefile.arm flash CUBEPROGRAMMER_CLI=/absolute/path/STM32_Programmer_CLI
```

该目标会先验证镜像，然后 SWD under reset 下载、校验、复位；本次工作只执行了设备列表读取，没有执行烧录。当前未检测到 ST-Link/J-Link/DFU 设备。

## 必须上板验证

先接电源/SD/屏幕和台架 CAN，检查 PE2 冷启动与释放时序、5 V 电源、USB 枚举/拔线/休眠、触摸方向与 LCD 色序、SD 插拔/长忙/掉电后的文件状态、两路实际波特率和 FD 数据段。确认后再测吞吐和长时间录制。屏幕 wire-time 不再占住 CPU；每个 LVGL partial block 都有采集 yield。FatFs 保留同步 API，但 SD 数据/命令等待会协作服务采集、USB transport 和屏幕 DMA；并非整套 FatFs 已变成抢占式异步文件系统。有限队列仍不能保证任意 SD 长尾或双路满载无丢帧。

发送表通过 `.canplan.json` 明确保存 / 加载持久化；名称前缀配置也保存到真实卡。RTC 对时、独立系统配置自动持久化、二进制 CFR/ASC、SD IAP、DMA/cache 性能调优、USB 主机兼容性和电源预算仍需后续工作或实板验证。代码中 `board_contract.h` 的 `_PROPOSAL` 名称保留兼容性，其 FDCAN 80 MHz 和 RAM 划分现已用于本固件。

## DMA 版运行与诊断

新流水线、内存分配和压测标准见 [DMA 与采集流水线](../../../docs/software/06_DMA与采集流水线.md)。

`make test test-web test-display test-display-dma` 验证共享服务、真实生产路径的 HAL 模型和 LVGL。`make -f Makefile.arm check` 还校验 DMA/CAN IRQ 向量、DMA staging 不在 DTCM、各 SRAM 段界限。`STATUS` 的 `performance` 返回 CAN 队列峰值与分类丢失、SD 队列/拒收/编码/DMA错误、LCD DMA进度。`frames/accepted/encoded` 均不表示每帧已通过掉电持久性验证。

D-cache 继续关闭。D2 SRAM1/2/3 在 C 运行时前开时钟并清零；DTCM 的 80 KiB 帧队列只由 CPU 编码，所有 SDMMC IDMA 实际传输经 AXI bounce。DMA1 Stream0/SPI4 IRQ 与 USB 设 7，SDMMC1 设 5，CAN IRQ 设 4。ST LL 本地适配补丁保留原许可与 SHA256 清单。


## FreeRTOS B 对照目标

裸机 A 保持 `make -f Makefile.arm check`；B 使用 `make -f Makefile.freertos check`，输出独立的 `build/revd-freertos/canfd-revd-freertos.{elf,bin,hex,map}`。
对应烧录分别执行同一 Makefile 的 `flash` 目标并明确指定 CubeProgrammer CLI，均不执行整片擦除。
B 的所有权/静态栈/中断与运行边界见 [rtos/README.md](rtos/README.md)，上板 A/B 见 [对照验收](../../../docs/software/09_FreeRTOS对照版本与验收.md)。
TIM7 IRQ6 在 B 专用于 CAN IRQ4 的内核通知转发，未启用 TIM7 外设定时器；不得另行复用这个 IRQ。
