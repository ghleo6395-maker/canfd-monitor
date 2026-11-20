# Rev D 屏幕、触摸与 LVGL 适配

固件调用真实 SPI4、I2C1 和 TIM1 外设；UI 使用相同的 `ui_json` 视图模型及 `app_touch` / `app_key_raw` 输入入口。主页八个入口、所有列表页、实时帧、报文草稿/数字位编辑、筛选和确认窗口均由既有状态机控制。物理键和屏幕底部方向/OK 软键沿用相同短按、长按、连发、锁定及发送前确认保护。列表每屏六行，随物理键/软键光标分页；点击发送条目进入草稿，不直接发送。

## 接口与供电顺序

- `bool display_init(void)`：调用前 PE3 共享 5 V 必须已稳定。复位触摸、读取 FT6336U `0xA3 == 0x64`，配置 SPI4/TIM1，调用 LVGL 原版 ST7796 控制器初始化并创建真实交互控件。没有触摸模块或初始化失败返回 `false`，主循环可继续无屏运行。
- `void display_task(uint32_t now_ms)`：只在主循环串行调用。每 100 ms 更新视图；`lv_timer_handler` 同时处理触摸。LVGL tick 使用 `HAL_GetTick`，没有 ISR 操作 LVGL。
- `void display_dma_poll(uint32_t now_ms)`：只推进 DMA 标志、超时和下一个命令/像素阶段，不进入 LVGL、触摸或应用事件；可供 SD IDMA 等待的前台 I/O pump 调用。
- `void display_set_backlight(uint8_t percent)`：PE9/TIM1_CH1，200 MHz 定时器输入分频到 1 MHz、PWM 1 kHz，0–100%。熄屏只关闭背光，不关闭共享 5 V；息屏时暂停视图更新但继续处理唤醒触摸。
- `void display_poweroff(void)`：关共享 5 V 前取消显示 DMA、清理旧中断并停止 SPI4/I2C1/TIM1，并把外部屏/触摸信号设置为输入无上拉。保留 LVGL 对象，避免在触摸回调里销毁正在分发事件的对象；下次 init 时回收。此函数不拉低 PE3。
- `const display_diag_t *display_diagnostics(void)`：触摸探测结果、SPI 错误、视图快照失败、影子图更新、DMA 条带/事务/超时/取消计数及待刷新条带位图。`panel_id_verified` 始终为 false：触摸芯片 ACK 只能识别模块存在，不能证明 LCD 正常显示。没有伪造 SPI 屏幕 ID 探测。

## 硬件配置

ST7796S：SPI4 PE12/13/14 AF5，100 MHz 内核 /8 = 12.5 MHz，mode 0、8 位。CS PE11、DC PE10、RST PE15；480×320 横屏，RGB565 高字节先发。复用 `vendor/lvgl/src/drivers/display/st7796` 的初始化及通用 MIPI 控制器配置；运行期使用公开 `lv_display_set_flush_cb` 接口将渲染结果复制到影子图，再由前台状态机通过 SPI DMA 发送。

触摸 FT6336U：I2C1 PB8/PB9 **AF4 开漏且 NOPULL**；外接模块有 5 V 上拉，不开启 MCU 内部上拉。I2C1 输入为 100 MHz，时序值 `0x90423031`，模拟滤波开启、数字滤波为 0，目标约 100 kHz。RST 使用实际 Rev D 的 PD7，INT 为 PE1。寄存器读取自写，小端 MCU 上解出原始 320×480 坐标，默认交换 XY 后镜像 Y，映射横屏 480×320。`REVD_TOUCH_SWAP_XY / MIRROR_X / MIRROR_Y` 可通过编译选项调整。面板安装方向和具体排线需要四角实测；软件单元测试验证的是当前约定的坐标变换。

触摸 HAL 数据传输超时为 3 ms；进入读事务前先检查 BUSY，已卡住的总线直接失败，避免 ST HAL 独立的 25 ms 初始 BUSY 等待。故障返回释放态，不阻塞重试。息屏后的第一次触摸会被完整吞掉直到松手，避免唤醒时误按仍显示的发送/格式化确认框。按下后界面或行映射改变时，释放不会激活新的条目。

## 内存、性能和当前边界

