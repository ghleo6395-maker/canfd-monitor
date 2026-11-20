# Rev D 双路 CAN FD 录制压测工具

入口：`python3 fw/tools/bench/revd_bench.py --help`。工具仅用 Python 3 标准库：
Linux SocketCAN、POSIX tty、CRC32 和 SQLite，不依赖 python-can/pyserial。
发送使用 Linux 内核 CAN FD ABI；日志兼容现有 can-utils 派生的 `candump -L` 编解码器。
SQLite 临时索引使逐帧对账的 RAM 开销不随帧数线性增长；磁盘空间仍随证据量增长。

`generate`、`report`、`selftest` 都离线运行。`send`、`monitor`、`control` 必须有
`--hardware` 和明确的接口/tty；不会扫描接口、配置波特率、提升权限、格式化或修改 SD 卡。
新 sender/monitor/control 文件以独占创建方式写入，避免覆盖已有证据。

## 离线检查

从工程根目录运行：

```sh
python3 -m unittest discover -s fw/tests/bench_tools -v
python3 fw/tools/bench/revd_bench.py selftest --out /tmp/revd-bench-selftest-001
```

`selftest` 生成一个正常用例和五个故障用例：单帧丢失、重复、CRC 损坏、相邻文件换序、尾帧丢失。
正常用例为 `PASS_SIMULATION`，五个故障都必须为 `FAIL`；这些结果不代表实板已经通过。
输出目录必须不存在或为空。C 联合流水线模型的 `fw/tests/stress/payload_fixture.json`
也由单元测试交叉校验，避免主机与模型误用不同 payload。

可单独生成任意有限长度的双路测试数据：

```sh
python3 fw/tools/bench/revd_bench.py generate --out /tmp/revd-fixture-001 \
  --duration 10 --fps 2000 2000 --run-id 0x5245564400000001 --segment-frames 1000
python3 fw/tools/bench/revd_bench.py report \
  --sender /tmp/revd-fixture-001/sender.jsonl --logs /tmp/revd-fixture-001/sd \
  --witness /tmp/revd-fixture-001/witness.log --witness-external \
  --monitor /tmp/revd-fixture-001/monitor.jsonl \
  --json /tmp/revd-fixture-001/report.json --csv /tmp/revd-fixture-001/anomalies.csv
```

## 台架连接与证据

两条独立总线各需要：发送器、Rev D 的对应通道、独立接收见证节点和正确的终端电阻。
发送器之外至少一个节点必须正常应答 ACK；Rev D 若处于 LISTEN 模式不会应答 ACK。
见证节点应接在总线上、能够接收 CAN FD；发送主机同一个 CAN 接口的本地回显不算独立见证。
接口名下列仅为示例，必须替换成已经配置好的专用台架接口。

每次测试更换 run_id，保存发送器/见证器型号、驱动、内核、配置的 nominal/data bitrate、
SD 卡型号/容量/空闲空间、固件构建标识、BENCH 配置、测试持续时间与接线说明。
工具的 `--fps CH0 CH1` 是每路计划帧率；它不估算或宣称总线利用率。
64 字节 BRS 的实际总线占用取决于波特率、位填充、仲裁、错误和适配器能力。

Linux 发送器只需两个显式指定的 SocketCAN CAN FD 接口；工具检查其 MTU 为 72。
请先另行确认接口已配置正确且位速率与 Rev D 一致。

## 典型测试顺序

下面命令都从工程根目录运行。先创建一个新的证据目录。每次命令使用不同的 JSONL 文件。
**同一 CDC tty 只运行一个进程**；monitor 运行期间请在板上操作确认、停止录制，避免控制进程抢走响应。
新 monitor 每次只允许一个未完成 STATUS 请求；超时会保存错误。
`roundtrip_ms` 从主机开始排队请求计至完整响应行，包含 USB、固件和主机调度，
不是 MCU 的 CPU 时间。报告提供 RTT 最大值/中位数/nearest-rank p99。

