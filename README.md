# linkerbot-transport-cpp

[English](#english) | [简体中文](#简体中文)

## English

C++17 CAN / CAN FD communication library for Linkerbot SDKs. It provides
physical connections, shared buses, receive routing and queued transmission.
Device commands, protocol parsing and hand control belong in the hand SDK.

### Backends

| Backend | Platform | Frames | Configuration |
| --- | --- | --- | --- |
| SocketCAN | Linux | Classic / FD | Configure the network interface before opening it |
| MiniCANFD | Linux / Windows x64 | FD | Vendor `libcanbus.so` / `HCanbus.dll`; [guide (中文)](docs/zh/minicanfd.md) |
| PCAN-Basic | Windows x64 | Classic / FD | PEAK driver and matching DLL; [guide (中文)](docs/zh/pcan.md) |
| Native SLCAN | Linux / Windows | Classic / CANable FD | Explicit serial port and firmware profile; [guide (中文)](docs/zh/slcan.md) |
| FakeTransport | All supported platforms | In-memory frames | Offline tests and examples |

Linux PCAN fixtures verify the loader ABI; they do not establish Linux hardware
support. SLCAN is experimental. Offline tests do not certify real adapters.

### Validated scope (0.3.0)

The GitHub Linux workflow builds and runs offline tests on Ubuntu 24.04
x86_64 and ARM64 in Debug and Release, including repeated Session recovery
checks and an installed-package consumer. These jobs do not test CAN hardware.
Both Linux Debug jobs require at least 95% aggregate production line coverage.
The measurement includes `src/` and `include/` code compiled for Linux; it
excludes tests and Catch2, and does not measure Windows-only code or hardware acceptance.

The validation baseline is Ubuntu 24.04 x86_64 / GCC 13 and Windows 11 x64 /
MSVC v143 14.44, using C++17. Windows Debug offline checks and Release engineering,
installation and ZIP relocation checks have passed for recorded source revisions.
Hardware evidence comes from the paired hand SDK: Linux SocketCAN covers all six
models; Windows Debug PCAN Classic covers L6/O6/L20 Lite/L20; MiniCANFD covers O30
with x64 `HCanbus.dll 3.3.103.0`; CANable-profile SLCAN covers all six models.
PCAN FD and L30 MiniCANFD hardware remain unverified. Lawicel firmware, native
Linux PCAN/MiniCANFD/SLCAN hardware, Release hardware, Windows USB hotplug and
long-running load are not certified by those records. Other firmware or driver
versions need their own validation; backend availability alone is not a hardware pass.

### Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --no-tests=error
./build/comm-example
```

Tests require Catch2 3. If it is missing, CMake downloads v3.8.1 by default.
For an offline build, install Catch2 3 or set `Catch2_DIR`, then pass
`-DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF`.
`LINKERBOT_COMM_BUILD_TESTS` and `LINKERBOT_COMM_BUILD_EXAMPLES` default to ON
for top-level builds and OFF for subdirectory builds. Disable tests to build
the library without Catch2. See [Windows builds (中文)](docs/zh/windows-build.md)
for MSVC, runtime and ZIP instructions.

To reproduce the Linux/GCC coverage gate, install `gcovr` and use a fresh build directory:

```bash
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DLINKERBOT_COMM_ENABLE_COVERAGE=ON
cmake --build build-coverage --parallel 4
ctest --test-dir build-coverage --output-on-failure --no-tests=error --timeout 60
gcovr --root . --filter '(src|include)/' --gcov-exclude-directories 'build-coverage/_deps' --fail-under-line 95 --txt build-coverage
```

Coverage requires tests to be enabled. Instrumented static libraries are for
validation; standalone consumers of those libraries must also link with
`--coverage`. Regular Release builds leave coverage disabled.

### Use from CMake

The repository is named `linkerbot-transport-cpp`. The CMake package remains
`linkerbot-comm`, with targets under `linkerbot::comm`, for compatibility.

From source:

```cmake
add_subdirectory(../linkerbot-transport-cpp linkerbot-transport-cpp)
target_link_libraries(your-sdk PRIVATE linkerbot::comm)
```

Or install and consume the package:

```bash
cmake --install build --prefix /path/to/comm
# Configure your application with -DCMAKE_PREFIX_PATH=/path/to/comm
```

```cmake
find_package(linkerbot-comm 0.3 CONFIG REQUIRED)
target_link_libraries(your-sdk PRIVATE linkerbot::comm)
```

`linkerbot::comm-compat` also exposes the legacy flat include names such as
`can_transport.hpp`. `linkerbot::comm-values` exposes headers and the C++17
requirement for value-only consumers; it does not link the implementation.
The `0.3.x` line pairs with `linkerbot-hand-cpp 0.3.x`; comm must be at least
the hand version. Rebuild applications with matching headers and libraries.

### Receive and send

`Session` groups receive IDs into bounded inboxes and serializes outgoing
batches. This complete example uses an in-memory adapter:

```cpp
#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <chrono>
#include <memory>
#include <utility>

int main() {
    namespace can = linkerbot::can;
    namespace transport = linkerbot::transport;
    auto adapter = std::make_unique<can::FakeTransport>();
    adapter->echo_cmd(0x28, 0x02, 6, 0x40);
    auto session = transport::Session::own(std::move(adapter));
    auto inbox = session->subscribe_routes({{0x28, false}});

    can::Message request;
    request.id = 0x28;
    request.dlc = 1;
    request.data[0] = 0x02;
    transport::SendOptions options;
    options.deadline = transport::Clock::now() + std::chrono::seconds{1};
    auto receipt = session->submit({{request}}, options);
    receipt.wait_until(options.deadline);
    auto reply = inbox->receive_until(options.deadline);
    session->close();
    return reply ? 0 : 1;
}
```

For a recoverable physical link, use `Session::create(factory)` with a factory
that opens a new backend. `Session::own(transport)` opens a session over an
existing adapter and cannot recreate it. See [Session and lifecycle (中文)](docs/zh/session.md)
for sharing, cancellation, deadlines and recovery, and
`examples/shared_bus.cpp` for two devices on one connection.

### Frame and backend contracts

`Message.dlc` is a byte count, not a wire DLC nibble. Classic supports up to
8 bytes and FD up to 64. BRS requires `is_fd=true`. Set `is_extended=true`
for 29-bit IDs. FD lengths that need padding are converted at the backend.

| TX backend / configuration | `is_fd=false, is_brs=false` |
| --- | --- |
| SocketCAN `fd_mode=false` | Classic |
| SocketCAN `fd_mode=true`, MiniCANFD | FD without BRS (compatibility default) |
| PCAN FD, CANable FD | Classic; explicitly set `is_fd=true` for FD |

`CANTransport` implementations own the physical link. `read()` returns
`std::nullopt` on timeout; `write()` returns when the backend accepts the frame.
Reads and writes may run concurrently. Custom backends must bound receive waits,
handle concurrent I/O and provide idempotent cleanup through `close()`.
`CANDispatcher` broadcasts to callbacks; `ManagedCANBus` adds explicit
start/stop/recover and queued sends; `SharedCANBusPool` shares connections by key.

Queue acceptance, backend acceptance and device execution are separate events.
Wait on a receipt for the backend result; inspect device replies for business
success. A receipt wait timeout does not cancel an in-flight write. `CANError`
provides stable error categories, system errors and native status codes.

### License

[MIT](LICENSE), Copyright (c) 2026 linker-bot. See [NOTICE](NOTICE) for optional
vendor libraries. Installation includes this README and the configuration guides
under `share/linkerbot-comm`. Vendor binaries and drivers are obtained separately.
See [CONTRIBUTING](CONTRIBUTING.md) for issue reports and contributions, and
[CHANGELOG](CHANGELOG.md) for version changes.

## 简体中文

Linkerbot SDK 共用的 C++17 CAN / CAN FD 通信库，负责物理连接、共享总线、接收路由和
排队发送。设备命令、协议解析及手部控制由 hand SDK 负责。

### 后端与构建

| 后端 | 平台 | 帧类型 | 配置说明 |
| --- | --- | --- | --- |
| SocketCAN | Linux | Classic / FD | 打开前自行配置网络接口及速率 |
| MiniCANFD | Linux / Windows x64 | FD | [厂商库、枚举与资源归属](docs/zh/minicanfd.md) |
| PCAN-Basic | Windows x64 | Classic / FD | [PEAK 驱动、DLL 与时序](docs/zh/pcan.md) |
| 原生 SLCAN | Linux / Windows | Classic / CANable FD | [串口与固件配置](docs/zh/slcan.md)，实验性支持 |
| FakeTransport | 各支持平台 | 内存帧 | 离线测试与示例 |

Linux 上的 PCAN 测试动态库只验证 loader ABI。离线测试不代表真实适配器已通过验收。

### 0.3.0 验证范围

GitHub Linux 工作流覆盖 Ubuntu 24.04 x86_64 与 ARM64 的 Debug / Release 构建、
离线测试、重复 Session 恢复检查和安装包消费验证；这些任务不验证 CAN 实物。
两个 Linux Debug 任务还要求生产代码的汇总行覆盖率至少达到 95%。统计包括 Linux 构建中
编译到的 `src/` 与 `include/`，排除测试和 Catch2，不衡量 Windows 专用代码或实机验收。
Linux/GCC 的本地复现命令见上方 [Build and test](#build-and-test)；覆盖率静态库仅用于验证，
单独链接它的消费者也须加 `--coverage`，常规 Release 构建默认不启用覆盖率。

基线为 Ubuntu 24.04 x86_64 / GCC 13 和 Windows 11 x64 / MSVC v143 14.44，使用 C++17。
已有 Windows Debug 离线检查和 Release 工程、安装、ZIP 解压接入通过记录，均对应
记录中的源码组合。配套 hand SDK 的实机结果如下：

| 路径 | 实机验证范围 |
| --- | --- |
| Linux SocketCAN | L6、O6、L20 Lite、L20、L30、O30 常用功能通过 |
| Windows PCAN Classic | L6、O6、L20 Lite、L20，Debug 适配检查通过 |
| Windows PCAN FD | 已实现，L30、O30 实机未测 |
| Windows MiniCANFD | O30 的 Debug 检查通过，x64 `HCanbus.dll 3.3.103.0`；L30 未测 |
| Windows SLCAN | 六型号的 Debug 检查通过，仅覆盖报告记录的 CANable 配置，保持实验性 |

上述结果不覆盖 Lawicel 固件、Linux 原生 PCAN/MiniCANFD/SLCAN 实物、Release 实机、
Windows USB 拔插或长期负载，也不代表所有固件和驱动版本。按型号的完整记录维护在配套
hand SDK 的 `docs/hardware-acceptance/<型号>/report.md`；本库不重复维护各手型报告。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --no-tests=error
./build/comm-example
```

测试依赖 Catch2 3；缺失时默认下载 v3.8.1。离线构建先安装 Catch2 3 或指定 `Catch2_DIR`，
再加 `-DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF`。顶层构建默认启用测试和示例，
作为子目录引入时默认关闭；分别通过 `LINKERBOT_COMM_BUILD_TESTS`、
`LINKERBOT_COMM_BUILD_EXAMPLES` 控制。关闭测试后，核心库不依赖 Catch2。
Windows 的 MSVC、运行库和 ZIP 消费方式见 [Windows 构建](docs/zh/windows-build.md)。

### 接入与接口选择

仓库名为 `linkerbot-transport-cpp`；为保持兼容，CMake 包名仍为 `linkerbot-comm`，
链接目标仍使用 `linkerbot::comm` 系列名称。

源码接入使用 `add_subdirectory`，安装包使用 `find_package(linkerbot-comm 0.3 CONFIG REQUIRED)`，
均链接 `linkerbot::comm`，完整命令见上方英文示例。旧扁平头文件用户链接
`linkerbot::comm-compat`；只使用值类型、不调用实现的目标可链接 `linkerbot::comm-values`。
`0.3.x` 与 `linkerbot-hand-cpp 0.3.x` 配套，comm 版本不得低于 hand，更新后配套重编译。

| 接口 | 用途 |
| --- | --- |
| `CANTransport` | 后端读写、关闭；构造即获取链路资源 |
| `CANDispatcher` | 一条连接广播给多个回调；回调自行过滤帧 |
| `ManagedCANBus` | 构造不打开设备；显式 start/stop/recover、排队发送 |
| `SharedCANBusPool` | 相同通道配置共享连接；最后一个 lease 负责关闭 |
| `transport::Session` | 精确 ID 路由、有界 inbox、批次调度、deadline 与取消 |

上方完整示例演示 `Session` 单设备收发；[共享总线示例](examples/shared_bus.cpp) 演示双设备。
需要恢复物理连接时使用可重建后端的 `Session::create(factory)`。路由、共享、回执与
恢复规则见 [Session 与生命周期](docs/zh/session.md)。

### 帧与完成语义

`Message.dlc` 表示字节数，Classic 最多 8 字节、FD 最多 64 字节，不是线上 DLC 编码。
扩展 ID 设置 `is_extended=true`；BRS 必须同时设置 `is_fd=true`、`is_brs=true`。
MiniCANFD 和 SocketCAN `fd_mode=true` 将未标记帧按 FD、无 BRS 发送；
PCAN 和 CANable FD 仍按 Classic 发送，必须显式设置 FD 标志。

入队成功、后端接受、设备执行成功是三个阶段。回执等待后端结果，设备执行仍需业务回复。
等待回执超时不会取消已经进入驱动的写入。`CANError` 保留稳定错误分类、系统错误和厂商状态码。
`read()` 超时返回 `std::nullopt`；自定义后端须支持并发读写、有界接收等待及幂等关闭。

采用 [MIT 许可证](LICENSE)，第三方说明见 [NOTICE](NOTICE)。安装包的文档位于
`share/linkerbot-comm`；厂商库和驱动需另行获取。
问题反馈和贡献流程见 [CONTRIBUTING](CONTRIBUTING.md)，版本变化见 [CHANGELOG](CHANGELOG.md)。
