# 发行版下载与校验

[English](../en/releases.md) · 简体中文

[文档索引](../README.md) · [源码构建](build.md) · [升级与回滚](operations.md)

当前源码使用 `linkerbot-transport` 包名。已发布至 v0.3.2 的包仍名为 `linkerbot-comm`；
下方 0.3.0 下载示例保留真实的旧资产名和安装路径。参见[命名迁移](naming.md)。

## 0.4.0

[版本 0.4.0](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.4.0)
新增共享 Runtime 所有权、独立设备 Session，以及复用现有 writer 的可选调度控制。
新增显式经典 CAN/FD 发送类型、原生接收时间戳元数据和 SocketCAN/PCAN 自身发送帧
接收选项；vendor 与 SLCAN 拒绝不支持的自身发送帧接收开启配置。

保留现有 C++ 签名和默认行为。公共结构有新增字段，使用方须使用匹配的头文件和库
重新编译，不承诺与旧 SDK 的二进制 ABI 兼容。Python 接入前探针属于测试夹具，
并非生产 Python 绑定或 hand/arm 接入；实机验收仍待完成。参见
[CAN 元数据](can-metadata.md)、[设备 Session](device-sessions.md)及[版本记录](../../CHANGELOG.md)。

发布验证沿用下方十二目标矩阵，归档名前缀为 `linkerbot-transport-0.4.0-`。
正式发布后再下载附件，并核对签名标签、manifest 和校验值。旧版本附件不变。

## 0.3.3

[版本 0.3.3](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.3)
新增可选通用串口和 Modbus RTU（03/04/06/10），修复 CAN/RTU 生命周期问题，
规范包名改为 `linkerbot-transport`，CAN 目标为 `linkerbot::can`。
旧包名、目标、选项及归档路径继续兼容。见[命名迁移与回滚](naming.md)和
[串口/RTU 契约](modbus-rtu.md)。本版本不包含 hand SDK 或设备寄存器映射。

沿用下方的十二目标产物矩阵。新归档以 `linkerbot-transport-0.3.3-` 开头，
安装文档和 metadata 位于 `share/linkerbot-transport`。默认 SDK 包含 CAN、串口、
Modbus 库及与 CAN 字节完全相同的旧 comm 库副本。核对签名标签、
`release-manifest.json`、`SHA256SUMS.txt` 和对应包的校验值。
验证和发布完成后才提供附件，源码版本及 CI 产物本身不代表正式发布。

例如在 Ubuntu 24.04 ARM64 上，使用 GitHub CLI 和 Bash 下载对应 Release SDK
及校验值，再构建移动目录后的消费者：

```bash
gh release download v0.3.3 --repo linker-bot/linkerbot-transport-cpp \
  --pattern 'linkerbot-transport-0.3.3-Linux-aarch64-ubuntu24.04-*-Release.tar.gz*' \
  --dir sdk-0.3.3
cd sdk-0.3.3
sha256sum --check *.sha256
archives=(*.tar.gz)
test "${#archives[@]}" -eq 1
tar -xzf "${archives[0]}"
prefix="$PWD/${archives[0]%.tar.gz}"
cmake -S "$prefix/share/linkerbot-transport/validation/installed-consumer" \
  -B consumer -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix"
cmake --build consumer --parallel 4
ctest --test-dir consumer --output-on-failure --no-tests=error
```

按实际发行版和进程架构选择包。Ubuntu 用户态及离线适配器测试不代表原生内核或实机验收。

## 0.3.2 产物矩阵

[v0.3.2 发行页面](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.2)
在验证和发布完成后提供下载。0.3.2 工作流要求八份实际 Ubuntu 专用 Release SDK（20.04、22.04、24.04、26.04 ×
x86_64/aarch64），以及四份 Windows SDK（x64/ARM64 × Debug/Release）。每个 Ubuntu
包均在自身发行版构建和消费，另保留 Focal 跨发行版及 Noetic 检查。十二份归档、十二份
单包校验文件、manifest 和总校验清单共 26 个上传附件。Windows ARM64 须通过原生运行
和对象 machine 校验才能发行。核对签名标签、`release-manifest.json` 和全部校验值，
选择匹配的发行版、架构和 CRT。清单记录来源、构建身份和验证运行；原生 SDK 检查不证明
厂商驱动或硬件可用。见 [CI 产物政策](ci.md)，不增补或替换下方 0.3.1 资产。