1. 在 Rev D 上确认 SD READY，配置两路 CAN FD 的位速率和模式，关闭周期发送。
   用 CDC 请求小段切分和整屏刷新。`64 KiB / 1000 ms / 100 ms` 为强切段压力示例：

   ```sh
   python3 fw/tools/bench/revd_bench.py control --hardware --port /dev/ttyACM0 \
     --command 'BENCH CONFIG 64 1000 100' --out bench/config.jsonl
   python3 fw/tools/bench/revd_bench.py control --hardware --port /dev/ttyACM0 \
     --command 'BENCH ON' --out bench/on.jsonl
   python3 fw/tools/bench/revd_bench.py control --hardware --port /dev/ttyACM0 \
     --command 'REC START' --out bench/start.jsonl
   ```

   `OK QUEUED` 只表示请求已排队；应在屏幕或 STATUS 中确认录制已启动。
   `BENCH ROTATE` 可显式请求额外切段。工具保留板上的停止/MSC 确认，不发送任何确认绕过命令。

2. 在独立接收器上，发送开始前启动 can-utils 原生记录，直到发送结束、尾帧到齐后再停止：

   ```sh
   candump -L witness0 witness1 > bench/witness.log
   ```

   若见证器位于另一台主机，复制其原始日志到本测试目录；保存见证器丢帧/驱动统计。
   本工具无法从日志内容证明其来源，`--witness-external` 是操作者对独立来源的明确声明。

3. 在发送器同一主机上先启动 CDC 采样，等待至少两次 STATUS 取得基线，再在另一终端发包：

   ```sh
   python3 fw/tools/bench/revd_bench.py monitor --hardware --port /dev/ttyACM0 \
     --duration 80 --interval 1 --out bench/monitor.jsonl
   ```

   ```sh
   python3 fw/tools/bench/revd_bench.py send --hardware --interfaces bench0 bench1 \
     --duration 60 --fps 2000 2000 --run-id 0x5245564400000001 --out bench/sender.jsonl
   ```

   `--extended` 选择扩展 ID。先从适配器能稳定提供的低帧率递增到高负载。
   两路可以给不同帧率，例如 `--fps 3000 1000`，用于检查公平性。
   sender 使用绝对单调时钟节拍；丢失主机节拍、队列满、短写、发送异常都记录。
   达到硬结束时间会停止追赶积压，不以长时间突发补发冒充原计划帧率。

4. 发送结束后，在板上停止录制并确认。保持 monitor 继续采样，直到 recorder=IDLE、
   CAN/SD 队列全为 0、写入/关闭完成，再让 monitor 正常结束。
   采样持续时间必须覆盖完整发送窗口及停止收尾；monitor 与 sender 需要同一主机时钟。
   若停止/收尾超过预留时间，保留现有证据，将该次结论视为不完整并重新安排更长窗口。

5. 完成录制和安全卸载后，复制本次 session 的**全部** `.LOG` 分段到主机。
   不在记录中读写卡，不把仅含最后一段的目录用于验收。然后对账：

   ```sh
   python3 fw/tools/bench/revd_bench.py report \
     --sender bench/sender.jsonl --logs bench/copied-card/S0000001 \
     --witness bench/witness.log --witness-external --witness-map witness0=0 witness1=1 \
     --monitor bench/monitor.jsonl --json bench/report.json --csv bench/anomalies.csv
   ```

   SD 固件日志 `can0=CH1`、`can1=CH2`。目录自动按路径中的数字自然排序，
   显式传入多个文件则保持给定顺序以便检查错误排序。`--channel-map` 可明确指定其它 SD 导出接口名。
   JSON 包括逐路期望/实际数、缺失尾帧、错误示例、计数器增量、队列高水位和耗时峰值。
   CSV 列出重复、已接受但缺失、已见证但缺失的序号。

## 判定边界

| 结果 | 含义 | 退出码 |
| --- | --- | --- |
| PASS | 完整双路 sender、独立接收日志、覆盖发送窗口的 CDC 数据相符，停止/队列排空，错误计数无新增 | 0 |
| PASS_SIMULATION | 上述条件仅由明确标记的合成 fixture 满足 | 0 |
| FAIL | 发现缺失、重复、坏 CRC/数据、错误通道/帧类型、段内/跨段逐通道回退、发送错误或错误计数增加 | 1 |
| INCONCLUSIVE | 缺独立证据、采样窗口不完整、主机节拍不达标、复位/重置/固件变化、尚未停止或未排空等 | 2 |
| 错误 | 参数/文件/传输异常；不能视为测试通过 | 3；中断为 130 |

