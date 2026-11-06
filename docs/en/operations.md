# Operation, upgrades, and rollback

English · [简体中文](../zh/operations.md)

[Documentation index](../README.md) · [Session lifetime](session.md) · [Diagnostics](troubleshooting.md)

## Runtime configuration and deployment

Pin the transport/hand versions, process architecture, compiler/CRT, driver, and firmware combination. Retain the source SHA and `build-info.json`. A successful Linux ARM64 build does not mean that an x64 vendor library can be loaded; verify vendor dependencies separately. Configure SocketCAN interfaces and bitrates through the operating system. Supply the SLCAN port and firmware profile explicitly; the library does not scan all ports or infer firmware.

MiniCANFD and PCAN select a library in this order: explicit configuration, their respective environment variable, then platform search defaults. The variables are `LINKERBOT_CANFD_LIB` and `LINKERBOT_PCAN_LIB`. They are read during creation/loading; changing the environment does not replace an existing connection. A recovery factory should capture verified configuration and library paths to avoid changing libraries during recovery. Empty environment variables are treated as unset; an explicitly empty path is rejected. See the backend guides and [build options](build.md) for other defaults.

The library provides no daemon, database, Redis dependency, automatic log files, persistent-volume requirement, idempotency-key storage, or acknowledgment records. If the application stores logs, device identity, calibration, command history, or business acknowledgments in files or a database, it must configure persistent volumes or remote storage, access permissions, and retention. Restarting the process does not restore in-memory inboxes, queued tasks, or receipts. They are not a durable queue.

Expose only the interfaces the container uses: a visible CAN network interface for SocketCAN, a specified serial port for SLCAN, or matching libraries/drivers for vendor backends. Passing the entire device directory and environment is not a general troubleshooting procedure. A service's shutdown grace period should cover normal driver completion and resource cleanup; an abnormal, non-cancellable driver call may exceed it.

## Sharing, shutdown, and failed cleanup

For independent device lifetimes, use `TransportRuntime` with one `DeviceSession`
per device. Closing a device releases its own work, subscriptions and counted lease;
peers remain connected, and the last device releases the channel. Retry ownership
and coordinated recovery are described in [device sessions](device-sessions.md).

For legacy callers sharing a raw Session, it is not an independent connection handle for each caller. Any sharer calling `close()` closes the entire Session, cleans up resources held by that Session, and invalidates the other sharers' inboxes. Ownership of an external bus or borrowed dispatcher does not transfer to the Session. When one device finishes, close its inbox and cancel its endpoint first; a common owner should close the Session last.

Shutdown order: stop producing application tasks → cancel endpoints no longer needed → check in-flight receipts and device state → close inboxes/Session → handle retryable cleanup errors → release borrowed dispatchers or external buses. Do not synchronously call Session `close()`/`recover()` from callbacks. Before destroying a borrowed dispatcher, call `Session::drain_deferred_cleanup()` from an external thread to wait for deferred cleanup.

If a vendor physical close or Exit operation fails, retain the original owner and retry. Do not clear resource tables, discard claims, or change a channel key to report a successful release. Backend deferred cleanup retains ownership only within the current process. It is not an on-disk recovery log and cannot prove that the driver released resources after the process was forcibly terminated.

## Disconnects and lost acknowledgments

Retain the error, generation, accepted-frame count, and uncertainty from the receipt, and stop new control tasks whose execution cannot be safely determined. A Session backed by a recovery factory can call `recover` with the observed generation. A successful recovery closes old routes and cancels unfinished tasks; it does not replay control frames. Register routes again, enumerate/check device identity and actual state, then resume application work.

A lost business acknowledgment does not mean the device did not execute the command. Manual checks may be needed when the current frame is uncertain, identity changes after a device restart, the driver blocks for a long time, or historical commands disagree with current state. The device protocol determines which commands can be retried idempotently. Changing request keys, relaxing identity checks, or deleting old acknowledgment records does not resolve uncertainty. The transport library does not guarantee exactly-once execution across processes or restarts.

## Upgrade order

1. Record the currently recoverable source/package, SHA256, compiler/CRT, driver/firmware, channel, and application configuration. Back up application logs, calibration, and acknowledgment records under their retention policy; do not delete them for an upgrade.
2. Validate the new transport in a separate build/install prefix. Run offline tests before rebuilding the matching hand SDK and application. A `0.3.x` hand SDK requires a transport SDK version at least as new as its patch version. Observe `SameMinorVersion` and rebuild the matching components.
3. Stop application writers and control producers, wait for/check in-flight operations, and complete cleanup by the original owner. Do not replace a DLL in a running process.
4. Switch the complete application, headers, static library, runtime, and configuration combination. Validate driver/firmware changes separately; source-level test success is not hardware acceptance for a new driver.
5. After startup, check the actual library path, package provenance, device identity, generation, reads, and bounded control operations. Retain acceptance logs. Integrate dependent PRs or SDK updates in dependency order, then validate downstream applications again.

## Rollback limits

Restore a previously validated application, hand SDK, transport, CRT, and configuration set; rebuild it or use the original matching artifacts. Replacing only headers or linking an old static library into a new binary is insufficient. Driver/firmware rollback support is vendor-specific. Stop new commands and complete resource cleanup before switching. Rollback does not undo device actions or restore an old generation.

The library has no persistent schema migration. If the application changes its record format, it needs its own compatible reader, manual checks, and rollback plan. Retain old logs and acknowledgments. Updating these documents or the library must not automatically clear existing diagnostic fields or historical logs.

## Delivery and release verification

Published downloads, checksum commands and documentation corrections are listed
in the [release guide](releases.md).

Use an independent branch, signed commits, and an English PR describing the changes, compatibility, reproduction commands, and Linux results. Check CI, review, and unresolved threads for the latest head. After merging, synchronize main, verify it again, and clean up task branches and temporary environments. A formal release also requires a Changelog update, signed tag, and the established artifact workflow. CMake/CPack currently build and package artifacts; they do not publish a GitHub Release automatically.

The Linux GCC Release jobs retain CPack TGZ archives and adjacent SHA256 files for
amd64 and ARM64. Windows jobs retain x64/ARM64 Debug/Release ZIPs and their SHA256 files.
Use successful runs for the exact commit named by the signed release tag. Do not
publish coverage-instrumented Linux Debug builds as Release packages. Check each
archive's `build-info.json`, binary architecture and relocated installed consumer
before uploading it. Publish the checked archives and a combined `SHA256SUMS.txt`
with English Release Notes; verify the downloaded Release assets again afterward.
The tag identifies the source commit, while checksums identify the uploaded bytes.

Before and after release, verify stable-release status, body, attachments, SHA256, source SHA, configuration, binary architecture, and installed-package consumption. This repository has no container-image publication/signing workflow; SDK ZIP/TGZ files do not carry a container-image signature. If the application distributes an image, verify its provenance and signature separately. Pushing a tag or uploading a CI artifact is not completion of a release.
