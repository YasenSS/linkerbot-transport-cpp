# SLCAN: native serial on Windows / Linux

[Documentation](../README.md) · [简体中文](../zh/slcan.md) · [Diagnostics](troubleshooting.md) · [Operations and upgrades](operations.md) · [Validation limits](validation.md)

The SDK provides two explicit firmware profiles: Lawicel Classic and the CANable 2.0 FD extension.
Native serial uses the OS driver without an additional vendor DLL. Confirm the firmware protocol:
a port name or CANable product name does not establish protocol compatibility.

## Firmware profiles

| Item | `kLawicelClassic` (default) | `kCanableFd` |
| --- | --- | --- |
| Initialization | `C\r`, `S0..S8\r`, `O\r`; wait for CR after each | `C\r`, `S0..S9\r`, `Y2\r` or `Y5\r` in FD mode, `O\r`; no serial ACK |
| CAN arbitration bitrates | 10k, 20k, 50k, 100k, 125k, 250k, 500k, 800k, 1M | 10k, 20k, 50k, 100k, 125k, 250k, 500k, 750k, 1M, 83.3k (S0..S9 respectively) |
| FD data bitrates | Unsupported | 2M / 5M, Y2 / Y5 respectively |
| Classic frames | `t` standard ID / `T` extended ID, 0..8 bytes | Same; an FD connection can also transmit Classic frames |
| FD frames | Unsupported; reject FD/BRS before opening | `d` / `D` without BRS, `b` / `B` with BRS, 0..64 bytes |
| Send completion | CR or `z`/`Z` + CR indicates adapter acceptance | Only indicates that the local serial driver accepted all bytes |

For neither profile does serial send completion establish a CAN acknowledgment or successful
device-command execution. CANable firmware sends no serial ACK/NACK, so serial completion cannot
verify initialization, CAN bitrate or device connectivity. Check the hand's identity and application
reply. With the CANable profile and `fd=false`, only Classic frames are sent and no Y command is configured.

FD uses the CAN DLC mapping `0..8,12,16,20,24,32,48,64`. `Message.dlc` counts bytes.
Sending 9, 10, 11, or other unrepresentable lengths pads with zeros to the next representable wire
length; receive retains the full wire length. Each message's `is_extended/is_fd/is_brs` selects its
frame format. `config.fd=true` permits FD; it does not convert Classic frames to FD or force BRS.
`config.brs` is a configuration-validation field, not a substitute for the message's `is_brs`.
The upper-layer device protocol selects standard/extended IDs and BRS.

