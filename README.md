# linkerbot-transport-cpp

[English](#english) | [简体中文](#简体中文) | [Documentation / 文档](docs/README.md)

## English

C++17 CAN / CAN FD transport library for Linkerbot SDKs. It owns physical links,
shares connections, routes incoming frames and schedules outgoing batches.
Device commands, request/reply matching and hand control belong in the device SDK.
The source version is **0.3.0 (unreleased)**; a successful CI artifact is not a published release.

### Start here

- [Build, install, CMake options and coverage](docs/zh/build.md) — Linux first.
- [Session, routing, ownership and recovery](docs/zh/session.md).
- Backends: [SocketCAN](docs/zh/socketcan.md), [MiniCANFD](docs/zh/minicanfd.md),
  [PCAN](docs/zh/pcan.md), [SLCAN](docs/zh/slcan.md).
- [Windows builds and packages](docs/zh/windows-build.md).
- [Errors and diagnostics](docs/zh/troubleshooting.md), [upgrade and rollback](docs/zh/operations.md).
- [Validation evidence and hardware limits](docs/zh/validation.md).

Detailed guides are maintained in Chinese. This English overview is current;
there is no separately maintained English translation of every guide.

<a id="backends"></a>
<a id="validated-scope-030"></a>

### Backends and support

| Backend | Implementation target | Frames | Runtime requirements |
| --- | --- | --- | --- |
| SocketCAN | Linux | Classic / FD | Configured CAN network interface |
| MiniCANFD | Linux / Windows x64 | FD | Matching vendor library and driver |
| PCAN-Basic | Windows x64 hardware target | Classic / FD | PEAK driver and matching DLL |
| Native SLCAN | Linux / Windows; experimental | Classic / CANable FD | Explicit port and matching firmware profile |
| FakeTransport | Offline use | In-memory frames | No adapter or vendor library |

Linux amd64 and ARM64 Debug/Release CI checks the library, offline regression,
recovery, example and installed consumer. Both Debug jobs require at least 95%
aggregate production **line** coverage for code compiled on Linux. Windows x64
Debug/Release CI additionally checks native DLL ABI fixtures and relocated ZIPs.
These checks do not establish hardware compatibility, Windows-only coverage,
USB hotplug reliability or real-time performance. Linux PCAN fixtures verify the
loader ABI, not Linux PCAN hardware support. See the [evidence and limits](docs/zh/validation.md).

### Build and test

Run from the source root with CMake 3.16+ and a C++17 compiler:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --no-tests=error --timeout 60
./build/comm-example
```

Tests require Catch2 3. CMake uses an installed package first, then downloads
v3.8.1 if permitted. For offline builds, install Catch2 3 and set `Catch2_DIR`
if needed, then use `-DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF`.
Top-level builds enable tests and examples; subdirectory builds default both to OFF.
The core library does not require Catch2. See the [complete build guide](docs/zh/build.md)
for a library-only build, coverage and package consumption.

<a id="use-from-cmake"></a>

### CMake integration

The repository is `linkerbot-transport-cpp`; the package name remains
`linkerbot-comm`, and the implementation target is `linkerbot::comm`.

```cmake
add_subdirectory(../linkerbot-transport-cpp linkerbot-transport-cpp)
target_link_libraries(your-sdk PRIVATE linkerbot::comm)
```

For an installed package:

```bash
cmake --install build --prefix "$PWD/install/comm"
```

```cmake
find_package(linkerbot-comm 0.3 CONFIG REQUIRED)
target_link_libraries(your-sdk PRIVATE linkerbot::comm)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/absolute/path/to/install/comm`.
Use `linkerbot::comm-compat` for legacy flat includes, or `linkerbot::comm-values`
for value-only headers without implementation linkage. The `0.3.x` line pairs
with hand SDK `0.3.x`; comm must be at least the hand version. Rebuild applications
with matching headers, architecture, compiler and runtime; binary ABI compatibility
across arbitrary builds is not promised.

<a id="receive-and-send"></a>

### Offline send/receive example

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

This example confirms an in-memory reply. Physical backend acceptance, CAN ACK
and device execution are separate events. A receipt timeout does not cancel an
in-flight driver write. Use `Session::create(factory)` for a recoverable link;
`Session::own()` and `Session::borrow()` cannot recreate their transports.
For two devices on one connection, see [the shared-bus example](examples/shared_bus.cpp).

<a id="frame-and-backend-contracts"></a>

### Frame contract

`Message.dlc` is a byte count: Classic 0..8, FD 0..64. Use `is_extended=true`
for a 29-bit identifier. BRS requires both `is_fd=true` and `is_brs=true`.
FD lengths may be padded by the backend to a representable wire length.

| TX backend / mode | Untagged message (`is_fd=false`, `is_brs=false`) |
| --- | --- |
| SocketCAN Classic | Classic |
| SocketCAN FD, MiniCANFD | FD without BRS, for compatibility |
| PCAN FD, CANable FD | Classic; set the FD flag explicitly for FD |

Custom transports must support concurrent reads/writes, bounded receive waits,
`std::nullopt` on receive timeout and idempotent, retryable cleanup where resources
remain owned. Frame parsing and business response validation remain upper-layer work.

### License and contributions

[MIT](LICENSE), Copyright (c) 2026 linker-bot. Optional vendor components are
covered by their own terms; see [NOTICE](NOTICE). Drivers, vendor DLLs and test
fixtures are not part of the runtime package. Installed documentation lives under
`share/linkerbot-comm`. See [CONTRIBUTING](CONTRIBUTING.md) and [CHANGELOG](CHANGELOG.md).

## 简体中文

本项目是 Linkerbot SDK 的 C++17 CAN / CAN FD 通信层，负责连接、共享总线、接收路由和
批次发送。设备命令、请求回复关联、型号识别与控制业务由上层 SDK 实现。
当前源码版本为 **0.3.0（未发布）**；CI 产物不等于正式发行版本。

<a id="后端与构建"></a>

### 阅读顺序

1. [构建、安装与 CMake 接入](docs/zh/build.md)：Linux、离线依赖、选项、覆盖率和消费安装包。
2. [Session 与生命周期](docs/zh/session.md)：路由、共享关闭、发送回执、取消和恢复。
3. 按设备选择 [SocketCAN](docs/zh/socketcan.md)、[MiniCANFD](docs/zh/minicanfd.md)、
   [PCAN](docs/zh/pcan.md) 或 [SLCAN](docs/zh/slcan.md)。
4. [排障与诊断](docs/zh/troubleshooting.md)、[运行、升级与回滚](docs/zh/operations.md)。
5. [验证记录与支持边界](docs/zh/validation.md)、[Windows 构建与打包](docs/zh/windows-build.md)。

完整索引见 [docs/README.md](docs/README.md)。中文详细指南和上方英文概览都按当前实现维护；
历史记录在验证文档中单独标记，不作为新增平台或硬件验收结论。

<a id="030-验证范围"></a>
<a id="接入与接口选择"></a>
<a id="帧与完成语义"></a>

### 快速接入与关键约定

上方构建命令和离线 C++ 示例可直接使用。仓库名与 CMake 包名不同：安装包用
`find_package(linkerbot-comm 0.3 CONFIG REQUIRED)`，实现链接 `linkerbot::comm`；
旧扁平头文件用 `linkerbot::comm-compat`，只读值类型用 `linkerbot::comm-values`。

Linux amd64/ARM64 和 Windows x64 的 Debug/Release 有离线 CI。Linux Debug 要求
编译到的生产代码汇总行覆盖率至少 95%，不是分支覆盖率或实机验收。
厂商库必须匹配进程架构；ARM64 CI 不证明任意厂商 x64 库可在 ARM64 使用。
SLCAN 仍属实验性，Linux PCAN 仅验证测试库 ABI。型号、驱动和实机范围统一见
[验证说明](docs/zh/validation.md)。

`Message.dlc` 表示字节数，Classic 最多 8、FD 最多 64。扩展 ID 须设置 `is_extended`，
BRS 须同时设置 `is_fd`。不同后端对未标记消息的兼容解释见上方帧契约。
入队、后端接受与设备执行是三个阶段；等待超时不会撤回在途帧。
共享同一 Session 的调用者必须协调关闭；恢复后须重新注册路由并核验设备身份。

采用 [MIT](LICENSE)；贡献流程见 [CONTRIBUTING](CONTRIBUTING.md)，版本变化见
[CHANGELOG](CHANGELOG.md)，第三方组件见 [NOTICE](NOTICE)。
