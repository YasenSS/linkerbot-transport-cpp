# Changelog

English · [简体中文](CHANGELOG.md)

## Unreleased

- Synchronize source guides, backend contracts, API comments and validation inventory with native receive metadata, explicit frame formats and per-device Runtime ownership. Retain fixed-version evidence and distinguish Unreleased APIs from published SDKs.

- Add explicit per-message classic/FD selection while preserving backend defaults, native receive timestamp metadata and opt-in SocketCAN/PCAN receive-own configuration. Reject unsupported vendor/SLCAN echo opt-in, preserve driver counters without epoch conversion, and add encoding, rollback, metadata routing and deployment-evidence regressions. No hand/arm integration or physical hardware acceptance is included; consumers must rebuild with matching headers and library.

- Reject unsupported FIFO capacity units, send policies and deadline policies with `ValidationError` before configuration changes or send admission. Preserve valid defaults and existing signatures; add configuration, queued-latest, device/owned-operation and installed-consumer regressions.

- Honor explicit whole-Session pending-age checks on device submissions. Add `PendingAgeScope::kDefault` to retain endpoint-local device defaults and whole-Session legacy defaults without changing existing enum values or signatures. Add scope, owned-operation, replacement, cooldown and installed-consumer regressions.

- Add shared runtime channel ownership, device rebind and coordinated recovery, owned-operation close permits, per-device pacing and failure limits, masked routes and bounded discovery observation. Keep legacy C++ defaults and use the existing Session writer. Add lifecycle, allocation rollback and installed-consumer regression coverage.

- Add logical device handles with owned inboxes, generation-bound submission and retryable endpoint drain on the existing Session writer. Add shared admission domains and opt-in endpoint-local age checks; preserve legacy defaults.

- Add opt-in Session scheduling for SDK bindings: weighted FIFO and separate latest capacity, endpoint FIFO/fair selection, generation/age admission, start-only deadlines, native per-frame pacing and a consecutive-send-failure threshold. Preserve existing signatures and defaults.
- Add atomic conditional latest replacement, endpoint admission/drain gates and value-returning terminal receipt waits. Existing cancellation and throwing waits retain their behavior.
- Add coalesced state-change signals, route-key extraction with original frame preservation, shared claim/RX event ordering and queue/route diagnostics. Document the single-writer integration boundary and add offline regressions; no Python binding or hardware acceptance is included.

- Reject stale health snapshots across Session lifecycle changes so concurrent recovery cannot roll generation or admission state back.

- Fix syscall fault injection on Focal/glibc 2.31 by wrapping the legacy `__xstat` entry point; run the regression in native Focal x86_64/ARM64 CI.

## 0.3.3 — 2026-11-06

- Use `linkerbot-transport` for the package and `linkerbot::can` for the CAN library. Retain legacy package/target/options, byte-identical comm archives and share paths; update build, package and consumer validation and both documentation editions. Published release assets are unchanged.

- Add optional serial byte streams and a Modbus RTU client (03/04/06/10), separate CMake targets and package component metadata. These modules are not in published v0.3.2 packages.
- Allow RTU backend cancellation diagnostics to query session health without deadlocking. Reject Linux RS485 configurations when the driver clears requested direction flags.
- Keep CAN Sessions terminal after internal routing or writer failures, rather than reopening them during health refresh. Require closing and recreating these Sessions.
- Add Linux syscall and allocation-failure regressions for serial, CAN sessions and resource rollback; update module and build documentation.

- Fix unbounded Session receipt waits on Focal/GCC 9 by using the untimed condition-variable wait for a maximum deadline. Preserve finite deadlines and saved errors; add native Focal x86_64/ARM64 Session regression jobs.

- Require Linux production line coverage above 99% before rounding across `src/` and `include/`, without excluding production sources. Retain CAN/SLCAN and native loader regression checks.
- Require native Focal quality checks for the exact release source. Seal the original tag-push artifact identities and bytes; draft promotion reuses that explicit validated run without rebuilding or replacing published assets.

