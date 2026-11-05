# Session, routing and lifecycle

[简体中文](../zh/session.md) · [Documentation](../README.md) · [Public API](api.md) ·
[Diagnostics](troubleshooting.md) · [Operations](operations.md)

`include/linkerbot/transport/session.hpp` provides the session interface for device SDKs.
It supplies exact receive routing and one transmit-scheduler thread on a physical connection.
Device protocols and request/reply correlation remain upper-layer responsibilities.

## Construction and resource ownership

| Entry point | Behavior | Resources affected by `close()` |
| --- | --- | --- |
| `Session::create(factory, tx_capacity=2000)` | Creates and starts a ManagedCANBus; can call the factory again for recovery | Stops its owned bus |
| `Session::own(unique_ptr<CANTransport>)` | Takes an already constructed transport and uses an owned dispatcher | Stops its owned dispatcher; no physical recovery |
| `Session::share(shared_ptr<ManagedCANBus>)` | Starts or reuses the bus; the same bus returns the same Session | Closes the session without stopping the external bus |
| `Session::lease(CANBusLease)` | Reuses the Session for that bus and retains the supplied lease | Releases leases; the last lease closes the connection |
| `Session::borrow(CANDispatcher&)` | Borrows a running dispatcher; the same dispatcher reuses the session | Unsubscribes without stopping the external dispatcher; no recovery |

A constructed Session is ready for I/O; there is no separate `open()` step. Constructing
`ManagedCANBus` only stores its factory; `start()` opens the device. To share a physical
channel across devices, acquire a lease through `SharedCANBusPool::acquire(key, factory)`
and pass it to `Session::lease()`.

The Pool shares equal keys and rejects conflicting FD/bitrate configurations for the same
physical channel. Callers must normalize identity fields such as endpoint and library path;
the Pool does not automatically recognize path aliases as the same hardware. If the last
lease's `close()` fails, that holder retains the resources and must retry. New acquisition
then throws `FaultedError`. `Session::close()` can also retry failed resource release.
A closed session cannot be reopened.

If the last lease is destroyed in a receive or fault callback, the Pool delegates closing to
an in-process cleanup thread rather than waiting for the current dispatcher callback.
Before process exit or destruction of related external resources, call
`CANBusLease::drain_deferred_cleanup()` from an external thread. If it throws `FaultedError`,
resolve the backend-close failure, then call `retry_deferred_cleanup()` and
`drain_deferred_cleanup()`. `pending_deferred_cleanup()` reports pending work for diagnostics.
Failed tasks retain the original Pool record, physical key and retry ownership; records
are not removed and keys are not replaced to suppress an error. Do not call these drain/retry
interfaces from receive or fault callbacks.

`share()` / `lease()` / `borrow()` reuse one Session, not independent logical connections.
Any sharing caller's `close()` invalidates all users' routes and releases every lease held
by that Session. `share()` does not itself stop the external bus, but other devices cannot
continue using the closed Session. Assign one owner to close the session; an individual
device should close only its own inbox and cancel its own endpoint. A closed Session still
alive in the registry rejects further share/borrow calls; release the old object before
creating a replacement.

A factory may reenter `share()` on the same thread for a different registry key, such as
another `ManagedCANBus`. Reentry for the same key, or a detected wait cycle between
initialization threads, throws `ValidationError`. A factory must not wait for another
thread's `share()` on the same unfinished key: the registry cannot observe or interrupt
that application-internal wait.

A backend's `close()` and destructor must not call `Session::share()`, `lease()` or
`borrow()`. Registry entry during Session cleanup throws `ValidationError` for both same
and different keys, preventing a cleanup thread from waiting on its own tombstone or
forming a cross-key cleanup cycle.

After the final Session reference disappears, the registry retains that generation's cleanup
marker until subscriptions, writer, leases and owned bus complete synchronous or deferred
cleanup. Same-key `share()` / `lease()` / `borrow()` waits for cleanup and does not publish
a replacement early. Receive/transmit callbacks cannot wait for cleanup they themselves
triggered; such calls throw `ValidationError` and must be retried from an external thread.

Registry nodes, the coordination mutex and wait graph remain reachable for the process
lifetime so static Session owners can release during process exit. After cleanup, nodes
retain only weak references and coordination state, not Sessions, buses or driver resources.
Nodes for distinct `ManagedCANBus` / dispatcher object identities are not deleted. Long-lived
processes should reuse managed bus/dispatcher objects; creating new objects or Pool keys
does not resolve close failures or configuration conflicts. This state is not written to
disk, is not a persistent queue or acknowledgment record, and is reclaimed by the OS at exit.

## Receive routing

`subscribe_routes({{id, extended}, ...}, capacity=512)` atomically registers a group of keys
sharing one bounded inbox. Each key belongs to at most one open inbox; standard and extended
identifiers are distinct keys. `update_routes(inbox, keys)` atomically replaces a group and
preserves the original registration on conflict.

- `receive_until(steady_clock_deadline)` waits for a frame; timeout returns `std::nullopt`.
- `try_receive()` does not wait for a frame to arrive; it takes the inbox mutex and returns
  a queued frame, or an empty optional if none is queued. A closed inbox throws `ClosedError`.
- A full queue rejects new frames and increments inbox `overflow` and Session `overflows`;
  it does not overwrite queued frames.
- `close()` clears the queue, releases keys and wakes waiters. Subsequent receives throw `ClosedError`.
- Each frame carries generation, sequence and receive time. Session checks FD/BRS, identifier
  and payload validity; the upper layer validates device-specific reply formats.

## Batches, receipts and cancellation