## 0.3.1 产物政策

[发行页面 v0.3.1](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.1)
在发行验证和发布完成后提供下载；源码版本号本身不代表已经发布。

- Linux x86_64/aarch64 Release 静态 SDK 使用 Ubuntu 20.04、GCC 9、libstdc++ ABI1。
  新包名包含构建发行版、编译器、标准库/ABI 和配置。
- Windows x64 MSVC v143 ZIP 提供 Release `/MD` 和 Debug `/MDd`，Debug 仍需开发运行库。
- 完整发行验证在 Ubuntu 20.04/22.04/24.04/26.04 双架构消费相同 Linux 归档，并在 Focal
  运行 Noetic 节点/nodelet 集成。
- 每份归档附 SHA256 文件。`release-manifest.json` 记录来源、构建身份和验证运行；
  `SHA256SUMS.txt` 覆盖其余九个上传附件。核对签名标签和清单，不根据文件名推断兼容性。

静态 SDK 不含厂商驱动或库，用户态检查不等于原生内核或硬件验收。范围见 [CI 指南](ci.md)。
下方既有 0.3.0 资产保留原名、来源 SHA 和校验值。

## 0.3.0

从上游仓库[下载 0.3.0](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.0)。
签名标签 `v0.3.0` 对应源码提交 `a46177e86e403bbd18834aedb6e81ef3233c6c22`。
上游与 [fork 标签](https://github.com/soulteary/linkerbot-transport-cpp/tree/v0.3.0)
指向同一源码；二进制包从上游 Release 下载。
`main` 上后续的文档提交不会移动该标签或替换发行包。

- Linux：`aarch64`（ARM64）和 `x86_64`（amd64）的 Release TGZ 包。
  使用 Ubuntu 24.04 和 GCC 13/libstdc++ 构建，不保证兼容更旧的系统运行库。
- Windows：x64 MSVC v143 14.44 的 Release（`MD`）和 Debug（`MDd`）ZIP 包。
  Debug 依赖 Visual Studio 开发运行库，见 [Windows 指南](windows-build.md)。
- 每个包附带 `.sha256` 文件。`SHA256SUMS.txt` 覆盖其余九个上传附件，包括
  `release-manifest.json` 和四个校验文件，不覆盖 GitHub 自动生成的源码归档。

发行包提供静态 SDK，不是设备控制程序，也不包含厂商驱动或库。
选择时匹配进程架构、编译器和运行库；将 `share/linkerbot-comm/build-info.json`
与源码标签、发行清单核对。

## 下载并使用历史 0.3.0 Linux ARM64 包

在 ARM64 Linux 上执行，需要 `curl`、`sha256sum`、`tar`、C++17 编译器和 CMake 3.20+。
使用新目录。Linux amd64 应选择 `x86_64` 归档。

```bash
mkdir linkerbot-0.3.0
cd linkerbot-0.3.0
release_url=https://github.com/linker-bot/linkerbot-transport-cpp/releases/download/v0.3.0
archive=linkerbot-comm-0.3.0-Linux-aarch64.tar.gz
curl --fail --location --remote-name "$release_url/$archive"
curl --fail --location --remote-name "$release_url/$archive.sha256"
sha256sum --check "$archive.sha256"
tar -xzf "$archive"
prefix="$PWD/linkerbot-comm-0.3.0-Linux-aarch64"
cmake -S "$prefix/share/linkerbot-comm/validation/installed-consumer" \
  -B consumer -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix"
cmake --build consumer --parallel 4
ctest --test-dir consumer --output-on-failure --no-tests=error
```

校验值用于发现下载文件与公开校验值不一致的情况；签名标签验证源码，并不是二进制归档的
独立签名。[发行验证记录](validation.md#release-030)链接构建运行并说明验证范围。
真实硬件须单独验收。

## 发布后的文档

[标签内的文档](https://github.com/linker-bot/linkerbot-transport-cpp/tree/v0.3.0/docs)
和发行包内容保留发布时快照。其中支持文档仍含发布前“尚无稳定发行版”的表述；该表述已由
[当前维护的支持政策](../../SECURITY.zh-CN.md)和本页更正。
后续勘误以[维护中的文档索引](../README.md)为入口。
文档更新不改变 0.3.0 的二进制包或校验值。
