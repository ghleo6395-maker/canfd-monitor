# Rev D 双路 CAN FD 联合压测模型

本工具只访问进程内 16 MiB RAM 卡，不打开任何块设备、USB 设备或真实 SD 卡。它是生产代码的可重复回归，实板验证另见项目上板流程。

```sh
make -C fw test-stress
python3 fw/tests/stress/run_stress.py --report work/stress-report.json
python3 fw/tests/stress/run_stress.py --scenario dual-256-continuous-segments-lcd
# 已编译后的自定义模型；stdout 为一行 JSON：
fw/build/stress/pipeline_stress --name custom --fps 1000 --duration-ms 5000 --busy-ms 250 --segments 16 --rotate-frames 200
```

`--fps` 为**每个通道**每秒帧数，两路均为标准 ID 0x5A0/0x5A1、64 B CAN FD+BRS。payload 与 `tools/bench/protocol.py` 共用 RVDB 版本 1 格式：run_id、每通道 uint64 sequence、计划发送偏移纳秒、确定性数据及 CRC32/IEEE。固定跨语言样例见 `payload_fixture.json`，C 启动时验证该样例。模型额外把每一帧的确切切段 fence 留在 RAM，复读 FatFs 文件核对实际所属分段。

`run_stress.py` 全量编译并运行生产 `can_fdcan.c`、`can_ingress.c`、`frame_ring.c`、`storage_sd.c`、`diskio_sd.c`、FatFs、`display.c` 和 LVGL。使用一个真实 LVGL 测试 label 触发全屏重绘，完整产品 UI 不参与此模型。屏幕驱动直接包含在测试编译单元内，只为请求全屏重绘并观测异步 DMA 快照；其生产实现没有替换。SD 和屏幕 HAL 是硬件边界替身，依赖统一微秒时钟。

- `HAL_GetTick()` 纯读；显式推进 CPU、SDMMC wire、SPI wire 和卡 programming 事件。
- 默认前台循环成本 40 us，协作 poll 成本 50 us，真实编码器每帧另加 10 us，LVGL 每个 partial flush 另加 200 us。可用 `--loop-cpu-us`、`--encode-cpu-us`、`--lcd-cpu-us` 修改。
- SD 参数为 6.25 MB/s wire、30 us 启动及正常 100 us programming；一次指定长忙用 `--busy-ms`，在至少接受 100 帧后注入。`--busy-target write|sync|close` 选择实际 FatFs 操作。显示 SPI 使用 12.5 Mb/s 计算每次 DMA 的完成时间。
- `--rotate-frames` 每达到指定新增接受数提交下一切段 fence；`--segments` 限制段数。每段预分配参数为 64 KiB，提前手动切段用来压测关闭/目录/FAT 边界，不冒充容量自动切段服务。
- `--clock-start-ms 4294965000` 令 HAL uint32 毫秒时钟在运行中回绕。
- `--card-full 1` 只在 RAM FAT 卷创建填充文件，保留约 128 KiB，触发真实 FatFs 短写。
- CAN IRQ 当前在每次模拟到达时立即执行且没有 CPU 时间成本；未模拟实物 IRQ 抖动、AHB/AXI 仲裁、缓存、CAN 线路错误和 SD FTL。此模型不能给出 Rev D 物理最大吞吐承诺。

默认报告列出注入/接受/编码/盘端计数、精确拒收、已接受但未持久化帧、queue peak、各层丢帧、逐帧 payload/CRC/顺序/时间/段边界、DMA/卡忙超时、各操作耗时、屏幕 DMA 快照完整性和关闭延迟。`written_bytes` 是 API 已接受字节，不是断电持久化证明。

严格无损场景必须所有帧逐一核对，`zero_loss:true`；零 count 差但有重复/CRC/时间倒序也失败。故障场景必须报告 `zero_loss:false` 并观测匹配的队列溢出、卡忙超时或满卡结果，摘要写 `EXPECTED FAULT`，不是无损 PASS。卡满文本尾部截断单列 `truncated_tails`；有效完整帧仍核对 payload 和 CRC。2500 ms 超时检查同一 nonfatal 错误不能在 200 ms 内重复重报。

256 段场景还验证重新挂载后的文件列表只缓存 128 段、其余 128 段留在盘上并正确计数。未列入缓存的日志不会因索引复用而被保护或自动删除。
