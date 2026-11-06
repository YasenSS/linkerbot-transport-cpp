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
queue throws `QueueFullError`. By default, capacity counts waiting batches, not the executing batch;
SDK options below can instead count FIFO frames and separate latest slots.
By default there is no global bound on batch frames or registered route keys.
Configure `SessionOptions::max_batch_frames`, `max_route_keys` and `max_pending_rx`
to add those limits; zero retains the unbounded legacy behavior. Queue capacity
alone does not bound total memory. Applications must also bound submission rate
and inbox capacities. `send(batch, options)` submits then waits through the same scheduler.

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
driver-specific limits; a zero `max_batch_frames` limit does not imply unlimited backend capacity.

`SendOptions.deadline` is an absolute monotonic-clock time. The default execution policy covers queueing, transmit-lock
waiting and inter-frame waits. `cancellation` cancels unfinished tasks; `cancel_endpoint(name)`
cancels both queued and in-flight tasks for that endpoint. A driver write already entered
may complete. Cancellation arriving after final backend acceptance does not replace
`dispatched`. `CancellationSource` and its tokens share one one-way state; repeated `cancel()`
is allowed but cannot undo cancellation or interrupt synchronous vendor calls without a cancel API.
The deadline is checked before backend calls and during scheduler waits, not after the final
successful backend call. That call can finish after the deadline and still yield `dispatched`;
a caller waiting only until the deadline may already have received a wait-timeout exception.

While a receipt is pending, `result().sent_frames` can advance as individual writes are accepted.
It counts only known acceptances; a failing native batch can still have uncertain partial delivery.
Progress alone does not advance `ChangeSignal`; use a bridge timer when intermediate updates are needed.

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

## SDK scheduling options

Existing constructors, `submit()`, `send()`, `cancel_endpoint()` and receipt waits retain their
signatures and default behavior. The options below prepare a binding to use this Session's
single writer; this library does not supply a Python binding or migrate hand/arm SDK code.
The additions require rebuilding consumers with matching headers and libraries; they do not
establish a new binary ABI guarantee.

Call `configure(SessionOptions)` before the first accepted send. Configuration can be revised
while no send has been accepted. Recovery does not reset the accepted-send counter, so an
earlier accepted send continues to prevent reconfiguration. `options()` reads the active settings. For a shared Session, its owner must choose one configuration and check
that all users agree before accepting traffic.

Defaults remain one queue of `tx_capacity=2000` waiting batches, `endpoint_fifo=false`,
`aging_quantum=10 ms`, no send period and no terminal consecutive-send-error threshold.
With `separate_policy_capacity=true`, FIFO admission uses `fifo_capacity` and latest admission
uses `latest_capacity` instead of the combined `tx_capacity` limit. `fifo_unit` selects
`CapacityUnit::kBatches` or `CapacityUnit::kFrames`; latest capacity always counts waiting
`(endpoint, family)` slots. The claimed batch is excluded from these waiting capacities.
A FIFO batch is accepted in full or rejected in full. Replacing a latest slot does not consume
another slot. Applications must still bound frame count per batch and route count.

For hand-style protocol requests, use one FIFO unit per request batch. For arm-style joint
commands, use `CapacityUnit::kFrames` so a seven-frame group needs seven available FIFO units.
Do not split a group into seven separate `submit()` calls: that loses atomic admission.

`endpoint_fifo=true` considers only the first waiting FIFO batch for each endpoint. Among
eligible batches it uses priority plus age, then earliest deadline, least recently served
endpoint, and queue order. It does not prevent latest work from competing with that endpoint's
FIFO head. Set `aging_quantum=50 ms` when matching the Python arbiter's default aging interval;
leaving it at 10 ms preserves the C++ default. Scheduling never preempts a claimed batch.

`replace_latest_if_pending(batch, options)` requires latest policy and nonempty endpoint/family.
Under the same queue lock as claim, it replaces an existing unclaimed slot or returns
`std::nullopt` without creating work. This differs from ordinary `submit(..., latest)`, which
may create a new slot after the previous batch was claimed. The replaced receipt becomes
`superseded`. Legacy `wait()`/`wait_until()` still rethrow its saved `CancelledError`;
`wait_result_until(deadline)` waits for completion and returns any terminal `SendResult`
without rethrowing its saved error. Only the wait timeout itself throws. A Future adapter can
therefore translate `superseded` into a value and translate other outcomes according to its
public exception contract.

## Admission, deadline and endpoint drain

Unsupported `SessionOptions::fifo_unit`, `SendOptions::policy` and
`SendOptions::deadline_policy` values throw `ValidationError` before configuration assignment
or send admission. Rejection leaves existing configuration and queued latest work intact.
Supported enumerators and default behavior are unchanged.

`SendOptions.expected_generation`, when present, is checked under the admission lock. A stale
value throws `ClosedError` without returning a receipt. This prevents an old device actor from
submitting into a newly recovered connection. It does not verify device identity.

`max_pending_age` is an optional positive duration. Admission rejects with `TimeoutError` if
the oldest accepted waiting or claimed task has reached that age. This is distinct from a
new task's deadline. Apply it to motion targets when required, rather than automatically to
lifecycle/configuration writes. `health().oldest_pending_age` includes claimed work; the
FIFO/latest pending gauges exclude it. `PendingAgeScope::kDefault` selects the whole Session
for these entry points and the local endpoint for DeviceSession. Explicit scope selections
are preserved; see [device sessions](device-sessions.md) for shared-domain behavior.

