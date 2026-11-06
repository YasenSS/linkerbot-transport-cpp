# linkerbot-transport-cpp

![像素风横幅：控制板连接 CAN 和串口接口](.github/assets/transport-banner.png)

[![C++ Report Card](.github/cppreportcard/badge.svg)](.github/cppreportcard/README.md)

[English](README.md) | [简体中文](README.zh-CN.md) | [完整文档](docs/README.md)

术语定义见[术语与可观测行为](docs/zh/terminology.md)，详细指南均提供中英文版本。

Linkerbot SDK 的 C++17 通信层，包含 CAN / CAN FD 后端、通用串口字节流和 Modbus RTU 客户端。
CAN 模块负责物理连接、共享总线、接收路由和批次发送，CAN 请求回复关联由上层 SDK 实现。
RTU 模块处理寄存器事务，设备寄存器映射和运动控制仍由上层 SDK 实现。
参见[串口与 RTU 指南](docs/zh/modbus-rtu.md)。

当前源码版本：**0.3.3**，包含可选串口和 Modbus RTU 模块。已发布的 v0.3.2 包不包含这两项。下载[最新已发布的稳定版](https://github.com/linker-bot/linkerbot-transport-cpp/releases/latest)。
下载、校验及版本文档见[发行说明](docs/zh/releases.md)。
源码指南也涵盖[未发布变更](CHANGELOG.md)；已发布 SDK 的实际接口以对应标签、头文件和随包文档为准。

规范包名为 `linkerbot-transport`；原 `linkerbot-comm` 查找入口和库文件继续兼容。
参见[命名迁移](docs/zh/naming.md)。

## Linux 快速开始

需要 CMake 3.20+、C++17 编译器和线程支持。从源码根目录执行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
(cd build && ctest --output-on-failure)
./build/can-example
cmake --install build --prefix "$PWD/install/transport"
```

默认构建测试和示例。测试需要 Catch2 3：CMake 优先查找已安装的包，再按许可下载 v3.8.1。
离线构建时提供 `Catch2_DIR`，并设置 `-DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF`。
核心库本身不依赖 Catch2。工具安装、仅构建库及依赖的 CMake 版本要求见
[构建指南](docs/zh/build.md)；MSVC 和多配置生成器命令见 [Windows 指南](docs/zh/windows-build.md)。

## 运行最小应用

完成上方安装后，使用仓库提供的 [main.cpp](examples/quickstart/main.cpp) 和
[CMakeLists.txt](examples/quickstart/CMakeLists.txt) 构建独立应用：

```bash
cmake -S examples/quickstart -B build-quickstart \
  -DCMAKE_PREFIX_PATH="$PWD/install/transport"
cmake --build build-quickstart --parallel 4
(cd build-quickstart && ctest --output-on-failure)
```

应用通过 `FakeTransport` 发送请求并核对内存回复，无需适配器或厂商库，展示路由注册、发送回执和
显式关闭会话。多个设备共享连接的例子见 [shared_bus.cpp](examples/shared_bus.cpp)。

## CMake 接入

仓库名是 `linkerbot-transport-cpp`，安装后的规范 CMake 包名为 `linkerbot-transport`：

```cmake
find_package(linkerbot-transport 0.3 CONFIG REQUIRED)
target_link_libraries(your-sdk PRIVATE linkerbot::can)
```

源码接入时，将 `find_package` 替换为
`add_subdirectory(../linkerbot-transport-cpp linkerbot-transport-cpp)`。
旧扁平头文件使用 `linkerbot::can-compat`，只读值类型使用 `linkerbot::headers`。
`0.3.x` 与 hand SDK `0.3.x` 配套，通信库版本须不低于 hand 版本；应用应使用匹配的头文件、
架构、编译器和运行库重新编译。

## 选择后端

- **SocketCAN：** Linux，Classic CAN / FD，需要已配置的 CAN 网络接口。
- **MiniCANFD：** Linux / Windows x64，FD，需要匹配的厂商库和驱动。
- **PCAN-Basic：** Windows x64 实机目标，Classic CAN / FD，需要 PEAK 驱动和 DLL。
- **原生 SLCAN：** Linux / Windows，Classic / CANable FD；实验性，需要明确的串口与固件配置。
- **FakeTransport：** 用于离线开发和测试的内存传输层。

Linux amd64/ARM64 和 Windows x64/ARM64 有离线 CI。硬件能力取决于适配器、驱动和固件，详见
[验证证据与边界](docs/zh/validation.md)。覆盖率和 Clang 质量检查命令见 [构建指南](docs/zh/build.md)。

## API 与设备接入

先读 [公共 API、线程与所有权契约](docs/zh/api.md)，再读 [Session、路由与恢复](docs/zh/session.md)。
中文详细指南与中英文 README 均对应当前实现。

`Message.dlc` 表示字节数：Classic 0–8、FD 0–64。扩展 ID 和 FD/BRS 标志须明确设置；各后端对
未标记帧的解释有所不同。发送回执描述后端接受状态，CAN 对端 ACK 和设备执行仍须单独确认。
等待回执超时不会撤回在途写入。

后端指南：[SocketCAN](docs/zh/socketcan.md)、[MiniCANFD](docs/zh/minicanfd.md)、
[PCAN](docs/zh/pcan.md)、[SLCAN](docs/zh/slcan.md)。
帧格式、时间戳及自身发送帧接收选项见 [CAN 元数据说明](docs/zh/can-metadata.md)。接入真实设备前请阅读
[排障与诊断](docs/zh/troubleshooting.md) 和 [运行、升级与回滚](docs/zh/operations.md)。

## 项目信息

[MIT 许可证](LICENSE) · [贡献流程](CONTRIBUTING.zh-CN.md) · [版本变更](CHANGELOG.md) ·
[第三方组件](NOTICE) · [支持范围与敏感报告](SECURITY.zh-CN.md)

安装文档位于 `share/linkerbot-transport`。SDK 安装包不包含厂商驱动、DLL 或测试 fixture。

## 串口与 Modbus RTU

可选模块提供独立的 `linkerbot::serial` 和 `linkerbot::modbus` 目标，不包含手型寄存器映射。
配置、事务语义与验证边界见 [RTU 说明](docs/zh/modbus-rtu.md)。
