# Rev E 裸机固件

[下载 canfd-reve.bin](canfd-reve.bin)。开发版本为 `reve-bare-reliability-20261104-dev`，源码基线为 `06fc68643cf1620be7e0c722e15ac1effa039caf`。镜像适用于 100 引脚 STM32H743VIT6 的旧 Rev E。144 引脚外部供电版 Hardware-C 需要单独的固件，已打样的 Rev D/r2 使用各自的历史版本。

镜像大小为 1,236,104 字节。它与归档开发镜像、公开源码的独立重建结果逐字节一致。编译工具为 Arm GNU Toolchain 15.2.Rel1。镜像结构检查及主机回归通过，实板功能仍待验证。[源码和构建说明](../fw/README.md)

在本目录执行以下命令可核对下载内容。Linux 使用 `sha256sum -c SHA256SUMS`，macOS 使用 `shasum -a 256 -c SHA256SUMS`。[SHA256SUMS](SHA256SUMS)

镜像随附第三方版权及许可。转发时请将许可文件和声明一同提供。[版权与组件声明](NOTICES.md) · [许可原文](licenses/)