- LVGL 固定内存池 96 KiB；部分绘图缓冲与稳定 DMA 缓冲各 19,200 字节，命令存储 32 字节，位于 DMA1 可访问的 D2 SRAM。307,200 字节影子图位于 AXI SRAM。
- 视图快照上限 32 KiB、2,560 个 8 字节 token。溢出时保留现有页面并显示错误，统计诊断计数，不解析截断内容。
- 当前运行期为 **DMA 非阻塞刷新**，保留 D-cache 关闭。LVGL 复制到影子图后立即释放绘图缓冲；SPI4 DMA1 Stream0 每轮推进一个阶段，地址命令和 20 行像素都异步发送，ISR 只通过 HAL 设置完成/错误标志。发送中的条带拥有独立稳定快照，新画面再次标脏，轮询条带避免饥饿；不会等待屏幕约 197 ms 的全帧总线时间。每阶段 100 ms deadline，错误时关闭屏幕并安全取消。CPU 渲染仍消耗计算时间，双路实际吞吐和丢帧必须上板统计。内存所有权、关机/错误流程及详细测试见 [DISPLAY_DMA.md](DISPLAY_DMA.md)。
- LCD 默认命令序列来自 LVGL，初始化有数百 ms 的复位/休眠退出延时；只发生在屏幕上电初始化。
- 无屏模式是基于“未找到指定触摸模块”的保守选择。触摸正常但 LCD 背光/数据线损坏仍须人工台架诊断。

## 中文字库复用与许可证

LVGL 9.5 内置 Source Han 1,338 常用字对本项目简体中文视图缺 173 个字，不能直接使用。`ui_font_revd_16.c` 是从 LVGL 已附的 Adobe `SourceHanSansSC-Normal.otf` 通过官方 `lv_font_conv 1.5.3` 生成的 16 px、4 bpp、未压缩子集，覆盖当前 UI 和服务文字的全部 392 个非 ASCII 字及可打印 ASCII。生成字库采用新名称 `ui_font_revd_16`，保留 [SIL OFL 1.1](fonts/LICENSE.SourceHanSansSC.txt) 许可。应用代码及 LVGL 使用各自许可，字体许可不扩展到应用代码。

固件编译使用已生成的 C，无需 npm 或系统字体。修改界面文字后运行：

```sh
python3 platform/stm32h743_revd/tools/generate_ui_font.py
```

新增任意中文文件名不保证处于这个静态字库子集，未包含字形会显示占位符；记录文件默认名称使用 ASCII。

## 验证

在仓库 `fw/` 中运行：

```sh
python3 platform/stm32h743_revd/tests/run_ui_host.py /absolute/path/to/scratch/previews
python3 tests/display_dma/run_test.py
```

测试实际编译并执行 LVGL、UI 适配与现有应用/模拟板：检查中文标题及字形覆盖、八个主页入口、实时行、发送编辑/数字位、软键原始按键事件、确认/取消、按下后改页防误触、内存池完整性和快照上限，输出 RGB565 软件渲染的 PPM 预览。另用 HAL double 执行实际 FT6336 驱动，检查地址、时序、OD/NOPULL、四角变换、无触点/抬起/畸形坐标、I/O 失败和掉电前安全引脚。

这些测试与 ARM 编译验证尚不等于物理屏/触摸已经验证。上板需要确认：SPI 时钟、供电时序、LCD RGB/BGR 和方向、四角触摸、3 ms 超时和热插故障、所有方向键/长按、熄屏唤醒不误确认、录制时屏幕刷新的实际丢帧统计。

## Rev E 触摸运行中故障恢复（2026-10-31）

仅`CANFD_REVE_TARGET`启用；Rev D的旧行为和r2复编哈希保留。任意读失败立即取消LVGL当前按压，不把故障当作松手点击；恢复后的持续按住也不会点击，必须先收到无触点帧，再接受新按压。连续三次失败进入恢复，短暂恢复成功会清除失败计数。

前台`touch_ft6336_service()`按250ms等待、10ms复位低电平、200ms启动等待推进，每轮最多进行一次有3ms超时的I2C事务。等待使用时间戳，不调用运行时`HAL_Delay(210)`。重试前检查BUSY；失败后退避2秒；错误芯片ID不放行。它只复位触摸控制器，不切断屏幕供电，也不在模块的5V上拉总线上模拟推挽时钟。关机deinit会取消后续恢复并释放引脚。

`display_task()`持续更新在线状态，现有离线提示也能反映运行中故障；BACK/REC/POWER仍可操作。未实现用三键完整替代触摸键盘/所有编辑页面。

验证：`make test-reve`包含实际触摸驱动的84项故障/计时/释放保护检查；`make test-reve-display`运行实际display.c和真实LVGL，验证通信失败不会产生按钮CLICKED，恢复后正常点击有效，以及在线提示状态随故障更新。均为HAL模型，不是实际屏幕热插测试。
