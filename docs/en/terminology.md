# Terminology and observable behavior

[Documentation](../README.md) · [简体中文](../zh/terminology.md)

Use the following terms consistently in guides, comments and issue reports.

- **Backend acceptance:** a successful `write()` or native batch call as interpreted by that backend. It does not establish CAN acknowledgment or device execution.
- **CAN acknowledgment (ACK):** acknowledgment at the CAN data-link layer. It does not identify the intended application device or confirm execution of a command.
- **Serial acknowledgment:** a firmware response to a serial command or frame. Lawicel and CANable profiles have different response behavior; see [SLCAN](slcan.md).
- **Application reply / business confirmation:** a response interpreted and correlated by the upper-layer device protocol. The transport library does not provide this correlation.
- **Send receipt:** software state for one scheduled batch. `dispatched` means all frames were accepted by the backend. A failed or timed-out operation can still have delivered frames.
- **Empty receive result:** a `read()` call returned no application frame. Besides a timeout, a backend may return early after filtering a frame; an empty result alone does not establish elapsed time, absence of traffic or disconnection.
- **Receive timeout:** the receive wait reached its configured timeout without returning an application frame. It does not by itself establish disconnection. Do not infer a timeout solely from an empty receive result.
- **Wait timeout:** the caller stopped waiting for a receipt. It does not cancel the scheduled task or an in-flight driver call.
- **Deadline:** an absolute time on `std::chrono::steady_clock`; it is not a UTC timestamp or a hard limit on an uninterruptible driver call.
- **Recovery:** an explicit attempt to rebuild an eligible connection. Success changes the generation and invalidates old inboxes; it does not replay commands or verify device identity.
- **Generation:** an in-process connection/session epoch used to distinguish states. It is not a persistent device identifier or a command sequence number.
- **Deferred cleanup:** in-process ownership retained for later resource release. It is not a durable queue or a recovery log.
- **Idempotent close:** repeated close calls are safe after successful cleanup. A failed cleanup may retain resources and require another attempt; it does not mean every call must succeed.
- **Offline regression / fixture:** a test using memory, a pseudo-terminal, a socket pair or a test library. It does not constitute hardware acceptance.
- **Supported target:** a platform/backend implementation target. **Verified result** refers to a specified commit, toolchain and test run. Neither term implies that every adapter/driver/firmware combination has been tested.

Describe measurements by their scope: aggregate production line coverage differs from per-file coverage, branch coverage and hardware validation. Describe rates as payload bytes or wire traffic explicitly, and distinguish configured timeout budgets from measured latency.

Keep API identifiers unchanged in both languages. Translate explanations, not enumerator names, error codes, environment variables or commands. When behavior differs by backend, state the backend and mode rather than generalizing a default to the entire library.
