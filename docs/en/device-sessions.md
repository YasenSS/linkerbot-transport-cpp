# Logical device sessions

`Session::attach_device(identity, DeviceOptions)` creates a logical device on the existing Session writer. Attach devices before legacy sends or route registration. Raw Session sends are rejected after the first device attaches. Existing Session constructors, default scheduling, receipt methods and deadline policies retain their behavior.

A device binds its endpoint and physical generation to submissions. Callers cannot override these fields. A closed handle stays closed; attaching the same identity creates a new registration. Recovery invalidates previous handles. `channel_instance` distinguishes connections recreated with the same backend generation.

## Shared physical ownership

Include `<linkerbot/transport/runtime.hpp>` to use `TransportRuntime`. Runtime instances in the same linked library share a channel registry and `SharedCANBusPool`. Each channel owns one physical pool lease and one Session; logical devices hold counted leases on that owner.

`open_device(ChannelSpec, DeviceSpec, factory)` requires an explicit configuration fingerprint for backend settings not represented by `CANBusKey`. Reusing a channel requires identical fingerprints and Session scheduling options. The pool also checks incompatible physical modes. Factories execute outside the registry lock; concurrent first opens share initialization, and cyclic initialization is rejected.

Closing one device leaves its peers connected. The last device closes the Session and releases the physical lease. Explicit close failures retain the registration and connection owner for retry. Destructor cleanup is best effort and uses the existing physical lease cleanup mechanism; applications should explicitly close and handle errors.

`recover_channel(key, observed_generation)` coordinates physical recovery. Concurrent callers join an in-progress attempt; successful recovery is reused for an older observed generation. Failed attempts may be retried. Existing handles remain invalid and unfinished work is not replayed. A device registered with `ChannelResetPolicy::kRequireDetach` prevents physical reset until it detaches. This supports SDKs whose recovery requires rebuilding their device state.

`rebind_device(key, previous, replacement, deadline)` pins the connection, closes the old logical registration, and attaches a replacement without reopening the backend. SDK identity checks, cache invalidation and initialization remain the caller's responsibility. This operation does not replace the existing Python hand recovery behavior, which requests physical recovery.

## Admission and scheduling

An `AdmissionDomain` is immutable accounting identity, not another queue. Share a domain object among devices sharing a budget and set it in `DeviceOptions::admission_domain`. FIFO capacity counts batches or frames; latest capacity counts waiting slots. Claimed work does not consume waiting capacity. Both domain and Session limits apply. Failed replacement admission leaves the previous latest job intact. Reusing a domain on separate physical Sessions does not combine their accounting.

`SessionOptions::domain_round_robin` selects an eligible domain by least recent service before ranking its jobs. It is opt-in for mixed deployments. FIFO work retains device ordering. `max_batch_frames` bounds a claimed atomic batch; zero retains the legacy unlimited batch size. Deployment configuration must choose aggregate budgets and batch limits explicitly.

`SendOptions::pending_age_scope` defaults to `PendingAgeScope::kDefault`: it resolves to the device endpoint through DeviceSession and to the whole Session through legacy Session entry points. Explicit `kSession`, `kEndpoint` and `kAdmissionDomain` selections are honored; domain scope requires an admission domain. Checks include queued, writing and final-cooldown work in the selected scope. Existing enum values and default admission behavior are unchanged; the default field value is now the added `kDefault` enumerator. Code that explicitly set `kSession` on a device now checks the whole Session rather than being rewritten to endpoint scope.

Device `send_period` measures each successful frame from dispatch attempt entry. It disables native batch merging, retains the atomic boundary within a batch, and releases the writer during the final cooldown so another device can send. The same Job remains owned until completion; drain and pending-age accounting include it. Device and legacy Session periods cannot be combined.

`max_consecutive_send_errors` faults only that device. A successful frame resets its streak; peer success does not. Reaching the threshold fails its waiting jobs and closes its inboxes with a fault reason. It does not directly fault the physical Session.

The Python source baselines use a shared hand domain of 256 request batches and 64 latest slots with 50 ms aging, and an arm domain of 2000 frames per arm with a 300 microsecond period and error threshold 10. These values are not new C++ defaults. Applications must provide resolved deployment profiles and provenance before treating them as original-business configuration.

## Deadlines and progress

`DeadlinePolicy::kDispatchStart` records claim, then checks the deadline once at dispatch entry. Expiry produces zero transmitted frames with claim metadata. Once dispatch begins, that queue deadline does not truncate physical lock waiting or the batch suffix. Explicit cancellation and physical failure still apply. Existing `kExecution` and `kStartOnly` retain their previous boundaries. Caller receipt waits have independent deadlines and do not cancel work.

`sent_frames` reports the known backend-accepted prefix. `dispatch_completed_frames` separately exposes the prefix whose pacing has completed. Pending final cooldown may therefore show full physical acceptance with incomplete dispatch progress. Ending final cooldown during close preserves a known dispatched result.

## Device close and owned operations

`begin_operation()` registers thread-bound SDK work before close begins. The move-only `DeviceOperation` permits that owner to submit restoration work after the public device gate closes. It creates no worker or payload queue. Other threads cannot submit through the operation, and its owning thread cannot synchronously close the device while still holding it.

`close_until(deadline)` closes public admission, waits for registered operations, cancels waiting jobs, drains claimed work without truncating it, closes device inboxes, and releases the logical channel lease. Timeout retains the registration for retry. Backend close calls can exceed the waiting deadline. Close itself sends no motion, disable, calibration or brake commands; model-specific restoration remains in the SDK.

## Routes and diagnostics

`subscribe_routes()` owns exact ID/standard-versus-extended-ID pairs. `subscribe_rules()` owns masked ID ranges with the same extended-ID flag, including extended A7 Lite routing. Rules preserve the original frame and record their logical key separately. Overlap is rejected using actual wire ranges, including exact-versus-masked conflicts. Group registration rolls back after allocation failure. Masked routes cannot use the legacy global extractor.

`observe_rules(rules, capacity)` provides bounded read-only discovery observation. It does not take route ownership or ordinary receive-budget slots; a full observer drops its own copy. Normal owners receive their frame first. Discovery requests still use the same device writer. Observation inbox health records its own backlog and drops.

`max_route_keys` bounds registered rules including observations; `max_pending_rx` bounds the ordinary receive backlog. Zero preserves legacy unlimited behavior. Inbox overflow drops the new frame. `compatibility_sequence` increments after matching and before queue offer, so overflow leaves gaps; it resets on physical invalidation. The existing C++ sequence remains unchanged. Device fault reasons are retained by inboxes; legacy inbox close exceptions retain their existing contract.

Channel health includes route count, receive backlog/high-water mark, routed frames, stale frames, registration quota rejections and close drops. Device health reports admission state, generation, instance, registered operations, send-error streak, queued FIFO frames/latest slots, in-flight jobs and oldest owned work.

This transport implementation does not provide Python bindings, Python Future execution, protocol transactions, SDK migration or motor safety policy. Native receive timestamps and backend-specific echo options are described in [CAN metadata](can-metadata.md). Arbitrary Python backend plugins are not implied by the native backends. Software tests do not establish hardware or firmware compatibility; deployments need the device/driver/topology acceptance checks described in the migration plan.
