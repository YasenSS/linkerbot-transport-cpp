# PCAN Classic CAN / CAN FD

[Documentation](../README.md) · [简体中文](../zh/pcan.md) · [Diagnostics](troubleshooting.md) · [Operations and upgrades](operations.md) · [Validation limits](validation.md)

`PcanTransport` provides attached-channel enumeration and runtime loading. Building the core SDK
does not require a vendor development package. The hardware support target is Windows x64 / MSVC;
Linux test shared libraries provide offline ABI validation.

## Vendor components and ABI

Install the matching-architecture PEAK driver, PCAN-Basic DLL and dependencies separately.
SDK packages do not include vendor headers, DLLs, drivers or test DLLs. Automatic enumeration
requires PCAN-Basic support for `PCAN_ATTACHED_CHANNELS_COUNT` / `PCAN_ATTACHED_CHANNELS`.
FD additionally requires the three FD entry points and a confirmed FD-capable channel.
See the [official PCAN-Basic page](https://www.peak-system.com/products/software/development-packages/pcan-basic/?L=1).

The ABI reference is the [official PCANBasic.h](https://www.peak-system.com/produktcd/Develop/PC%20interfaces/Windows/PCAN-Basic%20API/Include/PCANBasic.h).
The code declares its own Classic/FD ABI subset. Windows entry points use `__stdcall`; the backend does not call the XL API.

The header URL is mutable, not a versioned archive. The PCAN-Basic 5.1.0.1194 header retrieved
on 2026-11-03 says `Last change: 2025-10-24`, contains 40,098 bytes and has SHA256
`1ff0c5723b7733e145bebdfca12da0ed7bd174ab46a845be2bf3c647087f14d2`.
When updating vendor components, retrieve the header again and record its digest; this digest is
not evidence for future versions. The vendor header is not committed or distributed in packages.

Public headers assert layouts: Classic message 16 bytes, FD message 72 bytes, Classic timestamp
8 bytes, FD timestamp 8 bytes, and channel information 52 bytes. Message data begins at offset 6;
channel features/name/id/condition offsets are 4/8/44/48. `validation/pcan_header_abi.cpp`
compares these layouts against the official header.

## Loading, enumeration and configuration

Selection order is explicit UTF-8 `PcanConfig::library_path`, then `LINKERBOT_PCAN_LIB`, then
system `PCANBasic.dll`. Failure for an explicit or environment path does not select another DLL.
Windows uses wide-character loading. Explicit paths become absolute; dependencies are searched
in the library directory and restricted default directories. The `AddDllDirectory` cookie lives
as long as the module. Failures retain the original system code; the API stores the resolved module path.

`enumerate_pcan_channels()` returns channels reported as attached by the vendor, preserving native
handle, hardware name, device ID, controller, FD capability, driver occupancy condition and this
process's ownership flag. Names such as `PCAN_USBBUS1` only format an actual handle; they do not
guess available channels. Capability or availability query failures retain diagnostics. Unknown FD capability rejects FD use.

```cpp
#include <linkerbot/can/pcan_transport.hpp>
#include <algorithm>

int main() {
    namespace can = linkerbot::can;
    auto api = can::load_pcan_api();
    const auto attached = can::enumerate_pcan_channels(api);
    const auto selected = std::find_if(attached.channels.begin(), attached.channels.end(),
        [](const can::PcanChannel &channel) {
            return (channel.condition & can::pcan_abi::available) && !channel.owned_by_process;
        });
    if (selected == attached.channels.end()) return 1;
    can::PcanConfig config;
    config.channel = selected->handle;
    config.mode = can::PcanMode::kClassic;
    config.nominal_bitrate = 1'000'000; // Must match the target bus.
    can::PcanTransport transport(config, api);
    transport.close();
}
```

This example selects an available channel without checking the connected device's identity.
Production applications must select a confirmed adapter and channel; the first available entry
does not identify a particular device. The name `PCAN_USBBUS1` does not prove attachment.

Classic accepts 1M, 800k, 500k, 250k, 125k, 100k, 50k, 20k, 10k and 5k bit/s.
FD uses `CAN_InitializeFD` / `CAN_ReadFD` / `CAN_WriteFD`, independently of Classic bitrate constants.
`PcanFdTiming{}` defaults to an engineering timing of 80 MHz, nominal BRP/TSEG1/TSEG2/SJW
1/63/16/16, and data 1/11/4/4, producing exactly 1M/5M bit/s. This timing has not been
accepted on specified hardware. When changing bitrate, also set prescalers, segment lengths and SJW.
Configuration validates ranges, SJW ≤ TSEG2 and exact bitrates.

PCAN sends and receives explicit frame flags: `is_fd=false,is_brs=false` remains Classic on an
FD channel when `format` is unspecified. `FrameFormat::kClassic` and `kFd` explicitly
select the frame type; BRS still requires both `is_fd=true` and `is_brs=true`. The device protocol
selects standard/extended IDs and BRS; this library has no hand-model encoder. Wire DLC 0–15 maps
to 0–64 bytes. Unrepresentable send lengths round up to a legal DLC and are zero-padded.
Classic rejects payloads above 8 bytes, FD/BRS and invalid IDs without truncation.

## Lifecycle and errors

One Transport may claim a native channel in the same resolved module at a time, including
conflicting modes, bitrates and timings. Capability checks precede initialization. Cleanup calls
`CAN_Uninitialize(handle)` only for the acquired channel and never uses global handle 0.
Initialization returning CAUTION means resources were acquired without strictly meeting the
configuration; construction rejects the result and rolls back with `CAN_Uninitialize(handle)`.
A successful rollback immediately removes this process's claim. Only rollback failure retains
the claim, module reference and deferred-cleanup ownership. Other ordinary initialization errors
do not mark the channel initialized or clean up other connections.

`close()` wakes receive polling, serializes cleanup after current I/O, and is safe to repeat.
A failed close retains the claim for a later `close()` retry. Destructor cleanup failure, or
rollback failure after construction acquired a channel, becomes deferred cleanup. Later enumeration
or open attempts retry it. One channel's cleanup failure retains diagnostics without blocking
other channels. Applications may also call `retry_pcan_cleanup(api)` explicitly.

Receive combines nonblocking vendor reads with interruptible 1 ms polling. The default wait is
10 ms; configuration accepts 0–100 ms. A pure `QRCVEMPTY` is treated as an empty receive queue;
combined error statuses are not suppressed. `read()` also returns empty after filtering non-data
frames until the deadline or after at most 256 vendor read attempts, so an empty optional does not by itself
prove that the full receive wait elapsed. `PcanTransportError::reason()` distinguishes bus-off, unavailable/uninitialized
adapters, TX queue full and bus errors, retaining raw status bits in `native_status()`.
RTR frames are filtered. Echo frames are filtered by default; opt-in
`PcanConfig::receive_own_messages=true` enables the native setter and delivers them with
`Message::is_rx=false`. Missing setter support fails before initialization; driver
rejection follows the channel cleanup/retry protocol. Status/error messages produce
errors instead of application data. Driver timestamps retain microsecond values and
a driver-specific clock, without epoch conversion. See [CAN metadata](can-metadata.md).
Synchronous driver calls have no cancellation API, so a stuck driver cannot be forcibly interrupted.
Windows driver call timing and load behavior still require hardware validation.

## Validation

See [validation evidence](validation.md) for Windows Debug/Release CI and Linux fixtures.
Imported hand SDK / PCAN Classic hardware claims are retained there as historical reports;
they do not validate current source, PCAN FD or a new driver.

Commands are in [README](../../README.md) and [Windows builds](windows-build.md).
`pcan-native-loader` dynamically loads `pcan-fixture`, whose native ABI is declared independently,
covering Classic/FD, Classic-only libraries, missing symbols, enumeration, frame conversion and cleanup.
The fixture is not installed. Real DLLs, drivers and adapters need hardware tests for bidirectional
traffic, USB disconnect recovery, mode switching and sustained load.

With a separately obtained official header, optionally compile the ABI comparison. The object
file goes to a temporary directory; the command does not copy the vendor header. ARM64/Linux
uses fixed-width aliases to check layout and function types, not Windows `__stdcall`:

```bash
clang++-18 -std=c++17 -Iinclude \
  -DPCAN_REFERENCE_HEADER='"/absolute/path/PCANBasic.h"' \
  -c validation/pcan_header_abi.cpp -o /tmp/pcan_header_abi.o
```

An explicit library path overrides `LINKERBOT_PCAN_LIB`. An empty environment value is treated
as unset; an empty explicit path is rejected. The Linux loader defaults to `libpcanbasic.so`;
this implementation detail does not change the Windows x64 hardware target. Receive wait defaults
to 10 ms and accepts 0..100 ms. The default channel 0 is rejected before opening; select a channel explicitly.

```powershell
$env:LINKERBOT_PCAN_LIB = 'C:\PEAK\PCANBasic.dll'
```

Environment changes affect subsequent loads only; they do not replace the DLL used by an existing
connection. Dependency handling and cleanup retries are described above.