完整性 `integrity_verdict` 与队列余量 `headroom.verdict` 单列。
最终 PASS 还要求 CAN1/CAN2/SD 的 peak≤容量75%，即至少25%空槽。
仅余量不足时总体 INCONCLUSIVE，不误报丢帧；缺容量/高水位字段也不能验收余量。

`send_accepted_ns` 仅表示 Linux `send()` 接受 72 字节 CAN FD 结构，不能证明已经上总线。
`socket_echo` 的 `MSG_CONFIRM` 表示本 socket 的本地回显；能否解释为发送完成取决于驱动是否支持硬件回显。
工具单独保留这类数据，默认不将它升级为独立接收证据。见证数据自身漏帧会造成证据失败；
报告按 `accepted_vs_witness` / `witness_vs_sd` 区分，不能仅据失败归因于 Rev D。

检查每通道序号及时间戳顺序。两路 IRQ 队列合并可能产生跨通道时间回退，这单独计数，
不当作丢帧；若应用需要严格全局时间排序，应另做排序功能和相应时延验收。
未匹配 bench ID/payload 的背景流量单独计数；同 bench ID 的损坏数据及其它 run_id 会失败，避免污染本次结果。
计数器按相邻样本相减，仅对接近 UINT32_MAX 的回绕做模 2^32 处理。
其它回退按重置/不确定处理；高水位和最大延迟保留 baseline/end/min/max，不相减冒充计数。
需要新的 boot_id/uptime 和诊断字段；旧固件仍可采集，但不具备无复位验收证据时判 INCONCLUSIVE。

建议测试矩阵：每路阶梯帧率 → 20 分钟连续录制 → 小段高频旋转 → 同时整屏刷新 →
接近满卡/不同合格卡的长忙实测。每次只改变一个变量，保留 sender 的主机节拍结果和 SD 最大忙时。
实际可持续帧率及最坏忙时需上板测量；本工具不会将离线模型结果当作吞吐承诺。

## Payload RVDB/1

固定 64 字节，CAN FD+BRS。默认标准 ID `0x5A0 / 0x5A1`；扩展 ID `0x18FF5A00 / 0x18FF5A01`。
所有整数字段为小端。每路序号从 0 独立递增，run_id 跨两路相同。

| 字节 | 字段 |
| --- | --- |
| 0..3 | ASCII `RVDB` |
| 4 | 协议版本 1 |
| 5 | 逻辑通道 0 或 1 |
| 6..7 | 保留，必须为 0 |
| 8..15 | uint64 run_id |
| 16..23 | uint64 sequence |
| 24..31 | uint64 planned_offset_ns，相对发送计划起点，非总线时间戳 |
| 32..59 | `pattern[i]=(run_id + sequence*17 + channel*31 + i*13) & 0xFF`，i=0..27 |
| 60..63 | 前 60 字节的 IEEE CRC32，Python `zlib.crc32` |

允许 candump 的 BRS flags=1 或含冗余 FDF 位的 flags=5；ESI、RTR、错误帧及 ID/通道不一致会失败。
日志 parser 另外支持经典帧、RTR、错误帧和 can-utils 的点分字节，供明确过滤背景流量。

技术依据：

- Linux 官方 [SocketCAN 文档](https://docs.kernel.org/networking/can.html)：`struct canfd_frame`、CAN_RAW_FD_FRAMES、local loopback、MSG_CONFIRM。
- linux-can 官方 [candump.c](https://github.com/linux-can/can-utils/blob/master/candump.c)：原生 `-L` 日志采集方式。
- 已有固件复用的 can-utils 编解码器及许可证位于 `fw/services/logfmt/`，本目录 Python 代码为 MIT。

FreeRTOS 对照另见 `docs/software/09_FreeRTOS对照版本与验收.md`。
`compare --models bare.json rtos.json --json ab.json --csv ab.csv` 只输出 MODEL。
实板形式接受至少三对 `--bare/--rtos/--bare-monitor/--rtos-monitor` 文件和
`--environment`（参见 `ab_environment.example.json`），逐项检查相同负载、环境、窗口和顺序。
