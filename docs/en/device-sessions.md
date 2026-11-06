# Logical device sessions

`Session::attach_device(identity)` creates a device handle on the existing Session writer. Attach devices before legacy sends or route registration. Once a device is attached, raw `Session::submit()` is rejected; use the device handles. The parent Session remains the explicit physical connection owner.

A device assigns its own endpoint and expected generation to every submission. Callers cannot replace those fields through SendOptions. Route inboxes belong to the device and close after its claimed batch drains. `close_until(deadline)` rejects further submissions, removes unclaimed work and waits for claimed work without truncating it. Timeout retains the registration and permits another close attempt. Other devices remain open. After successful close, the same identity can be attached again with a new endpoint; an old handle stays closed. Physical recovery invalidates existing handles, which must be closed and reattached.

An `AdmissionDomain` is an immutable accounting identity, not a queue. Share the same domain object among devices that share a budget. Set `SendOptions::admission_domain` on submissions. Its FIFO limit can count batches or frames; latest capacity counts waiting slots. Claimed work does not consume waiting capacity. Both domain limits and existing Session limits apply. Replacement releases the previous job's accounting only after successful admission. Domain objects reused on separate Sessions are accounted independently.

`PendingAgeScope::kEndpoint` restricts max_pending_age to the submitting endpoint's queued and claimed work. The default kSession retains the previous whole-Session policy. Device handles select the endpoint scope.

This is the initial ownership and admission layer. It does not implement the complete multi-device design: channel configuration management, per-device pacing/error policies, internal cleanup permits, composable masked routes, expanded health counters and Python bindings remain separate work. SDK motion restoration must finish before calling device close. Device close sends no disable, brake, calibration or motion commands and does not call parent Session close or recover.
