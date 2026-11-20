# CAN 数据库解析模块

本轮只为主机模拟器增加数据库元数据导入，不改 STM32、SD、USB、DMA 驱动。

## 复用的模块

| 模块 | 锁定版本 | 许可 | 来源 |
| --- | --- | --- | --- |
| canmatrix | 1.3.3 | BSD-2-Clause | https://pypi.org/project/canmatrix/1.3.3/ · https://github.com/ebroecker/canmatrix |
| attrs | 26.1.0 | MIT | https://pypi.org/project/attrs/26.1.0/ |
| click | 8.5.0 | BSD-3-Clause | https://pypi.org/project/click/8.5.0/ |

许可原文保留于本目录 `licenses/`。依赖组合要求 Python 3.10 或更新版本。使用项目内虚拟环境 `.venv-sim`，不安装到系统全局。canmatrix 1.3.0 已由维护者撤回，因此锁定官方建议的 1.3.3。

```sh
python3 -m venv .venv-sim
.venv-sim/bin/pip install -r sim/web/requirements.txt
.venv-sim/bin/python sim/web/server.py
```

## 输入和结果

`database_import.parse_database(name, raw_bytes, encoding='auto')` 返回完整数据库目录或抛出 `DatabaseImportError`。该函数没有发送、保存条目或操作 SD 的能力。

- `.dbc`：Vector/CANdb 文本，包括 11 位、29 位和明确声明的 CAN FD 帧，读取周期和信号布局。
- `.dbf`：实际内容必须是 BUSMASTER 的 `[DATABASE_VERSION]` / `[PROTOCOL] CAN` / `[START_MSG]` 文本格式。本期 DBF 支持 Classic CAN；普通 dBase/表格 DBF、未知 DBF、CAN FD DBF 扩展及其他协议拒绝，并建议从原工具导出 DBC。
- 自动尝试 UTF-8、GB18030、CP1252。也可显式选择这些编码、GBK 或 ISO-8859-1；名称乱码时需选择原编码，不能把自动猜测当作绝对识别。
- 单文件最多 2 MiB、4096 帧；每帧最多 256 信号、总计最多 16384 信号，单行不超过 32 KiB。
- 校验原始 ID 后再交给 canmatrix，防止不合法高位被上游掩码悄悄移除。拒绝长度、信号边界、重复同格式 ID、非 CAN/CAN XL 及不完整/损坏结构；解析器日志错误和它吞下异常后打印的错误也会导致整份失败，不返回部分目录。
- 默认字节均为 `00`。数据库描述布局，不含实际报文实例；本期不把信号初值编码为数据。未指定、事件型或超出当前发送条目范围的周期先填 100 ms，并给出警告。BRS 只在明确 `CANFD_BRS=1` 时启用，不从 FD 标记自行推断。
- canmatrix 1.3.3 的 DBF 加载器先把原始 ID 当标准帧校验，再应用 `X` 标记。适配器只在解析副本中转为该库的 bit31 扩展 ID 表示，返回的 29 位 ID 始终与原文件一致，已用无 bit31 的 BUSMASTER 扩展帧回归验证。

`examples/review.dbc` 和 `review.dbf` 是为此模拟器创建的通用测试数据，不对应真实车辆数据库。示例同样只提供结构与全零草稿。

## 验证

```sh
.venv-sim/bin/python tests/test_database_import.py
```

测试实际调用 canmatrix 解析 DBC/DBF，覆盖 Classic、扩展、FD+BRS、信号/周期、编码、损坏输入、容量和类型边界。接口和页面的导入应用操作另由 HTTP 与浏览器回归检查。
