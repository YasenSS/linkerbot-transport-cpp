# MiniCANFD: platform ABI, enumeration and resource ownership

[Documentation](../README.md) · [简体中文](../zh/minicanfd.md) · [Diagnostics](troubleshooting.md) · [Operations and upgrades](operations.md) · [Validation limits](validation.md)

MiniCANFD loads the vendor library at runtime. The core SDK build does not require the vendor
development package. Linux uses `libcanbus.so`, Windows x64 uses `HCanbus.dll`; install the library
and driver separately.

## Native ABI and unified calls

`VendorCanFdApi` is an injectable, platform-independent call table. The production loader binds
entry points with separate native function-pointer types, then wraps them with `std::function`.
It does not cast a Windows single-index function to a Linux two-index function before calling it.
Existing Linux-shaped test functions can still be assigned to the table.

| Operation | Windows HCanbus.dll | Linux libcanbus.so |
| --- | --- | --- |
| Process initialization / exit | Neither binds nor calls `LibCANbus_Init/Exit` | Shared reference counting when both exist; reject if only one exists; when neither exists, lifecycle is managed inside the vendor library |
| Scan | `CAN_ScanDevice()` returns native channel count | The same function name returns physical device count |
| Open / close | `(devNum)` | `(device, channel)` |
| FD initialization | `(devNum, config*)` | `(device, channel, config*)` |
| Send / receive | `(devNum, frames*, count, timeout_ms)` | `(device, channel, frames*, count, timeout_ms)` |
| Channel type | `CAN_GetDevType(devNum)`, with compatibility for the sample's `CAN_GeDevType` | `CAN_GetCHType(device, channel)` with Init/Exit; a device-level API without Init/Exit may use `CAN_GetDevType(device)` to verify channel 0 |

Windows entry points declare `__stdcall`, with x64 as the support target. Windows `device_index`
is the enumerated devNum and `channel_index` must be 0. Invalid configuration is rejected before
scan, open or other vendor calls; do not calculate `device * 2 + channel`. Linux retains explicit
channel numbers rather than guessing a channel count from physical-device count.

Structures use natural 4-byte alignment. Public headers assert size, field offsets, standard
layout and trivial copyability: `CanFDConfig` is 20 bytes, `CanFDMsg` 80 bytes, DLC offset 9 and payload offset 16.

