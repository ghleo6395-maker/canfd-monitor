# Rev D 双路 CAN FD 接收路径

复用当前 ST HAL v1.11.6 的 FDCAN 接收/发送接口，未更换 CAN 栈。内核时钟仍为 80 MHz，两路 message RAM 分别为 0/1248 words；初始化另一通道不复位、关闭共享 FDCAN 时钟或覆盖另一通道的 RAM。

RX FIFO0 每路 48 个 64-byte 元素，blocking 模式。`FDCAN1/2_IT0_IRQHandler` 调用 ST HAL IRQ handler，RX callback 每次最多取 48 帧，填入该通道的 512 帧 SPSC 队列。即使 FatFs 正在等待卡，CAN IRQ 仍可接收。ISR 仅读 TIM2 低 32 位，记录帧及计数，不调用应用事件队列、广播 ring、FatFs 或 LVGL。32-bit C11 acquire/release 原子为编译期确认的 lock-free 操作。

前台 `board_can_task()` 先处理真实 TX event，向应用报告完成，然后将两路 RX 和暂存的 TX 回显按接收/取出时间合并，每轮最多交付 128 帧。TX 回显并不提前写入广播 ring，因此不会把积压 RX 的较早时间戳放在 TX 之后，导致 candump 时间倒退。停止或重新初始化单路时保留已入队 RX，仍由统一合并器交付。RX 时间是中断读取时间，TX 时间是前台取出完成事件的时间，未声称硬件线上的 SOF 精确时间。

目标构建将两路 CAN 状态、RX 队列和 64 帧 TX 回显缓冲置于 32-byte 对齐的 `.revd_d2_bss`，合计 **88,832 bytes**。根启动文件必须在 C 运行时前打开对应 D2 SRAM 时钟并清零此段。普通主机构建不使用 MCU 内存段。CAN IRQ 优先级 4，高于 SDMMC 和显示 DMA/USB；TIM2 不被屏幕 TIM1 复位影响。

`board_can_metrics()` 提供每通道队列当前深度/峰值、ISR 队列丢帧、广播 ring 丢帧、TX 回显缓冲丢帧、HAL 读取错误和硬件 lost 通知次数。前台通过 `EV_CAN_DROPS` 汇总，事件队列满时保留未交付增量重试：

| b | 含义 | 精度 |
|---|---|---|
| 1 | 硬件 RF0L lost 通知 | 下界；多个硬件丢帧可合并到一个锁存位 |
| 2 | 512 帧 ISR 队列满，丢弃新帧 | 精确帧数 |
| 3 | 应用广播 ring 满 | 精确帧数 |
| 4 | 前台 TX 回显缓冲满 | 精确帧数；真实 TX 完成事件仍正常报告 |

缓存具有明确上限。若每通道接收速率为 f 帧/秒，单通道 512 帧队列的名义缓存时间为 512/f 秒；这不是已测得的板级性能。主循环在 SD 的数据 DMA 和命令等待期间继续消费，减少仅靠此队列承受卡长尾的情况。持续吞吐超过卡的实际写入能力时仍会溢出，并如实计数。

`make test-can` 使用真实 `can_fdcan.c`/SPSC 实现和 48-entry HAL FIFO 模型，覆盖双路积压、队列溢出、RF0L 合并、广播满、TIM2/队列索引回绕、单路重启、TX 入队失败、事件满重试、中断预算、TX/RX 合并时间顺序，另有 100,000 帧生产/消费线程并发。ASan/UBSan 和 ThreadSanitizer 通过。还需实板双路发生器、SD 长尾和屏幕刷新并发压测；这些主机模型不能证明物理板零丢帧。

ST LL 命令响应的 cooperative 适配见 `third_party/patches/STM32H7_SDMMC_cooperative_manifest.json`：线程等待每 256 轮调用板级 pump，并按 HAL tick 计入 pump 耗时；SDMMC ISR 不调用 pump，使用 TIM2 将命令响应等待限制为 1 ms。原 SD 响应解析/CRC/错误代码保留。CMD12 当前采用短响应，此限制未替代线程上下文中的卡内部编程状态等待。实卡兼容性仍须验证。
