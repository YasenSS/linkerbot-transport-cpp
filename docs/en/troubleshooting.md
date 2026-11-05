# Errors, diagnostics, and troubleshooting

English · [简体中文](../zh/troubleshooting.md)

[Documentation index](../README.md) · [Session](session.md) · [Operation and upgrades](operations.md)

Distinguish invalid configuration, connection/driver failures, queue pressure, and device-level failures. Passing offline tests does not exclude physical-driver problems. An empty receive result does not by itself indicate a disconnected link or prove that the full receive timeout elapsed: some backends also return empty after filtering frames or processing partial input. Use the error type, execution stage, and actual effect to determine the cause.

## Stable error interface

`CANError` derives from `std::runtime_error` and provides `code()`, `system_error()`, and `native_status()`. System errors and vendor status are optional; retain the exception type, classification, and message together. `error_code_name(code)` returns a stable lowercase name suitable for a log field. Do not parse `what()` to select application behavior.

| Class / name | Typical triggers | Handling boundary |
| --- | --- | --- |
| `ValidationError` / `validation` | Invalid ID, length, FD/BRS, bitrate, path, route conflict, or a lifecycle call from a callback | Correct the configuration/call; repeated recovery does not fix parameters |
| `ClosedError` / `closed` | Closed Session, old generation, or invalidated inbox | Recreate after closure; an old inbox cannot be reused after recovery |
| `TimeoutError` / `timeout` | Queueing, pacing, receipt wait, or backend time budget | Identify the stage; do not assume no frame was sent |
| `QueueFullError` / `queue_full` | Bounded send queue at capacity | Reduce submission rate, coalesce replaceable tasks, or increase capacity within a memory budget |
| `TransportIOError` / `transport_io` | System call, dynamic library, or driver/vendor error | Retain system error, native status, and backend parameters; handle according to the backend |
| `FaultedError` / `faulted` | Repeated I/O errors, channel conflict, or a claim retained after failed cleanup | Address the fault and owner cleanup before recovery/reacquisition |
| `InternalError` / `internal` | Unclassified nonstandard exceptions, for example | Retain a minimal reproducer and source SHA; do not report success |

`transport::CancelledError` is a transport cancellation exception outside the `can::CANError` hierarchy. Waiting for a receipt can throw it. The application parses device-protocol error codes; the library does not convert them into these classes. PCAN also exposes `PcanTransportError::reason()` for bus-off, unavailable adapters, queue-full conditions, and bus errors.

## Receipts and lost acknowledgments

Inspect `SendResult.outcome`, `sent_frames`, `current_frame_uncertain`, and `error`:

- `pending`: unfinished; the writer may already have claimed the task.
- `dispatched`: all frames were accepted by the backend; this does not prove CAN acknowledgment or device execution.
- `superseded`: a newer task replaced a queued `latest` task with the same endpoint/family; an in-flight batch is not replaced.
- `cancelled`, `expired`, `failed`: use the known accepted-frame count and current-frame uncertainty to assess progress.

`current_frame_uncertain` can refer to the current frame or an entire native batch. A native-batch
failure may not report per-frame progress; `sent_frames` counts only known accepted frames and is
not proof that all remaining frames were undelivered.

`result()` does not rethrow the saved exception and can be used after a wait failure. `wait()`/`wait_until()` do rethrow it. A receipt wait timeout does not cancel the task. If a business reply is lost, read actual device state and verify identity and business sequence numbers before the application decides whether a retry is valid. Changing the request identifier, deleting local records, or blindly replaying control frames does not resolve uncertainty.

## Health fields and observation

`health()`/`get_health()` read software state without sending hardware probes. `connected()` means only that the cached state is open; device identity and business replies are needed to assess actual availability.