The currently traceable ABI source is the historical Python adapter repository's unsigned commit
[`4af61830b1562addafcb431edf8187d3d72f335f`](https://github.com/linker-bot/linkerbot-transport/commit/4af61830b1562addafcb431edf8187d3d72f335f).
Its recorded x64 HCanbus sample SHA256 is
`1c7a7fdb2b78f276cea84127f9ad72af6f7fe4004132f151586cd7899db8c5a8`.
No independently verifiable original vendor V1.3 specification, release date or digest is available.
This is historical adaptation evidence, not a current official specification or hardware acceptance.
Do not infer vendor semantics for `Config`, `Model` or `Cantype` from field names alone.

## Enumeration without CAN-frame traffic

```cpp
#include <linkerbot/can/vendor_canfd_transport.hpp>
#include <iostream>

int main() {
    namespace can = linkerbot::can;
    const auto report = can::enumerate_vendor_canfd_channels(
        std::string(u8"C:\\MiniCANFD 驱动\\HCanbus.dll"));
    for (const auto &failure : report.failures)
        std::cerr << failure.stage << ": " << failure.message << '\n';
    for (const auto &channel : report.channels)
        std::cout << channel.device_index << ':' << channel.channel_index
                  << ' ' << channel.library_path << '\n';
}
```

Windows queries all native channels returned by scan by default. Linux requires a confirmed layout, for example:

```cpp
const auto report = linkerbot::can::enumerate_vendor_canfd_channels(
    std::string("/opt/minicanfd/libcanbus.so"), {{0, 0, {}}, {0, 1, {}}});
```

The two-channel example illustrates the call only; it is not a specification for every device.
Missing Linux layout produces `layout/channel_layout_required`; a missing type interface produces
`type/type_unavailable`. Only type 1 enters the FD channel list. Classic (0), LIN (2), unknown
values and query failures retain individual diagnostics. A failed channel does not discard other
verified channels. Results retain native indices, status values and resolved library paths;
interpret an empty list together with `failures`. Enumeration neither opens channels, initializes
bitrates nor exchanges CAN frames, but does call vendor scan and type queries. Do not repeat it
inside an arbitrary real-time control loop.

Preserve the resolved library path after enumeration and use it explicitly in later factories;
do not assume an environment variable remains unchanged during reconnection.

## Opening, sharing and closing

Build `VendorCanFdTransport::Config` from the enumerated `device_index`, `channel_index` and
`library_path`. Transport construction opens the channel immediately. To defer I/O, pass a factory
that is invoked by `ManagedCANBus::start()` or `Session::create()` startup. A newly created Session
is already started and has no additional open method.

Current configuration defaults follow. A numeric default does not establish hardware acceptance;
confirm indices, bitrates and vendor fields against enumeration and vendor documentation before opening.

| Field / internal boundary | Default or limit |
| --- | --- |
| `device_index` / `channel_index` | `0` / `0`; Windows `channel_index` must be 0 |
| `library_path` | Unset; read `LINKERBOT_CANFD_LIB` or use platform default search |
| `nom_baud` / `dat_baud` | 1,000,000 / 5,000,000 bit/s |
| `config_flags` / `model` / `cantype` | `0x07` / `0` / `1`; confirm vendor meanings separately |
| `frame_type` | `0x04`; retained for aggregate-initialization source compatibility only; unused by TX or `CANFD_Init` |
| `send_timeout_ms` | 10 ms, accepts 0–100 ms |
| `receive_timeout_ms` | 10 ms, accepts 0–10 ms |
| `receive_own_messages` | `false`; `true` is rejected before library loading or channel initialization |
| Native receive buffer per call | Requests at most 64 frames |
| Native send batch per call | At most 100 frames |

Default arbitration/data rates are 1 Mbit/s / 5 Mbit/s. Standard/extended IDs, FD and BRS are
handled per message. Untagged messages on an FD Transport retain the existing FD-without-BRS
behavior. The upper-layer device protocol selects IDs and BRS; the Transport does not infer flags
from model names. `Message.dlc` counts 0–64 bytes and is converted to the vendor's 0–15 DLC code.
Unrepresentable lengths round up and are zero-padded. `Config::frame_type` does not override send flags. Set `Message::format` to
`FrameFormat::kClassic` for an explicit classic frame on the FD channel, with at most
eight bytes and no FD/BRS flags. Receive timestamps preserve the raw 32-bit vendor
counter; units, epoch and wrap extension are not inferred. Direction remains unknown,
and receive-own opt-in is unsupported by this ABI. See [CAN metadata](can-metadata.md).

A direct `write_batch()` call with more than 100 frames synchronously throws `ValidationError`.
When Session selects a native batch with `gap=0`, it passes the entire `MessageBatch` to that
interface. More than 100 frames completes the receipt with failure; `send()` / `wait()` rethrow
the saved exception. Split larger batches explicitly in the upper layer and verify each segment's
application confirmation. Changing a Pool key, deleting old records or replaying blindly does not resolve this limit.

Within a process, scan, open, close and cleanup for the same library share a coordination mutex:

- The first connection scans and saves a count snapshot. Other connections reuse it without refreshing vendor-global indices.
- One Transport owns each native channel. Duplicate opens fail before vendor Open.
- Open resources, initialized resources pending rollback, failed physical closes and failed Exit calls retain ownership.
- Enumeration returns `busy` for live connections or `cleanup` for failed deferred cleanup, without continuing to scan.
- The snapshot is released only after the last connection closes and any required Exit succeeds. Close all old connections before rescanning after hotplug.
- Initialization failure attempts to close only the channel successfully opened by that construction. Failed close enters deferred records, retried by later acquire/enumeration or `vendor_canfd_lifecycle::retry_deferred_cleanup(key)`.
- Exit failure retries only Exit, without repeating physical close or decrementing the reference count again. Each loader call owns its own module reference; modules remain loaded while used by in-flight connections or cleanup records.

Scan indices are not permanent physical identity. After reconnection the hand layer must verify
the hand identity again. In-process coordination does not prove that another process is not using the adapter.

Receive timeout accepts 0–10 ms and send timeout 0–100 ms; both default to 10 ms. `close()` first
rejects new I/O, waits for in-flight reads/writes, then calls vendor Close/Exit. Tests cover concurrent
read/write/close and repeated close. Bounded waiting depends on the driver honoring timeouts.
Native Scan/Open/Init/Close/Exit have no cancellation parameter, so the SDK cannot terminate a
thread stuck in vendor code. Validate actual driver timing, hotplug recovery and load on the target hardware.

## Library paths and dependencies

Both platforms select `Config.library_path`, then `LINKERBOT_CANFD_LIB`, then default search.
Failure for an explicit or environment path does not fall back. Linux defaults try, in order,
`libcanbus.so` through dynamic-loader search, `/usr/local/lib/libcanbus.so`, and `libcanbus.so` in
the current directory. Once a library loads, failed ABI binding reports an error without trying another candidate.

Windows interprets `Config.library_path` and `LINKERBOT_CANFD_LIB` as UTF-8. Environment values
are read with wide APIs. Explicit paths become absolute and load with `LoadLibraryExW`;
`GetModuleFileNameW` uses a growing buffer and returns the resolved module path as UTF-8.

An empty environment value is treated as unset. Empty explicit paths and paths containing NUL
are rejected. Vendor libraries must match process architecture; ARM64 offline CI does not establish
availability of an ARM64 vendor library or driver.

Explicit paths use `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` and retain
an `AddDllDirectory` cookie for the module's lifetime so dependencies can resolve from the same
directory. Default names use `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`: the application directory, System32
and explicitly registered directories, without relying on current-directory or PATH lookup.
Duplicate dependency-module names still follow Windows loader rules. Use a matching x64 DLL set
and stable directory configuration. See [Microsoft LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw).

Load/symbol `TransportIOError` retains the Windows error code in `system_error()` and
`native_status()`. Messages contain the requested path, or the resolved module path after successful
resolution. Missing dependencies commonly report 126, wrong PE architecture 193, and missing
exports 127. Error 126 alone does not establish that the top-level DLL is absent.

## Validation

See [README](../../README.md) and [Windows builds](windows-build.md) for commands.
`vendor-native-loader` uses shared libraries with independently declared ABI to test loading,
enumeration, configuration arguments, single/batch traffic, DLC/FD/BRS, channel conflicts,
failed rollback and concurrent close. Test libraries are not installed. Windows additionally
covers missing symbols, wrong architecture, dependency libraries and Chinese/space-containing
paths. Linux covers unpaired Init/Exit and device-level APIs. Offline fixtures do not establish
compatibility with arbitrary vendor DLLs or hardware.

Environment examples affect subsequently created connections only. Prefer an explicit `Config.library_path`:

```bash
export LINKERBOT_CANFD_LIB=/opt/minicanfd/libcanbus.so
```

```powershell
$env:LINKERBOT_CANFD_LIB = 'C:\MiniCANFD\HCanbus.dll'
```

Explicit parameters override the environment. A nonexistent selected path fails without fallback;
do not dump the full environment to diagnose a library path. Historical device-model statements
and traceable CI scope are in [validation evidence](validation.md); offline tests are not hardware acceptance.
This backend stores scan snapshots, claims and deferred-cleanup state only in process memory.
It writes no database or recovery log. Receipts and unfinished commands do not survive process
exit. See [operations and upgrades](operations.md) for upper-layer persistent volumes, retention
of old records, upgrade order and rollback limits.
