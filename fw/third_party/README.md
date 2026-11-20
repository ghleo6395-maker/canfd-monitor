# 开源依赖与复用台账

2026-10-27 裸机交付更新：继续复用已锁定依赖；新增使用LVGL buttonmatrix、FatFs UTF-8长文件名/ffunicode以及同源发送方案校验；中文固定字库覆盖GB2312常用汉字。正常构建使用已生成字体C文件，无运行时字体下载。FreeRTOS对照保留历史，本次裸机不链接内核。

核对日期：2026-10-24。实际依赖现在已放入 `fw/vendor/` 并通过本板 ARM 编译。精确版本、commit、许可见 `dependencies.lock.json`；FatFs 补丁及校验见 `patches/FatFs_R0.16_manifest.json`。编译与主机模型测试已完成，物理板验证尚未完成。下表保留实际引入与仅参考的边界。

| 模块 | 当前状态 | 引用点/许可 | 实际范围和后续工作 |
|---|---|---|---|
| can-utils candump codec | **已实际复用（BSD 选项）** | [`834ceef1f1eabec67625389172eebf1050d2f42b`](https://github.com/linux-can/can-utils/tree/834ceef1f1eabec67625389172eebf1050d2f42b)，原文件双许可 BSD-3-Clause OR GPL-2.0-only，选 BSD-3-Clause | `services/logfmt/candump.c/.h` 适配十六进制、ID、Classic/RTR/FD 布局和 DLC；新增有界时间戳/通道解析。完整上游 notice 在该 `.c` 文件及 `services/logfmt/LICENSE.can-utils`。仅支持 candump `-L` 子集，不含 CAN XL/len8_dlc，不含 Linux/socket 驱动 |
| STM32 H7 HAL | **已导入并链接固件** | [v1.11.6 / a1996eed](https://github.com/STMicroelectronics/stm32h7xx-hal-driver/tree/a1996eed9172b59887bafaaa0ea1816ea14d48b5)，BSD-3-Clause | 电源/时钟/GPIO/FDCAN/SDMMC IDMA/SPI DMA/I2C/ADC/TIM，保留原许可；USB 不混用 HAL PCD。SDMMC LL 协作等待本地补丁及 SHA 见 patches/STM32H7_SDMMC_cooperative_manifest.json |
| CMSIS Device / Core | **已导入并链接固件** | ST H7 v1.10.7 / ARM CMSIS 5.9.0，Apache-2.0 | 寄存器、系统初始化与向量模板；本地启动文件在 C 运行时前增加 PE2 保持，保留 ST notice |
| SuperCAN | **仅借鉴，未引入源码/协议实现** | [`10ec81bcbd06f440b6e907e2bd822bdc07ba9c44`](https://github.com/jgressmann/supercan/tree/10ec81bcbd06f440b6e907e2bd822bdc07ba9c44)，[MIT](https://github.com/jgressmann/supercan/blob/10ec81bcbd06f440b6e907e2bd822bdc07ba9c44/LICENSE) | 参考 USB CAN FD 消息/队列、主机接口；上游 NUCLEO-H7A3ZI-Q 不是本机 H743VIT6。不可整包烧录；后续若采用线协议必须双路适配并实测兼容性 |
| FreeRTOS-Kernel | **已导入；仅FreeRTOS B目标链接** | [V11.3.2 / `8a59a4b1`](https://github.com/FreeRTOS/FreeRTOS-Kernel/tree/8a59a4b19bf72977819feddef87d41163ced7004)，[MIT](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/8a59a4b19bf72977819feddef87d41163ced7004/LICENSE.md) | 官方2026-10-16稳定发布；GCC Cortex-M7 r0p1 port，静态4任务/队列，固定1ms tick；保留裸机A目标，缓冲与cache策略相同。并发所有权、IRQ阈值、任务栈与A/B测量见平台rtos/README.md；编译及模型验证不代替实板 |
| TinyUSB | **已导入并链接固件** | [0.21.0 / `dae3f9a3`](https://github.com/hathach/tinyusb/tree/dae3f9a366bfcddbf9dcf1b48d7500286a849539)，MIT | CDC ACM + MSC BOT/SCSI + H743 DWC2 FS；本地驱动负责无 VBUS、USB2 SOF、前台命令与 SD 独占，见平台 USB.md；主机模型通过，实机枚举待测 |
| LVGL | **已导入并链接固件** | [v9.5.0 / `85aa60d1`](https://github.com/lvgl/lvgl/tree/85aa60d18b3d5e5588d7b247abf90198f07c8a63)，MIT/组件随附许可 | 直接复用内置 ST7796 与 MIPI 驱动、组件、字体工具；新增 SPI4 DMA/CPU shadow、FT6336U、共享 UI 状态机适配和 OFL 简体字库；实际 LVGL host 渲染通过，实板待测 |
| FatFs | **已导入并链接固件** | [R0.16 mirror / `30ca13c6`](https://github.com/abbrev/fatfs/tree/30ca13c62615df0d2e9104ab41256985b96590c1) + [官方 patch1/patch2](https://elm-chan.org/fsw/ff/patches.html)，ChaN 单条件许可 | SDMMC1 diskio、流式 candump 读写、MSC 卷所有权；真实文件系统 RAM 卡测试通过，实际卡待测；当前 FAT12/16/32、UTF-8长文件名（静态工作区），不支持 exFAT |
| inih | **待移植，未导入** | [`2bbdec4a366c8c39746ee0982e7ca0febbb044b6`](https://github.com/benhoyt/inih/tree/2bbdec4a366c8c39746ee0982e7ca0febbb044b6)，[BSD-3-Clause](https://github.com/benhoyt/inih/blob/2bbdec4a366c8c39746ee0982e7ca0febbb044b6/LICENSE.txt) | 仅作 INI 语法层；参数范围/事务提交/原子写回由本项目负责。暂不为尚未使用的配置路径引入解析器 |

早期研究阶段检查的 FatFs 官方 `ff16.zip` 原始包：SHA-256 `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`。该值不覆盖补丁；本次实际导入、补丁顺序和最终 ff.c SHA256 以 patches/FatFs_R0.16_manifest.json 为准。

MIT/BSD 许可允许复用，不免除保留上游版权/许可的义务。以后拷入文件时逐文件核对所属许可，不把仓库顶层许可直接套到第三方子目录。主机端的 GPL 工具可作为独立测试程序；本次没有把 SavvyCAN、can-utils Linux CLI 或内核驱动链接进固件。

ESP32-C5 Logger、CANBadger 本次仅作产品交互/架构资料参考，尚未复制其代码；不预先承诺其代码许可适合导入。SavvyCAN 目前只作为日志互通/后续上位机候选，尚未实现 GVRET 或 SuperCAN 设备协议，也未验证其双路 CAN FD 实机连接。
