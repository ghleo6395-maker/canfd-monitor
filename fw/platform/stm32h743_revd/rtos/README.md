# Rev D FreeRTOS B 目标

裸机 A 目标继续使用 `make -f Makefile.arm check`；B 使用 `make -f Makefile.freertos check`。两者输出分别位于 `build/revd/` 和 `build/revd-freertos/`，不会共用 object 或覆盖对方 HEX。B 使用官方稳定 FreeRTOS-Kernel **V11.3.2**，commit `8a59a4b19bf72977819feddef87d41163ced7004`；发布来源、下载 SHA、选定 port 与 MIT 许可记录在 `third_party/dependencies.lock.json`。内核源代码在 `vendor/freertos/`，未修改上游源文件。

## 运行模型

| 所有者 | 任务优先级 | 静态栈 | 内存域 | 单次工作 |
| --- | ---: | ---: | --- | --- |
| core | 5 | 8 KiB | DTCM | 核心桥、CAN 队列、状态机、请求分派 |
| storage | 4 | 12 KiB | DTCM | 存储所有者 step，串行 FatFs/SDMMC 与最多一个 RPC |
| usb | 3 | 6 KiB | DTCM | TinyUSB 与串行传输桥 |
| ui | 2 | 16 KiB | AXI SRAM | LVGL/触摸/显示桥 |
| Idle | 0 | 1 KiB | AXI SRAM | 内核静态 Idle |

MSP 中断栈仍单独保留 16 KiB。DTCM 的 SD 1024 帧队列及所有 DMA staging 容量未扩大。四个 TCB、Idle TCB、TLS/newlib 状态和桥队列另占静态 SRAM，以最终 map 为准。内核动态分配关闭；TinyUSB 0.21.0 的 FreeRTOS OSAL 直接选择静态 queue/semaphore/mutex 实现，无需额外 `CFG_TUSB_OS_STATIC_ALLOC` 宏。

任务平时通过通知等待，超时 1 tick 兜底；连续通知最多运行四次 step 或一个 tick 后，强制延迟一个 tick，避免高优先级持续就绪饿死 UI/USB。存储在 DMA pending 中用 `revd_rtos_wait_io()` 等待通知，卡状态先检查 ready；初次短忙预算 200 µs 轮询，仍忙才延迟 1 tick。等待只阻塞当前存储任务，不从其栈里调用采集、LVGL 或 USB。通知 index 0 用于运行时/DMA，index 1 留给存储 RPC 完成，不能交叉消费。

短 SDMMC 命令响应使用独立 `revd_rtos_poll_command_wait()`：先在连续约 200 µs 轮询预算内 yield，超预算才阻塞一个 tick，防止给每个正常快速命令硬加 1 ms。这个旧 LL hook 没有单条命令 begin/end 参数，因此预算按每任务的连续调用窗口计算，200 µs 以上空档或主动睡眠会清窗口；它不是逐命令硬实时保证。SD card programming busy 与 DMA completion 各自使用驱动里的独立等待策略。此差异应在 A/B 报告中保留，不能只比较 scheduler 名称。

初始化顺序：先完成时钟/GPIO，调用桥 bootstrap 创建静态通信对象，再创建静态任务并启动 scheduler。core task 执行产品 boot；storage task 建立存储所有权。UI/USB bridge 只在各自初始化命令发布后工作。核心状态、文件系统、USB/LVGL 通过独立所有者通信，不把原广播环直接暴露给多个任务读写。

## 中断、时钟与启动

保留 ST 启动文件和早期 PE2 保持。M7 `r0p1` port 的 SVC/PendSV 通过配置别名直接接入原向量表。SysTick 仍每毫秒 `HAL_IncTick()`，scheduler 开始运行后再转发 `xPortSysTickHandler()`，保证 scheduler 前 HAL 初始化延时可用。

FreeRTOS system-call 阈值为 NVIC 5。CAN 仍为 4，ISR 只写原 SPSC，再软件 pend TIM7；TIM7 设置为 6，在其中调用 `vTaskNotifyGiveFromISR` 唤醒 core。TIM7 没有启用定时器硬件，作为专用 deferred IRQ 使用；不得把它复用于其他 HAL 定时器。SDMMC 为 5、USB/LCD 为 7，可以使用各自 FromISR 通知。`configASSERT` 和 port 的向量/优先级校验保持开启。

TIM2 继续 1 MHz。`board_time_us()` 的 64 位低字回绕扩展用极短 PRIMASK 临界区保护，允许多个任务调用；CAN ISR 仍直接读取 TIM2 低 32 位。D-cache 继续关闭，I-cache 开启；RTOS 不改变 SD IDMA/SPI DMA 内存域或 coherency 要求。

## 诊断与限制

`revd_rtos_task_metrics()` 提供 step 次数、最大 step 壁钟微秒、静态栈大小和 FreeRTOS 栈高水位折算后的最小空闲字节。最大 step 时间包含被更高优先级抢占与当前任务阻塞时间，不能当作 CPU 执行时间。栈高水位是运行观测，必须覆盖错误路径和长时间压力，不代表形式上的栈安全证明。

每任务启用 newlib 重入状态。newlib allocator 的全局锁用 scheduler suspension 串行化，IRQ 不受该锁屏蔽；禁止 ISR 使用 allocator。内核/桥/驱动采用静态对象，LVGL 使用其固定池。诊断输出不要加入浮点格式化或运行时大对象分配。

`tools/verify_firmware.py` 对 B 额外检查 SVC/PendSV/SysTick/TIM7 向量、所有 bridge 的强符号、静态 task stack 所在区域、DMA staging 可访问域、无动态 kernel allocator 及 BIN/HEX 一致性。此检查确认镜像结构，并不证明调度、吞吐或上板正确。

同卡同载荷 A/B 需保留时钟、cache、队列容量和日志格式，先完成 MCU 复位，再分别保存 DIAG 与全部段文件。FreeRTOS 不提高卡的物理写速，也不会消除超过缓冲容量的卡长忙。只有端到端记录完整性相同、栈余量充分且服务间隔/队列峰值有可重复改善时，才能根据测量判断 B 更适合该硬件。