`deadline_policy=DeadlinePolicy::kExecution` retains checks while waiting for the physical lock
and between frames. `DeadlinePolicy::kStartOnly` retains queue, lock and initial-delay checks,
then stops checking the send deadline once the first backend write begins. The accepted
suffix can finish after that deadline. Cancellation and lifecycle failure still apply. A wait
using the original deadline can time out before a later successful completion; it does not
cancel the task. `send()` continues to wait only until `SendOptions.deadline`, regardless of
policy. Use the returned receipt from `submit()` to observe eventual completion.

`stop_endpoint(name, deadline)` closes admission for that endpoint, removes its unclaimed
tasks and waits for its claimed batch without cancelling that batch. It returns the number
of removed tasks. The wait releases the Session state lock, allowing other endpoints to
progress. Timeout leaves the admission gate closed and the claimed owner intact; retry the
drain rather than creating a replacement writer. `resume_endpoint(name)` reopens the gate
only after no claimed batch for that endpoint remains and the Session is open. Endpoint
names must identify devices consistently; an empty name matches unnamed tasks.

The existing `cancel_endpoint(name)` remains nonblocking and may interrupt the unsent suffix
of a claimed batch. It does not close admission. Keep these two operations distinct. Call
`stop_endpoint()` from an external thread, not a receive/backend callback. Closing one device
should drain its endpoint and close its inbox; only the shared owner closes the Session.

`send_period` is an optional minimum successful-frame period measured from the native
physical write's start, including backend duration and a remaining cooldown. It applies
across Session batches using the same dispatcher and disables native batch merging when
nonzero. It is separate from `MessageBatch::gap`, which is an additional inter-frame wait.
It does not include Python-side encoding, locks or other frontend time before native dispatch.
Final-frame cooldown cannot change known backend acceptance into a cancelled/expired result.
Legacy direct `send()`/`send_batch()` entry points are not a way to share this pacing contract.

A nonzero `max_consecutive_send_errors` makes that many consecutive failed sends terminal
for the Session. Each known successful frame resets the counter, including a successful prefix
before a later failure; unknown partial native-batch delivery cannot reset it. The default zero leaves this
policy disabled. Terminal Sessions require closing and recreation, rather than `recover()`.
Backend-thrown timeouts count toward this threshold while retaining the existing `expired`
outcome and `current_frame_uncertain=true`. Queue/lock/pacing expiry and cancellation do not
count as backend write failures.

## Routing and bridge notifications

`set_route_key_extractor()` accepts a native function that maps a received `can::Message` to
a `RouteKey`. Configure it before the first successful route registration or accepted send. Unmatched
unsolicited RX before registration does not freeze it; recovery does not unfreeze it. An empty
function restores exact ID/standard-versus-extended-ID routing. The function runs under the Session state lock:
it must be fast, nonblocking and must not reenter the Session. It should not invoke Python
or perform I/O. Exceptions or invalid returned keys increment `route_key_errors` and drop
only that frame. The delivered `message` retains its original ID, flags and payload;
`RoutedMessage::route_key` records the extracted key.

`received_order` and `SendResult::claimed_order` share a Session-local event counter.
Compare them only within the same Session and retain generation checks. The RX `sequence`
remains a separate receive-only counter. These values order software claim and routed RX
events independently of clock resolution; they cannot establish that a device executed a
command or distinguish every delayed reply in a protocol without request tokens. Recorded
receive times describe native routing, not later consumption by a binding.

`Session`, its inboxes and receipts expose a shared `ChangeSignal` through `notification()`.
The signal contains only a revision and a condition variable. RX admission, overflow,
route closure, claim, receipt completion and generation invalidation can advance it; changes
can coalesce, and one inbox may wake because another inbox or receipt changed. It does not
execute application callbacks or add a payload queue. An external runtime bridge should read
the revision before inspecting its state, then wait using that revision if no work is ready:

```cpp
auto signal = session->notification();
const auto observed = signal->revision();
// Inspect inboxes, receipt snapshots and lifecycle state here.
// If none is ready, wait for a change or the bridge's next timer deadline.
const bool changed = signal->wait_until(observed, next_deadline);
(void)changed; // Always inspect state again after waiting.
```

Keep the signal alive with its shared pointer. `wait_until()` returns false on its own wait
timeout; `Deadline::max()` performs an unbounded wait. A terminal state must be handled before
waiting again, because a retained signal does not continually advance after Session closure.
Route health also records capacity, delivered frames and queued frames dropped on close.

A migrated SDK should submit directly from its binding into Session. Do not wrap this with
a Python `TransmitArbiter`, and do not forward Session work into `ManagedCANBus::queue_send()`;
that would add another queue/claim point. Keep protocol actors, ACK correlation, response
quarantine, target validation and device caches in the upper SDK. Those are not a second
physical send scheduler. Existing legacy entry points remain available for other consumers;
a single-queue integration must keep them out of the active Session path. Offline regressions
cover software contracts, not adapter, firmware, driver timing or hardware acceptance.