Protocol references: [CANable firmware README](https://github.com/normaldotcom/canable2-fw/blob/b158aa7ab106bc018715cd4d5f02f544f9de1830/README.md),
[parser](https://github.com/normaldotcom/canable2-fw/blob/b158aa7ab106bc018715cd4d5f02f544f9de1830/src/slcan.c), and
[python-can 4.6.1 SLCAN](https://python-can.readthedocs.io/en/v4.6.1/_modules/can/interfaces/slcan.html).
This implements the specified extension; it does not promise interoperability with every vendor's SLCAN FD firmware.

## Connection example

This is a program fragment: put the includes at the top of the source file and the remaining statements inside an application function.

```cpp
#include <linkerbot/can/slcan_transport.hpp>
#include <chrono>
#include <iostream>
linkerbot::can::SlcanConfig config;
config.port = "/dev/ttyACM0"; // Use COM10 on Windows; select a confirmed adapter.
config.serial_baud = 115200; // UART bitrate, independent of CAN; match the firmware.
config.nominal_bitrate = 1'000'000;
config.protocol = linkerbot::can::SlcanProtocol::kCanableFd;
config.fd = true;
config.data_bitrate = 5'000'000; // Only 2M / 5M.
config.startup_delay = std::chrono::milliseconds{0}; // Up to 5 seconds if opening resets firmware.
linkerbot::can::SlcanTransport transport(config); // Exclusive open, configuration and initialization.
const auto frame = transport.read(); // May return nullopt if no complete frame is available.
if (frame) std::cout << frame->id << '\n';
transport.close();
```

Defaults are Lawicel Classic, UART 115200, CAN 1 Mbit/s, `receive_timeout` 10 ms,
`command_timeout` 500 ms, `startup_delay` 0 ms, `fd=false`, and `brs=false`.
The default `data_bitrate` is 5 Mbit/s and is used only for FD. For Classic Lawicel, retain
the default protocol and `fd=false` without FD parameters. Disable firmware echo and timestamps
in advance; the SDK sends no speculative probe or flashing commands. The first Lawicel C command
must return a successful CR even when already closed.

The library sends `C/S/Y/O` for the current connection and does not persist those commands.
Whether an adapter retains state across reset depends on its firmware. The library stores no
cross-process receipts or firmware state. Recheck profile, bitrates and target identity after a device or service restart.

`slcan_port_name` / `validate_slcan_config` perform no I/O. `enumerate_slcan_ports` queries only
metadata for the explicit allowlist and does not open ports. An empty list returns empty; it does
not scan all serial ports. Windows accepts COM1..COM65535, including COM10+. Linux accepts absolute
`/dev/...` paths, including `/dev/serial/by-id/...`, and rejects paths containing `..` components.
Metadata does not establish that a port is available or that its firmware protocol or FD capability matches.

Linux opens the serial port directly: no `slcand` or `can0` is involved. Do not allow slcand,
ModemManager or another SDK to use the same port concurrently. Use the distribution's device groups
or udev rules to grant read/write access; Ubuntu commonly uses dialout, with a new login required
after joining. USB CDC firmware may ignore UART baud settings; still configure the value required by the firmware.

## I/O, timeouts and close

UART settings accept 1200..3000000; actual driver support is checked on open. Receive waits accept
1..100 ms, serial-write / Lawicel ACK budgets 1..5000 ms, and startup delay 0..5000 ms.
Each initialization command has its own budget. Lawicel send and receive share a parser; frames
received while waiting for a serial ACK enter the receive queue. CANable send and receive are
independent and do not wait for a serial ACK that the firmware does not provide.

`read()` pumps the serial input once when its frame queue is empty. Partial input or a protocol
acknowledgment may produce no complete application frame, so an empty optional can return before
the configured receive timeout. The caller may call `read()` again to continue parsing input.

The parser supports fragmented and concatenated serial input. Incomplete lines are limited to
26 characters for Classic and 138 for FD; up to 256 frames are buffered, and each serial read
requests at most 256 bytes. Invalid ASCII, BEL, overlong lines, queue overflow, send failure and
serial ACK timeout enter the faulted state. Close and reconstruct the transport to avoid using
a late ACK. RTR, timestamp suffixes and command echo are unsupported.
`SlcanConfig::receive_own_messages` defaults to false; true is rejected before serial
initialization. No receive timestamp or direction is synthesized. `Message::format`
can select classic/FD explicitly within the configured profile and link capability;
see [CAN metadata](can-metadata.md).

Windows uses exclusive `CreateFileW`, 8N1, no software/hardware flow control, disabled DTR/RTS,
and overlapped I/O / CancelIoEx. After cancellation it waits for OS completion before freeing
buffers; a faulty driver may exceed the budget. Release failures retain resource ownership and
support close retries plus `pending_slcan_cleanup()` / `retry_slcan_cleanup()`.

Linux uses nonblocking descriptors, termios2 raw 8N1 / BOTHER baud settings, flock / TIOCEXCL
exclusion, poll timeouts and eventfd cancellation. It handles partial writes, EINTR, USB disconnect
and local resource release. Close cancels reads/writes, attempts C without waiting for its ACK,
and always attempts local release. Even when Linux close reports an error, the same numeric fd
is not closed again because another thread may have reused it. Serial open/configure/close remain
subject to the OS driver; no hard duration bound is promised for every driver.

## Validation and throughput

`can-tests "[slcan]"` includes byte-stream fixtures and Linux pseudoterminal tests for profile
startup, ACK differences, all FD lengths, standard/extended IDs, BRS, fragmented/concatenated input,
invalid input, buffer limits, cancellation, partial writes, disconnect, exclusion and reopening.
Linux tests access only PTYs they create, not existing serial ports. See [README](../../README.md)
and [Windows builds](windows-build.md) for commands.

At 115200 baud / 8N1, ideal one-way throughput is 11520 bytes/s. An 8-byte extended Classic ASCII
frame occupies 27 bytes including CR; a 64-byte extended FD frame occupies 139 bytes. The ideal
limits are about 426 / 82 frames/s respectively. Actual throughput also depends on serial ACKs,
USB, firmware and bidirectional traffic; do not apply PCAN / MiniCANFD performance thresholds.
Hardware acceptance must record firmware, driver, adapter, UART/CAN bitrates, load and latency.

The imported Windows Debug / six CANable model statements are retained as
[historical reports](validation.md#historical-hardware-statements), not current acceptance evidence.
No hardware report is bundled. Port names, USB product names and enclosure model names do not
establish a validated combination. Lawicel hardware, Linux native serial hardware, other
firmware/drivers, Release hardware operation and sustained throughput require separate validation.
SLCAN remains experimental.
