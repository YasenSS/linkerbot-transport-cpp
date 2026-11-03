# Changelog

English · [简体中文](CHANGELOG.md)

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
- Require complete, finding-free A+ analysis in CI without lowering rule groups, scoring or source coverage. The verified report scores 100/100. Record individual decisions in the [diagnostic review](docs/en/quality-review.md).

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
