# Changelog

English · [简体中文](CHANGELOG.md)

## 0.3.0 (unreleased)

- Cache subscriber snapshots for dispatch when subscriptions are unchanged. ARM64 measurements show lower steady-state dispatch time and no snapshot allocation for nonempty registries; rebuilding after subscription changes costs more. Add a Linux benchmark harness, separate timing/allocation modes, raw results, and an ARM64 CI smoke check. No idle-wakeup improvement is claimed.
- Preserve the active subscription if allocation fails while preparing unsubscribe bookkeeping; add a regression that injects the failure. Cancellation and callback-lifetime contracts are unchanged.
- The snapshot cache changes the layout of registry-containing types. Rebuild downstream applications and libraries together; do not mix objects built against the old and new headers.

- Display the generated `.github/cppreportcard/` badge in both READMEs with snapshot provenance. Install the badge and notes, and generate current CI report artifacts in the same directory.

- Add full C++ Report Card analysis on Linux ARM64 with JSON, Markdown and SVG artifacts, read-only permissions, and the existing strict quality gates retained.

- Distinguish empty receive results from timeouts and filtered frames. Document synchronous Session submission errors and completion after a deadline. Add reproducible sanitizer commands and state the static-analysis scope for the independent quickstart.

- Complete the English and Chinese guides, contribution instructions, changelog and support policy. Align terminology for timeouts, receipts, acknowledgments, recovery and cleanup.

- Split the English and Chinese READMEs and add an independently buildable, runnable installed-package quickstart. Clarify CMake 3.16 consumption and modern CTest command requirements, and document Session receipt, recovery, and cleanup contracts beside the public declarations.
- Rename the repository to `linkerbot-transport-cpp`. Preserve the `linkerbot-comm` CMake package, `linkerbot::comm` target family, library names, and installation directory names for integration compatibility.
- Provide SocketCAN, MiniCANFD, PCAN Classic/FD, and native serial SLCAN backends. SLCAN supports Lawicel Classic, an explicit CANable FD profile, and native Linux/Windows serial I/O; it remains experimental.
- Define Session, shared-bus, receive-routing, bounded-send, timeout, cancellation, close, and recovery contracts. Fix lifetime/concurrency issues and add offline regressions.
- Add Windows x64/MSVC builds, native test DLLs, ZIP packages, build provenance and SHA256, installation, and consumption after extraction into paths containing Chinese characters and spaces. Vendor DLLs, drivers, and test DLLs are excluded from distributed runtime packages.
- Add GitHub Linux amd64/ARM64 Debug/Release offline CI, repeated Session recovery and registry active-generation/completed-attempt/initializer handoff regressions, and an aggregate production line-coverage threshold of at least 95%. Add dual-architecture Clang 18/libc++, ASan+UBSan, and TSan jobs.
- Format sources with Clang 18 and add Linux ARM64 clang-format/clang-tidy checks. libc++ `-Wall -Wextra -Wpedantic -Werror` applies to the production library, tests, fixtures, example, and installed consumer. Pin GitHub Actions dependencies to full commit SHAs and use Node.js 24 action versions.
- Fix a process-exit reliability failure caused by registry destruction order when static owners retain `Session::share()`, `Session::lease()`, or `Session::borrow()`. Add independent shared/borrowed process-exit regressions.
- Mark `SendReceipt::result()`, `wait_until()`, `completed()`, and `RouteInbox::health()` as `[[nodiscard]]`. Downstream code using `-Werror` must explicitly cast intentionally unused results to `void`.
- Organize bilingual entry points and Chinese guides, including Linux/SocketCAN, build options, diagnostics, upgrades/rollback, and installed documentation navigation. Label historical hardware statements separately; they do not establish current hardware acceptance.
- Pair `0.3.x` with `linkerbot-hand-cpp 0.3.x`; the comm version must not be older than the hand version. Rebuild the matching application when updating the SDK; binary ABI compatibility across arbitrary builds is not promised.