| Object | Fields | Meaning |
| --- | --- | --- |
| `TransportHealth` | state, generation, consecutive_errors, last_error_code/last_error | Lifecycle, connection generation, consecutive receive errors, and latest error |
| `TransportHealth` | last_successful_tx/rx | Most recent software send/receive success on a monotonic clock, not UTC/Unix time |
| `TransportHealth` | subscriber_count, disabled_subscriber_count, last_subscriber_error | Callback count, disabled callbacks, and their latest error |
| `SessionHealth` | rx_frames/rx_bytes, tx_frames/tx_bytes | Cumulative valid receives and known backend-accepted frames/payload bytes, not wire traffic |
| `SessionHealth` | route_misses, invalid_frames, overflows | Cumulative unrouted frames, invalid frames, and new frames rejected by full inboxes |
| `SessionHealth` | pending_tx, queue_high_water, recoveries | Waiting plus in-flight tasks, waiting-queue peak, and successful recoveries |
| `RouteHealth` | pending, high_water, overflow, closed, generation | Inbox occupancy, peak, overflow, closure, and valid generation |

Session capacity counts waiting batches and excludes the in-flight task, whereas `pending_tx` includes it. Do not compare both against the same limit. ManagedCANBus `pending_tx` describes its legacy send queue; the Session `transport_health()` projection adds Session pending work.

By default, Dispatcher enters faulted state after 10 consecutive counted receive errors, with a 5 ms base and 50 ms maximum backoff. Timeouts and validation errors do not count toward that threshold. A callback is disabled after three consecutive exceptions. Check diagnostics rather than assuming every subscriber still receives broadcasts. These are `CANDispatcher::Options` defaults, not device heartbeat thresholds.

The library has no HTTP endpoint, Prometheus exporter, PromQL, or alert rules. Applications exporting metrics must define names, label cardinality, reset-on-restart behavior, and monotonic-clock conversion, and retain the last business-reply time. Documentation field names are not built-in Prometheus metrics. The last error alone is insufficient as a current-availability alert.

## Common failure checks

| Symptom | Check first |
| --- | --- |
| CMake cannot find Catch2 | Use Catch2 3; install it or provide `Catch2_DIR` for offline builds; check the download option |
| `find_package` cannot find the package | Package name is `linkerbot-transport`; `CMAKE_PREFIX_PATH` points to the install prefix, not the source directory |
| Undefined symbols / coverage link errors | Link the matching transport module; match architecture/CRT; consumers of coverage-instrumented static libraries also need `--coverage` |
| Dynamic library cannot load | Explicit paths override environment variables; check architecture, dependencies, and exports in the backend guide |
| Windows errors 126/193/127 | These can indicate missing dependencies, wrong architecture, or missing exports respectively; 126 does not prove the top-level DLL is missing |
| Session route conflict | Exact `(id, extended)` already belongs to another open inbox; coordinate routes between Session sharers |
| `ClosedError` after recovery | Resubscribe: the old inbox is invalid. An explicitly closed Session cannot recover |
| Send completed but no reply | Check frame-mode compatibility defaults, ID/extended flags, inbox overflow, device identity, and business parsing |
| Slow close/process exit | A driver write, callback, or batch may still be active; retain thread stacks; do not synchronously close from callbacks |
| SLCAN initialization/ACK timeout | Check firmware profile, UART/CAN rates, echo/timestamps, and exclusive port use; CANable does not wait for serial ACK |

Do not run `env` or print the complete process environment when checking library paths: it can disclose passwords, tokens, or verification codes. Record the successfully resolved `library_path` and inspect only relevant configuration fields.

## Evidence for an issue report

Follow the [contribution guide](../../CONTRIBUTING.md): provide a fixed SHA, platform/architecture/compiler, backend, publishable adapter and driver details, minimal commands/program, structured exception, receipt details, generation, and event order. For concurrency/exit problems include thread roles, Session/lease ownership, cleanup retries, and lost business acknowledgments. Reproduce before fixing, and retain regression evidence of failure before and success after the fix. State reproduction limits and do not enlarge the claimed impact.
