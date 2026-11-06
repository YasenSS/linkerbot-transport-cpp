# SocketCAN: Linux Classic CAN / CAN FD

[Documentation](../README.md) · [简体中文](../zh/socketcan.md) · [Session](session.md) · [Operations and upgrades](operations.md) · [Validation limits](validation.md)

`SocketCANTransport(interface_name, fd_mode=false)` uses a Linux CAN network interface.
Construction creates a raw CAN socket, selects the interface and binds it. It does not configure
the bitrate, bring the interface up, or install a driver. The commands below change interface
state; stop applications using the channel and check adapter parameters before running them.

## Configure an existing interface

Confirm that the system recognizes the physical interface. Use
`ip -details -statistics link show can0` to inspect its name and state.
The following bitrates are examples and must match the other nodes and hardware capabilities:

```bash
# Classic: 1 Mbit/s
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 1000000
sudo ip link set can0 up
ip -details -statistics link show can0
```

FD configuration requires an FD-capable adapter, kernel driver and bus nodes:

```bash
# FD: 1 Mbit/s arbitration, 5 Mbit/s data
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 1000000 dbitrate 5000000 fd on
sudo ip link set can0 up
ip -details -statistics link show can0
```

Interface configuration normally requires network-administration privileges. Run the application
under the socket/device permissions of the host system; do not grant all privileges merely to run
the control program. A container must also see the CAN interface in the relevant network namespace.
Mounting `/dev/ttyACM0` does not configure a SocketCAN network interface. Serial SLCAN is a separate backend.

See the [Linux SocketCAN documentation](https://docs.kernel.org/networking/can.html) for parameters,
kernel support and vcan. SDK offline tests do not require can0 or vcan; automated tests must not
reconfigure an existing physical channel. Interface names, bitrates and up/down state are OS
configuration, not library state. Upgrading or rolling back this library does not restore previous
`ip link` settings; deployment procedures must retain and recheck that configuration.

## Integration and recovery factory

This Linux-only example constructs a connection. Subscribe to the protocol's reply IDs before
submitting actual device commands:

```cpp
#include <linkerbot/can/socketcan_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <memory>

int main() {
    auto session = linkerbot::transport::Session::create([] {
        return std::make_unique<linkerbot::can::SocketCANTransport>("can0", true);
    });
    auto inbox = session->subscribe_routes({{0x28, false}});
    // 0x28 is only a routing example; the caller supplies the protocol,
    // device-identity checks and device commands.
    session->close();
}
```

Share the connection through Pool/Session when multiple devices use the same interface; do not
bypass batch scheduling with independent upper-layer senders. Recovery recreates the socket only.
It does not execute `ip link`, reconfigure the bitrate or replay control frames. If an interface has
been removed or renamed, repair the OS configuration before following the
[recovery procedure](session.md#recovery-close-and-diagnostics).

## Frames, waits and errors

Classic mode receives `can_frame`. FD mode receives both Classic and FD frames and retains the
extended-ID, FD and BRS flags. CAN error frames and RTR frames are not delivered as application
frames; filtering one returns an empty optional immediately. FD receive lengths above 64,
unexpected read sizes and system I/O failures produce errors.
`Message.dlc` counts bytes; extended identifiers require `is_extended=true`.

For compatibility, an untagged message sent in FD mode is FD without BRS. Setting `is_fd=false`
alone cannot force a Classic transmission in FD mode. Set `Message::format` to
`FrameFormat::kClassic` to select it explicitly; FD/BRS flags must be false and the
payload must not exceed 8 bytes. `kUnspecified` retains the old FD default. A Classic
backend rejects FD/BRS and payloads larger than 8 bytes.

The options constructor is `SocketCANTransport(options, interface_name)`.
`SocketCANOptions::receive_own_messages` defaults to false and `timestamp_mode` to
`kDisabled`; opt in to native echo or software/raw hardware timestamps explicitly.
`read()` uses `recvmsg()` to preserve available metadata. See [CAN metadata](can-metadata.md)
for clock domains, missing timestamps, direction flags and hardware-validation limits.

The socket receive timeout is 10 ms and the send timeout is 50 ms. A receive timeout returns an
empty optional, but an empty result does not prove that the full receive wait elapsed. A negative system `write()` result throws `TransportIOError` with `system_error()`;
an unexpected short write also throws `TransportIOError`, but may have no system error code.
Invalid frames throw `ValidationError` before the system call. These socket options and Session
deadlines do not specify a device-execution deadline or guarantee that every driver returns within
a hard time limit. A receive timeout means only that no application frame was returned during that wait.
A send timeout or `write()` failure does not prove non-delivery; inspect the error, receipt and device state.

`close()` rejects new I/O, waits for in-flight calls, and attempts the actual fd close once.
Even if Linux close reports an error, the old fd number must not be closed again because it may
have been reused. `AdoptFd` is an offline socketpair test entry point, not a recommended application configuration.

## Troubleshooting

- `Failed to get interface index`: check the interface name, network namespace and driver-created interface.
- `Failed to enable CAN FD frames`: check kernel and adapter FD support.
- The channel opens but no application reply arrives: check interface state, bitrate, wiring/termination,
  node identity, ID/extended flag and routes.
- bus-off or a full TX queue: preserve `ip -details -statistics link show can0`, system errors and
  application receipts before applying the recovery procedure for the device and driver. Do not replay
  control commands unconditionally.