## 0.3.2 — 2026-11-04

- Produce eight native Ubuntu SDK archives (20.04/22.04/24.04/26.04 × x86_64/ARM64) and four native Windows archives (x64/ARM64 × Debug/Release), rather than treating baseline consumption tests as distinct distribution builds.
- Validate the exact 12-target release set, archive machines and distribution identities before assembling 26 uploaded assets. Preserve Focal cross-distribution and Noetic checks and leave existing releases unchanged.
- Correct the wrong-machine Windows DLL fixture to choose the opposite supported architecture, retaining loader rejection on native ARM64 as well as x64. Production loader behavior and public APIs are unchanged.

## 0.3.1 — 2026-11-04

### Builds and packages

- Build native x86_64 and ARM64 Release SDKs with the Ubuntu 20.04/GCC 9 baseline. Validate the same packages in Ubuntu 20.04/22.04/24.04/26.04 userspaces; PR checks cover the oldest and newest endpoints, while full runs cover all four.
- Enable PIC for the static SDK and add relocated executable/shared-library consumers. Add real Noetic catkin node/nodelet FakeTransport tests on Focal, including unload; the core library gains no ROS dependency.
- Include build distribution, compiler, probed standard library/dual ABI and configuration in new Linux package identities. Fix configuration-specific standard-library detection and multi-config Debug/Release package collisions. Reject unsupported Windows package architectures instead of mislabelling them x64.
- Use an absolute CPack output directory on Focal and reject empty SDK archives even when their SHA256 matches. Validate all ELF archive objects, metadata and unwanted runtime/test components.
- Cache Linux compilation and the verified-source minimum CMake installation without removing existing quality gates. No measured speedup is claimed.
- Add exact-source release validation and opt-in draft creation from the same tested Linux/Windows artifacts, with a manifest, checksums and download verification. Automatic publication and replacement of existing assets remain disabled.

### Compatibility and validation limits

- C++17, public APIs, CMake target names and installation layout remain unchanged. New Linux archive names require updates to filename-based download scripts. Select matching architecture, standard library/ABI and Windows CRT; rebuild dependent applications as needed.
- Containers validate userspace integration, not native kernels, vendor drivers or hardware. Noetic is upstream EOL and is checked only on Focal. Image digests are recorded per run, but mutable apt inputs mean byte-reproducible builds are not promised.
- Preserve the existing 0.3.0 tags, assets and historical validation records. No protocol or persistent-data migration is required.

- Correct post-release support status, add bilingual 0.3.0 download and verification instructions, and link the README badge to the released commit's full report. These documentation changes do not replace the signed tag or published packages.

## 0.3.0 — 2026-11-03

### Transport and Session

- Provide the standalone C++17 communication library with SocketCAN, MiniCANFD, PCAN Classic/FD and native serial SLCAN backends. SLCAN supports Lawicel Classic and an explicitly selected CANable FD profile; it remains experimental.
- Add Session ownership, shared buses, receive routing, bounded outgoing queues, asynchronous receipts, deadlines, cancellation, health reporting and recovery. Transport acceptance, CAN acknowledgment and business acknowledgment are distinct outcomes.
- Preserve the `linkerbot-comm` CMake package, `linkerbot::comm` target family, library names and installation directories after the repository rename to `linkerbot-transport-cpp`.

### Reliability fixes

- Complete send-error diagnostics and health updates before publishing a Session receipt. This prevents concurrent access to the same exception by the writer and receipt consumers. Diagnostic allocation failure preserves the original outcome, exception, accepted-frame count and uncertainty; a gated-exception regression checks publication ordering.
- Correct Session registry generation and initialization handoffs, including waiters racing replacement owners and failed initialization attempts. Keep cycle detection and cleanup ownership intact.
- Fix process-exit destruction ordering for static shared/borrowed Session owners. Deferred cleanup retains the owner until cleanup completes; failures do not justify discarding resource claims.
- Preserve cancellation barriers, self-unsubscribe behavior and in-flight callback waits. If preparing unsubscribe bookkeeping fails to allocate, the original subscription remains active and can be retried.
- Retain retryable ownership after physical close failures. Recovery invalidates old routes and unfinished work without replaying uncertain commands.
- Handle exceptions at the installed-consumer program boundary. An injected allocation failure now reports an error and exits with status 1 instead of aborting; the normal consumer path remains tested.

