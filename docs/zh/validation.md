# 验证记录与支持边界

[文档索引](../README.md) · [构建复现](build.md)

本文将实现能力、离线验证和实机验收分开记录。源码版本在 CMake 中为 0.3.0，
CHANGELOG 仍标记未发布。更新文档或 CI 不代表已经发布新的 SDK 包。

## 已执行的通信库验证

截至 2026-11-02，下面的记录可在本仓库公开核对：

| 源码 | Linux amd64/ARM64 Debug/Release | Windows x64 Debug/Release |
| --- | --- | --- |
| 签名提交 `50586ff3b201ed944d7ca8d9dac4ebbaee5c1753`（PR #1） | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41445930151) | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41445930282) |
| 合并提交 `d4e1c70567bd6ec4b3fbfd4a60b4156ec2fcc53f` | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41446343946) | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41446343963) |

两次源码树相同。Linux 记录涵盖全量离线 CTest、5 组恢复用例各重复 20 次、共享总线示例、
安装后消费者；两个 Debug 任务还执行汇总行覆盖率至少 95% 的硬门槛。
Windows 记录涵盖原生 DLL fixture、Debug/Release、ZIP、SHA256、来源信息及中文空格目录消费。
精确工具版本、测试数量和覆盖率以对应 run 的日志/附件为准，不把某次数字当作以后提交的保证。

相同源码的本地 Ubuntu 24.04 ARM64 / GCC 13.3 检查为 Debug/Release 各 167 个 CTest 项通过，
汇总生产行覆盖率 2,935/3,079（95.3%）。这是行覆盖率，不是分支覆盖率；当次分支覆盖率为 61.1%。
29 个 Session/故障用例的 TSan 逐例进程检查通过；整组单进程运行曾报告不同测试复用栈地址的
锁顺序告警，不能将该整组运行记为无告警。回归负面对照省略物理 reopen 时，失败重试测试按预期失败；
生产恢复逻辑未因此修改。后续修改仍应保存自己的通过、失败和跳过证据。

## 平台与设备能力

- Linux amd64/ARM64 的构建和 fixture 通过，不证明存在同架构厂商库或驱动。
- SocketCAN 需要 Linux CAN 网络接口；Unix socketpair 验证帧和资源契约，不验证真实 CAN 线。
- PCAN 的硬件目标为 Windows x64；Linux 动态库测试仅核验 loader ABI。
- MiniCANFD 需要相同进程架构的 `libcanbus.so` 或 x64 `HCanbus.dll`，不能在 ARM64 中加载 x64 库。
- SLCAN 支持显式 Lawicel Classic / CANable FD 配置，但仍标为实验性；PTY 和字节流测试不证明固件兼容。
- 当前矩阵不包含 macOS 实机、Windows ARM64、USB 热插拔验收或长期负载/硬实时保证。

## 历史实机陈述：保留出处，不作本仓库验收

[最初导入的 README](https://github.com/soulteary/linkerbot-transport-cpp/blob/4393d831dcdb1abb1873e685964f6eb17427f121/README.md)
及其后端文档声称配套 hand SDK 有以下历史记录，部分标注日期为 2026-10-19：

| 历史路径 | 导入文档陈述 |
| --- | --- |
| Linux SocketCAN | 六型号 L6、O6、L20 Lite、L20、L30、O30 常用功能 |
| Windows Debug PCAN Classic | L6、O6、L20 Lite、L20；PCAN-Basic 5.0.0.1115 |
| Windows Debug MiniCANFD | O30；x64 HCanbus.dll 3.3.103.0；L30 未测 |
| Windows Debug CANable SLCAN | 六型号，限报告记录的固件与参数 |

这些是**历史转述**，并非本轮补做的实机测试。本通信库不包含对应报告、原始日志、
适配器序列号或 hand SDK 固定提交；原文只给出了配套仓库内
`docs/hardware-acceptance/<型号>/report.md` 的路径。无法从本仓库独立复核，
因此不能据此宣布当前源码或任意同名硬件通过验收。需要实机支持时，应向维护者取得固定
hand/transport SHA、报告和原始证据。当前维护入口是本文及对应后端指南，而不是旧日期陈述。

历史陈述也没有覆盖 PCAN FD、MiniCANFD L30、Lawicel 固件、Linux 原生厂商后端/SLCAN 实物、
Release 实机、USB 热插拔和长期负载。新增驱动、固件、架构或型号应单独验收。

## 新增实机记录需要什么

保留 transport/hand 完整 SHA、操作系统和架构、编译器/CRT、驱动与 DLL/固件版本、
适配器身份、接口/通道、UART/CAN/FD 时序、手部型号和身份回复、命令及预期结果。
分别记录读取、发送/业务确认、错误注入、断连恢复和持续负载，并报告时延分布、丢帧与退出耗时。
设备执行成功不能仅由发送回执代替；超时或确认丢失时不得自动重发非幂等控制命令。

CI 记录只对其 run 的提交有效。合并后重新检查 main，发布前再核对实际包、SHA256、架构、
来源信息与安装消费结果；不要复用旧包校验值或把新增文档视为新实机证明。