`MessageBatch` contains frames, inter-frame `gap` and pre-first-frame `initial_delay`.
`submit(batch, options)` enqueues and immediately returns `transport::SendReceipt`; a full
queue throws `QueueFullError`. Capacity counts waiting batches, not the executing batch.
There is no global bound on frames per batch or total route groups, so a bounded queue
does not bound total memory. Applications must also limit batch sizes, submission rate
and total routes. `send(batch, options)` submits then waits through the same scheduler.

Submission validates the batch before enqueueing. An already-cancelled token, an already-expired
deadline or closed session throws synchronously, without returning a receipt. Backend-specific
validation can still fail later through the returned receipt.

The default policy is `SendPolicy::fifo`. Higher priority is scheduled first; waiting time
contributes to priority and equal ranks retain order. `SendPolicy::latest` requires nonempty
endpoint and family, replacing only waiting latest batches in that group. Replaced receipts
become `superseded`; executing batches are not replaced. Session and legacy dispatcher/managed-bus
sends share the physical batch lock, excluding other transmissions even during batch gaps.
Priority cannot preempt in-flight batches; large batches or long gaps/initial delays block
other devices. Endpoint/family are caller-defined scheduling keys, not automatically CAN IDs.
Use explicit endpoints: `cancel_endpoint("")` matches unnamed tasks and does not identify
one particular device. Backends supporting native batching may combine sends with no gap.
MiniCANFD accepts at most 100 frames in one native batch; exceeding this with `gap=0`
completes the receipt as failed. See [MiniCANFD](minicanfd.md). Other backends may impose
driver-specific limits; Session's lack of a global frame limit does not imply unlimited backend capacity.

`SendOptions.deadline` is an absolute monotonic-clock time covering queueing, transmit-lock
waiting and inter-frame waits. `cancellation` cancels unfinished tasks; `cancel_endpoint(name)`
cancels both queued and in-flight tasks for that endpoint. A driver write already entered
may complete. Cancellation arriving after final backend acceptance does not replace
`dispatched`. `CancellationSource` and its tokens share one one-way state; repeated `cancel()`
is allowed but cannot undo cancellation or interrupt synchronous vendor calls without a cancel API.
The deadline is checked before backend calls and during scheduler waits, not after the final
successful backend call. That call can finish after the deadline and still yield `dispatched`;
a caller waiting only until the deadline may already have received a wait-timeout exception.

Receipt `result()` does not wait for task completion; it reports outcome, generation, stage times, accepted-frame
count and exception. `wait()` / `wait_until()` rethrow saved errors on failure, expiry,
cancellation or replacement. A wait timeout does not cancel the task; the same receipt remains
queryable. `SendReceipt::result()`, `wait_until()`, `completed()` and `RouteInbox::health()`
are `[[nodiscard]]`; explicitly cast intentional discards to `(void)` for `-Werror` consumers.
`sent_frames` counts known backend acceptances. `current_frame_uncertain=true` means that
the failing frame or native batch may have been partially delivered; do not automatically replay it.
Backend acceptance establishes neither CAN acknowledgment nor device execution. CANable SLCAN
confirms only that the local serial driver accepted the bytes.

`can::SendReceipt` belongs to legacy `ManagedCANBus::queue_send()`, provides completion/error,
`wait()` / `wait_for()`, and is distinct from `transport::SendReceipt`.

## Recovery, close and diagnostics

Save `session->health().generation` and pass it to `recover(observed_generation)`.
Concurrent calls with the same old generation share one successful reopen; failed recovery
can be retried. Recovery closes old inboxes, aborts old unfinished batches and does not
replay control frames. Completed receipts remain readable. Register routes again and
recheck device identity after recovery. `own()` / `borrow()` has no factory for rebuilding
the connection and cannot perform physical recovery.

`health()` reports queues, frame/byte counters, unrouted and invalid frames, overflow,
recoveries and latest error. `transport_health()` adds underlying transport state. Both read
cached state without probing hardware. Closing wakes receivers and waits for writer exit;
elapsed time remains constrained by in-flight driver calls.

Synchronous `close()` and `recover()` require an external thread; calls from receive or
transmit callbacks throw `ValidationError`. Releasing the final Session reference inside a
callback delegates destruction to a cleanup thread. A borrowed dispatcher must remain alive;
before destroying it, call `Session::drain_deferred_cleanup()` from an external thread.

## Recovery example

This fragment uses an existing recoverable `session`; it is not a standalone program.
Each factory call must create a new usable backend, not return the same closed adapter.

```cpp
const auto observed = session->health().generation;
const auto generation = session->recover(observed);
auto inbox = session->subscribe_routes({{0x28, false}});
// Recheck device identity/state here; do not automatically replay old commands.
(void)generation;
```

After a successful reopen, other callers presenting the same old observed generation receive
the current generation without reopening again. Failed recovery does not increment successful
`recoveries` and may be retried. Explicit close is final and prevents recovery. Backend
`write()` / `write_batch()` has no deadline parameter; Session cannot forcibly interrupt a
driver call already entered. `recover()` and `close()` may wait for it; the send deadline is
not a hard deadline for the entire lifecycle operation.

After an exception, `result()` exposes outcome and accepted-frame count. Receipt `accepted_at`,
`claimed_at`, `completed_at` and received-frame `received_at` use monotonic time, not Unix
timestamps. Querying or waiting on a default-constructed empty SendReceipt throws `ValidationError`.

An internal receive-routing or send-worker failure leaves the Session terminally
closed even while its physical backend remains connected. Health refresh cannot
revive it, and `recover()` rejects it. Close and destroy the Session, then create a
new one; physical recovery alone cannot restart a terminated writer.