### Performance and quality

- Cache immutable subscriber membership between changes. The recorded ARM64 workload eliminates the steady nonempty snapshot allocation and has lower measured timing medians. Empty registries and cold membership rebuilds cost more; no idle-wakeup improvement is claimed. Keep the original copying `snapshot()` API and lifecycle semantics. See the [measurements and raw evidence](docs/en/performance.md).
- Review all 156 baseline report diagnostics, distinguishing confirmed runtime failures, intentional contracts and analyzer limitations. Make optional access explicit, preserve fixed vendor ABI signatures and throwing lease-move behavior, and avoid an unnecessary configuration copy without changing initialization order.
- Add a Catch2 model for Cppcheck with 29 static probes, two real/model runtime comparisons and a literal-registration guard. Test bodies and assertion operands remain analyzed.
- Require complete, finding-free A+ analysis in CI without lowering rule groups, scoring or source coverage. The verified report scores 100/100.

### Builds, packages and documentation

- Add Linux amd64/ARM64 Debug/Release regression, a minimum-CMake integration check, Clang 18/libc++, ASan/UBSan and TSan matrices, repeated lifecycle regressions, and an aggregate production line-coverage gate of at least 95%. Local C++ validation uses ARM64 Linux.
- Generate Linux Release TGZ packages and Windows x64 Debug/Release ZIP packages through CMake/CPack, with adjacent SHA256 files and `build-info.json` recording the source commit, architecture, compiler, configuration and runtime. Verify relocated installed-package consumers. Vendor drivers, vendor libraries and test binaries are not distributed.
- Use the Windows v143 14.44 toolset, `/MD` for Release and `/MDd` for Debug. Debug packages require the development runtime; they are not end-user runtime packages.
- Split English and Chinese READMEs, complete the maintained bilingual guides, and include a directly runnable installed-package quickstart. Clarify that the core requires CMake 3.16+, tests require 3.19+, and examples using `ctest --test-dir` require 3.20+.
- Pin GitHub Actions to full commits, retain read-only analysis permissions, and publish the report badge under `.github/cppreportcard/` with source/run provenance. Historical hardware statements remain explicitly separated from current offline evidence.
- Update C++ Report Card Action to v1.0.2 for its revised badge rendering; retain the full-analysis A+ gate and existing analyzer rules.

### Compatibility, upgrade and rollback

- Rebuild applications and dependent SDKs against matching headers and libraries. The snapshot cache changes registry-containing class layouts; arbitrary binary ABI compatibility is not promised. The `[[nodiscard]]` receipt and health accessors may require explicit `(void)` casts for intentionally ignored results in warning-as-error builds.
- Pair this version with `linkerbot-hand-cpp 0.3.x`; the transport patch version must not be older than the hand SDK patch version. Match architecture, toolset and runtime when selecting a binary package.
- Stop command producers, settle or manually reconcile in-flight outcomes, close the existing owners, then switch the complete validated application/library/runtime combination. Rollback likewise requires a matching rebuilt or previously validated combination; it cannot undo device actions.
- There is no persistent schema migration or new log-field migration in the release preparation. Retaining application diagnostics, historical logs and acknowledgment records is an existing operational requirement, not a new cleanup step. The library has no durable queue and does not restore in-memory receipts after restart. Do not delete records or change keys to hide an uncertain outcome.
- Offline tests and an A+ report do not certify a particular adapter, vendor driver or firmware. Validate the actual deployment separately. See [operation and rollback](docs/en/operations.md).
