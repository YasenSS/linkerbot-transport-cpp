# Public API, threading and ownership contracts

[简体中文](../zh/api.md) · [Documentation](../README.md) · [Session](session.md) ·
[Diagnostics](troubleshooting.md) · [Operations, upgrades and rollback](operations.md)

This guide describes the interfaces under `include/linkerbot/`. The project uses C++17.
Device commands, identity checks, request/reply correlation and business-level idempotency
belong to the upper SDK. Link `linkerbot::comm` to construct implementation objects or perform I/O;
use `linkerbot::comm-compat` for legacy flat includes. `linkerbot::comm-values` supplies
header search paths and the C++17 requirement only; it does not replace the implementation library.

## Interface layers

| Layer | Main types | Purpose and ownership |
| --- | --- | --- |
| Frames and values | `can::Message`, `TransportHealth`, error types | Describe frames, state and stable error categories; do not own a device |
| Physical backend | `can::CANTransport`, SocketCAN/SLCAN/PCAN/MiniCANFD | One object owns one physical connection; construction acquires resources and `close()`/destruction releases them |
| Reception and physical batch boundaries | `can::CANDispatcher` | Dedicated receive thread, subscription broadcasts and serialized physical transmit batches |
| Recoverable shared bus | `can::ManagedCANBus` | Stores a factory, explicit `start/stop/recover`, legacy transmit queue |
| In-process connection pool | `can::SharedCANBusPool`, `CANBusLease` | Same-key bus sharing; leases retain ownership needed to retry the final close |
| Device SDK session | `transport::Session`, `RouteInbox`, `SendReceipt` | Exact routing, one transmit scheduler, deadlines, cancellation and generation-based recovery |
| Offline testing | `can::FakeTransport` | In-memory I/O, error injection and synchronization gates; does not represent an adapter |

Device SDKs normally use Session. Use lower layers when implementing a backend or maintaining legacy code.

## Frames and backend interpretation

`Message.dlc` is a payload **byte count**, not the 0–15 wire DLC code. Standard identifiers
are at most `0x7ff`; extended identifiers are at most `0x1fffffff` and require
`is_extended=true`. Classic frames carry at most 8 bytes and FD frames at most 64;
`is_brs=true` requires `is_fd=true`.

| Backend / mode | Transmit interpretation of `is_fd=false,is_brs=false` |
| --- | --- |
| SocketCAN Classic, PCAN Classic, Lawicel Classic | Classic |
| SocketCAN FD, MiniCANFD | Compatibility behavior: FD without BRS |
| PCAN FD, CANable FD | Classic; set `is_fd=true` explicitly for FD |

Backends may round lengths such as 9, 10 or 11 bytes up to a representable wire length
and pad with zeros. Upper-layer parsers must use the returned length and validate the
business format. Backend acceptance does not establish CAN acknowledgment or device execution.

## `CANTransport` implementation contract

Custom backends derived from `can::CANTransport` must meet these requirements:

- `read()` uses a finite receive wait and returns `std::nullopt` on a normal timeout.
  An empty result means that this call returned no application frame; a backend can also
  return it early after filtering a frame, as SocketCAN does for RTR and error frames.
  It does not prove that the full receive timeout elapsed or that no traffic arrived.
  Link failures, closed state or invalid received data may throw. OS or driver calls can
  extend elapsed time; the configured wait is not a hard return-time guarantee.
- The dispatcher and transmit threads may call `read()` and `write()` concurrently;
  protect resources used by that pairing.
- `write()` returns after backend acceptance. Failures use stable `CANError` subclasses
  and preserve system errors or native status when available.
- Default `write_batch()` calls `write()` sequentially. A backend combining a batch into
  one native call cannot reliably identify every partially accepted frame.
- `prefers_write_batch()` indicates that a native batch may be used when the inter-frame
  gap is zero. `initial_delay` still occurs before the batch, and backend limits still apply.
- `close()` rejects new I/O, coordinates in-flight operations and retains retry ownership
  while releasable resources remain owned.

Without cancellation APIs for driver Scan/Open/Init/Close/Exit or synchronous writes,
the library cannot guarantee a hard thread-termination deadline.

