# 验证记录与支持边界

[English](../en/validation.md) · 简体中文

[文档索引](../README.md) · [构建复现](build.md)

本文将实现能力、离线验证和实机验收分开记录。源码版本在 CMake 中为 0.3.0。
已发布的包和校验说明见[发行指南](releases.md)；
仅更新文档或通过 CI，不代表已经发布 SDK 包。

<a id="release-030"></a>

## 0.3.0 发行验证

已发布的 [0.3.0](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.0)
使用签名标签 `v0.3.0`，标签对象为 `ba02c06867a3014ad1c01f56ea4a5602bc07189a`，
源码提交为 `a46177e86e403bbd18834aedb6e81ef3233c6c22`。
上游仓库在该提交上的 16 项检查全部通过：

- [Linux 回归与打包](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41636452844)：
  amd64/ARM64、最低 CMake、libc++、ASan/UBSan、TSan、安装后消费者及 quickstart。
- [Windows 回归与打包](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41636452738)：
  x64 Debug/Release、原生 ABI fixture 及移动目录后的消费者。
- [风格检查](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41636452851)与
  [完整报告](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41636452744)：
  完整 A+、100/100、零诊断，格式检查 54/54 文件，两个分析器各 37/37 编译单元。

本地 ARM64 Linux 回归为 218/218 通过。新增回执发布顺序测试在旧 Session 实现上失败，
应用修复后通过。上游 ARM64 生产代码行覆盖率为 3,673/3,838（95.7%）。
发布后重新下载全部十个上传附件，校验值、大小、源码元数据及每个静态库的七个目标文件均匹配。
四个 SDK 包分别包含相应的 Linux AArch64、Linux x86-64 和 Windows AMD64 目标文件。
下载后的 ARM64 包在本地也通过了两个安装后消费者测试。
这些是离线结果，不是物理适配器验收。

## 已执行的通信库验证

截至 2026-11-03，下面的精确提交记录可在本仓库公开核对：

此前补充的文档与最小接入回归已在 PR #9 合并提交
`79f1731c355fbd46eeb63224d89aaa9039c2df27` 上通过
[Linux](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41559611149)、
[ARM64 格式与静态检查](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41559611240) 和
[Windows](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41559611152) 工作流。
其中新增 ARM64 CMake 3.16.9 核心库构建/安装、安装后 quickstart 运行，以及旧版本启用
完整测试时预期拒绝配置的检查；GCC 矩阵也运行安装后的 quickstart。这些是固定提交的离线
记录，不是本文后续编辑提交的验证，也不证明真实适配器可用。以下保留较早修复的历史证据：

| 源码 | Linux amd64/ARM64 | ARM64 格式与静态检查 | Windows x64 |
| --- | --- | --- | --- |
| 签名提交 `50586ff3b201ed944d7ca8d9dac4ebbaee5c1753`（PR #1） | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41445930151) | 当时未提供 | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41445930282) |
| 合并提交 `d4e1c70567bd6ec4b3fbfd4a60b4156ec2fcc53f` | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41446343946) | 当时未提供 | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41446343963) |
| 合并提交 `38d30cc936cde505718314f3cdde62d022481f89`（PR #3） | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41478753364) | 当时未提供 | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41478753429) |
| 合并提交 `0c91316af183b48c224a1a3cb6f827622dafa1be`（PR #4） | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41481036933) | 当时未提供 | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41481037581) |
| 签名提交 `737ac66dd4abebdf6139b3199eb9019d91ea6186`（PR #5 head） | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553062590) | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553062589) | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553062617) |
| 合并提交 `4ad07b3b87863b04f3d25d13d9840c465497f853`（PR #5） | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553596800) | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553596723) | [通过记录](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553596766) |

PR #3 的签名 head `f854c05e26a1812fbd1748769aaf0badc50a0cea` 也有
[Linux](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41474006568) 和
[Windows](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41474006403) 通过记录；
PR #4 的签名 head `6216db5203d797561da2d6797c9636b90f7e47f9` 也有
[Linux](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41480373636) 和
[Windows](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41480373570) 通过记录。
上表保留合并后 main 记录以便直接核对主分支状态。PR #1 的 Linux 工作流只包含 GCC
Debug/Release、Session recovery 重复回归、示例、安装后消费者和当时的覆盖率门槛。PR #3
及后续 Linux 工作流才加入 libc++、ASan+UBSan、TSan、registry handoff 重复回归和双架构
至少 95% 的生产代码汇总行覆盖率；独立 ARM64 格式与静态检查从 PR #5 开始。对应时期的
Windows 记录涵盖原生 DLL fixture、Debug/Release、ZIP、SHA256、来源信息及中文空格目录消费。
精确工具版本、测试数量和覆盖率以对应 run 的日志及附件为准，不能把一次结果当作以后提交的保证。

较早的 PR #1 本地 Ubuntu 24.04 ARM64 / GCC 13.3 记录为 Debug/Release 各 167 个 CTest
通过，生产代码行覆盖率 2,935/3,079（95.3%），当次分支覆盖率为 61.1%。29 个 Session/故障
用例的 TSan 逐例进程检查通过；整组单进程运行曾因不同测试复用栈地址报告锁顺序告警，不能
将该整组运行记为无告警。回归负面对照省略物理 reopen 时，失败重试测试按预期失败；生产恢复
逻辑未因此修改。该历史记录不能替代后续提交自己的验证。

PR #5 head 的本地 ARM64 验证结果如下；Linux 构建在 Ubuntu 24.04 容器中执行，actionlint
在 ARM64 host 上执行：

- Clang 18/libc++ 严格构建、GCC 13 Debug、GCC 13 Release 与 TSan 各 211/211 个 CTest 通过；
- ASan+UBSan 209/209 通过，按 CI 规则仅排除两个 native-loader fixture；
- 安装后的 libc++ consumer 1/1 通过；
- Session recovery 与三个 registry handoff 组在 TSan 下各重复 20 次通过；
- 生产代码行覆盖率为 3,635/3,798（95.7%），不是分支覆盖率或逐文件 95%；
- clang-format、clang-tidy 和 actionlint 1.7.12 通过。

静态 owner 在 `main()` 中取得 `Session::share()` 或 `Session::borrow()` 并保留到进程退出时，
修复前的 ARM64 ASan 对照会在 registry release 报告 heap-use-after-free；修复后的两个独立退出
用例及完整 ASan 套件通过。这是进程退出阶段的可靠性问题。修复保持原有
registry key、记录保留和校验行为，未删除或改写旧记录。

本地未运行 x86；amd64 由上述 GitHub CI 验证。没有执行真实硬件验收，也没有编译需要外部官方
PCAN 头的 `validation/pcan_header_abi.cpp`。第一次本地 Release 配置因关闭依赖下载且未提供
Catch2 包路径，在构建前停止；改用已缓存的固定 Catch2 3.8.1 源码后，完整 Release 构建和
CTest 通过。通过、失败和跳过项不能相互替代。

本节较早的记录属于 `soulteary/linkerbot-transport-cpp` fork，单独使用不能证明上游 main 的状态。
上方发行验证章节记录了后来在上游发布提交上完成的检查。
仓库同时保留次级 Gitea workflow 配置，但当前没有可公开核对的 act_runner 运行链接；不能把
该配置或 GitHub 上对 YAML 的检查当作 Gitea 已通过证据。

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
