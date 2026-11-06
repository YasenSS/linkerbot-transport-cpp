# linkerbot-transport-cpp

![Pixel-art banner showing a controller board connected to CAN and serial interfaces](.github/assets/transport-banner.png)

[![C++ Report Card](.github/cppreportcard/badge.svg)](.github/cppreportcard/README.md)

[English](README.md) | [简体中文](README.zh-CN.md) | [Documentation](docs/README.md)

<a id="english"></a>

C++17 transport library for Linkerbot SDKs, with CAN / CAN FD backends, serial
byte streams and a Modbus RTU client. The CAN module manages physical connections,
shared buses, incoming-frame routes and outgoing batches; CAN request/reply
matching belongs in the device SDK. The optional RTU module handles register
transactions. Device register maps and motion control remain in the device SDK.
See the [serial and RTU guide](docs/en/modbus-rtu.md).

Current source version: **0.3.3**, including optional serial and Modbus RTU modules.
Published v0.3.2 packages do not contain those modules. See the [latest published stable release](https://github.com/linker-bot/linkerbot-transport-cpp/releases/latest).
See [downloads, checksums and release documentation](docs/en/releases.md).
Source guides also cover [Unreleased changes](CHANGELOG.en.md); use the matching tag,
headers and bundled documentation to determine a published SDK's actual interfaces.

The canonical package is `linkerbot-transport`; former `linkerbot-comm` calls and
archives remain available as compatibility entries. See [name migration](docs/en/naming.md).

## Linux quick start

From the source root, with CMake 3.20+, a C++17 compiler and threads:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
(cd build && ctest --output-on-failure)
./build/can-example
cmake --install build --prefix "$PWD/install/transport"
```

The default build includes tests and examples. Tests require Catch2 3: CMake first
looks for an installed package, then downloads v3.8.1 if permitted. For offline
builds, provide `Catch2_DIR` and set `-DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF`.
The library itself does not require Catch2. See the [build guide](docs/en/build.md)
for prerequisites, library-only builds and dependency-specific CMake requirements.
For MSVC and multi-configuration commands, use the [Windows guide](docs/en/windows-build.md).

<a id="receive-and-send"></a>

## Run the minimal application

After the install step above, build the complete
[quickstart application](examples/quickstart/main.cpp) with its supplied
[CMakeLists.txt](examples/quickstart/CMakeLists.txt):

```bash
cmake -S examples/quickstart -B build-quickstart \
  -DCMAKE_PREFIX_PATH="$PWD/install/transport"
cmake --build build-quickstart --parallel 4
(cd build-quickstart && ctest --output-on-failure)
```

It uses `FakeTransport` to send a request and check an in-memory reply, without an
adapter or vendor library. It demonstrates routing, a send receipt and explicit
session shutdown. For two devices on one connection, see
[shared_bus.cpp](examples/shared_bus.cpp).

<a id="use-from-cmake"></a>

## Use from CMake

The repository is named `linkerbot-transport-cpp`; the canonical installed package is
`linkerbot-transport`:

```cmake
find_package(linkerbot-transport 0.3 CONFIG REQUIRED)
target_link_libraries(your-sdk PRIVATE linkerbot::can)
```

For a source-tree dependency, replace `find_package` with
`add_subdirectory(../linkerbot-transport-cpp linkerbot-transport-cpp)`.
Use `linkerbot::can-compat` for legacy flat includes, or
`linkerbot::headers` for value-only headers. The `0.3.x` line pairs with hand
SDK `0.3.x`; the transport SDK must be at least the hand version. Rebuild applications with
matching headers, architecture, compiler and runtime.

<a id="backends"></a>
<a id="validated-scope-030"></a>

## Choose a backend

- **SocketCAN:** Linux, Classic CAN / FD; a configured CAN network interface.
- **MiniCANFD:** Linux / Windows x64, FD; matching vendor library and driver.
- **PCAN-Basic:** Windows x64 hardware target, Classic CAN / FD; PEAK driver and DLL.
- **Native SLCAN:** Linux / Windows, Classic / CANable FD; experimental, with an explicit port and firmware profile.
- **FakeTransport:** in-memory transport for offline development and tests.

Linux amd64/ARM64 and Windows x64/ARM64 have offline CI. Hardware support depends on
the adapter, driver and firmware; see [validation evidence and limits](docs/en/validation.md).
Coverage and Clang quality-check commands are in the [build guide](docs/en/build.md).

<a id="frame-and-backend-contracts"></a>

## API and device integration

Start with [public API and ownership](docs/en/api.md), then
[Session routing and recovery](docs/en/session.md). The detailed guides and README
are maintained in English and Chinese; see the [terminology guide](docs/en/terminology.md).

`Message.dlc` counts bytes: Classic 0–8, FD 0–64. Set extended-ID and FD/BRS
flags explicitly; untagged-frame behavior varies by backend. A send receipt
describes backend acceptance, while CAN ACK and device execution require separate
confirmation. A receipt timeout does not cancel an in-flight write.

Backend guides: [SocketCAN](docs/en/socketcan.md), [MiniCANFD](docs/en/minicanfd.md),
[PCAN](docs/en/pcan.md), [SLCAN](docs/en/slcan.md).
See [frame selection, timestamps and receive-own messages](docs/en/can-metadata.md) for opt-in metadata and per-backend limits.
See [diagnostics](docs/en/troubleshooting.md) and [upgrade/rollback](docs/en/operations.md)
before deploying with devices.

## Project information

[MIT license](LICENSE) · [Contributing](CONTRIBUTING.md) · [Changelog](CHANGELOG.en.md) ·
[Third-party notices](NOTICE) · [Support and sensitive reports](SECURITY.md)

Installed documentation lives under `share/linkerbot-transport`. Vendor drivers, DLLs
and test fixtures are not included in the SDK package.

<a id="简体中文"></a>
<a id="后端与构建"></a>
<a id="030-验证范围"></a>
<a id="接入与接口选择"></a>
<a id="帧与完成语义"></a>

中文内容已移至 [简体中文 README](README.zh-CN.md)。

## Serial and Modbus RTU

Optional serial byte streams and a Modbus RTU client are available as separate
`linkerbot::serial` and `linkerbot::modbus` targets. They do not implement hand
register maps. See the [RTU guide](docs/en/modbus-rtu.md) for configuration,
transaction semantics and validation limits.