## Dispatcher, ManagedCANBus and Pool defaults

`CANDispatcher` uses one receive thread. By default it becomes faulted after 10 consecutive
receive errors counted toward the fault threshold, with 5 ms base and 50 ms maximum backoff.
Timeouts and validation errors do not count toward that threshold. A subscription callback
is disabled after three consecutive exceptions; health reports the disabled count and latest
callback error. External unsubscribe/stop calls wait for callbacks already entered to finish.
A callback thread cannot synchronously wait for lifecycle operations requiring that same thread to exit.

Constructing `ManagedCANBus` stores the factory without opening a device. Current `Options` defaults:

- `send_queue_size=2000`
- `send_batch_max=32`
- `send_interval=0 ms`
- dispatcher defaults: 10 receive errors and 5/50 ms backoff as above

`start()`, `stop()` and `recover()` are serialized. `queue_send()` and `can::SendReceipt`
are legacy transmit interfaces. New device code should use Session batches and
`transport::SendReceipt`; the two receipt types are distinct.

`CANBusKey` combines backend, endpoint, device/channel, FD mode and nominal/data bitrate
into a logical configuration. Physical-conflict comparison excludes bitrate and FD fields,
so the same physical channel cannot be acquired concurrently with conflicting configuration.
If `CANBusLease::close()` fails, the lease retains its bus, key and retry ownership.
Final release from receive/fault callbacks uses in-process deferred cleanup; external threads
perform retry/drain. Removing Pool records or changing keys does not resolve failed cleanup.

## Session, deadlines and cancellation

The default Session transmit queue holds 2000 **waiting batches**; a route inbox defaults
to 512 frames. An executing batch does not occupy waiting-queue capacity but counts toward
`pending_tx`. Defaults are FIFO, priority 0 and no finite deadline. See the
[Session guide](session.md) for sharing, routing, receipts, recovery and exit contracts.

`transport::Clock` is `std::chrono::steady_clock`; deadlines and timestamps are not UTC
or Unix time. `CancellationSource` and its tokens share an atomic, one-way state.
Cancellation is repeatable but cannot be reversed. Cancellation, deadline expiry or a wait
timeout does not establish that a frame already passed to a driver was not delivered.
Inspect `sent_frames`, `current_frame_uncertain` and device state at the business layer.

## Errors and health

`CANError` exposes stable `code()`, optional `system_error()` and `native_status()` values.
Do not branch on parsed `what()` text. `transport::CancelledError` does not derive from
`CANError`. Error categories, receipt outcomes and health fields are described in
[Diagnostics](troubleshooting.md).

`health()` / `get_health()` returns an object-local software snapshot without probing hardware;
`connected()` means only that cached state is open. Gauges such as `pending_tx`, subscription
count and consecutive-error count change with state. Session cumulative counters cover only
that Session object's lifetime, reset on recreation and are not persisted. Monotonic timestamps
need upper-layer correlation before being used in reports with wall-clock time.

## FakeTransport testing boundary

`FakeTransport` injects received frames, records transmit history, simulates read/write/batch/close
failures and controls write gates. It supports unit tests and hardware-free consumer checks.
An `on_request()` handler runs synchronously while the FakeTransport mutex is held; it must not
call back into the same transport. It does not model driver scheduling, USB, CAN arbitration,
acknowledgment, bus-off timing or real throughput, and does not replace backend fixtures or hardware tests.

## Compatibility, persistence and upgrades

`0.3.x` does not promise binary ABI compatibility across arbitrary compilers, CRTs or build
options. Recompile applications using matching headers and static libraries. Added
`[[nodiscard]]` annotations require an explicit `(void)` when intentionally discarding results
under `-Werror`, but do not change symbol ABI. Flat headers are migration aids; new code
should use `<linkerbot/...>` include paths.

This library has no database, Redis, disk queue or cross-restart acknowledgment records and
does not itself require a persistent volume. In-process registries, scan snapshots, inboxes,
receipts and deferred cleanup are not persistent state. Upper layers retain their own logs,
calibration, device identities and business confirmations. See
[Operations](operations.md) for retention, manual reconciliation, upgrade order and rollback limits.
