# Validation records and support boundaries

English · [简体中文](../zh/validation.md)

[Documentation index](../README.md) · [Reproduce builds](build.md)

This guide records implementation capabilities, offline validation, and hardware acceptance separately. CMake identifies the source as 0.3.0. Published packages and checksum instructions are in the [release guide](releases.md); a documentation update or successful CI run alone does not publish a package.

<a id="release-030"></a>

## 0.3.0 release validation

The published [0.3.0 release](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.0)
uses signed tag `v0.3.0`, tag object `ba02c06867a3014ad1c01f56ea4a5602bc07189a`,
and source commit `a46177e86e403bbd18834aedb6e81ef3233c6c22`.
All 16 checks passed on this commit in the upstream repository:

- [Linux regression and packages](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41636452844):
  amd64/ARM64, minimum CMake, libc++, ASan/UBSan, TSan, installed consumers and quickstart.
- [Windows regression and packages](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41636452738):
  x64 Debug/Release, native ABI fixtures and relocated consumers.
- [Style checks](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41636452851) and
  [full report](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41636452744):
  complete A+, 100/100, zero diagnostics; 54/54 format inputs and 37/37 entries per analyzer.

Local ARM64 Linux regression passed 218/218 tests. The new receipt-publication test
failed against the previous Session implementation and passed with the fix.
Upstream ARM64 production line coverage was 3,673/3,838 (95.7%). After publication,
all ten uploaded assets were downloaded again: checksums, sizes, source metadata
and all seven objects in each static library matched. The four SDK packages contain
Linux AArch64, Linux x86-64 and Windows AMD64 objects as appropriate. The downloaded
ARM64 package also passed both installed-consumer tests locally.
These are offline results, not physical-adapter acceptance.

## Recorded transport validation

The following records for exact commits were publicly available as of 2026-11-03:

The documentation and minimal-consumer regression additions passed the
[Linux](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41559611149),
[ARM64 formatting/static analysis](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41559611240),
and [Windows](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41559611152)
workflows on PR #9 merge commit `79f1731c355fbd46eeb63224d89aaa9039c2df27`.
The additions include ARM64 CMake 3.16.9 library build/install, execution of the
installed quickstart, and expected configuration rejection when that older CMake
enables the full tests. The GCC matrix also runs the installed quickstart. These
are offline records for that fixed commit, not validation of subsequent edits to
this guide or proof of physical-adapter availability. Earlier repair evidence is
retained below:

| Source | Linux amd64/ARM64 | ARM64 formatting/static analysis | Windows x64 |
| --- | --- | --- | --- |
| Signed commit `50586ff3b201ed944d7ca8d9dac4ebbaee5c1753` (PR #1) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41445930151) | Not available then | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41445930282) |
| Merge commit `d4e1c70567bd6ec4b3fbfd4a60b4156ec2fcc53f` | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41446343946) | Not available then | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41446343963) |
| Merge commit `38d30cc936cde505718314f3cdde62d022481f89` (PR #3) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41478753364) | Not available then | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41478753429) |
| Merge commit `0c91316af183b48c224a1a3cb6f827622dafa1be` (PR #4) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41481036933) | Not available then | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41481037581) |
| Signed commit `737ac66dd4abebdf6139b3199eb9019d91ea6186` (PR #5 head) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553062590) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553062589) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553062617) |
| Merge commit `4ad07b3b87863b04f3d25d13d9840c465497f853` (PR #5) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553596800) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553596723) | [Passed](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41553596766) |

PR #3 signed head `f854c05e26a1812fbd1748769aaf0badc50a0cea` also has successful [Linux](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41474006568) and [Windows](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41474006403) records. PR #4 signed head `6216db5203d797561da2d6797c9636b90f7e47f9` also has successful [Linux](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41480373636) and [Windows](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41480373570) records.

The table retains post-merge main runs so their branch state can be checked directly. PR #1 Linux jobs included only GCC Debug/Release, repeated Session recovery regressions, the example, installed consumer, and the coverage threshold used at that time. PR #3 and later added libc++, ASan+UBSan, TSan, repeated registry handoff regressions, and at least 95% aggregate production line coverage on both architectures. Independent ARM64 formatting/static analysis started with PR #5. The corresponding Windows records cover native DLL fixtures, Debug/Release, ZIPs, SHA256, provenance, and consumption from paths containing Chinese characters and spaces. Exact tool versions, test counts, and coverage belong to each run's logs and attachments; one result is not a guarantee for later commits.

Earlier local PR #1 validation used Ubuntu 24.04 ARM64/GCC 13.3: Debug and Release each passed 167 CTest cases, with production line coverage 2,935/3,079 (95.3%) and branch coverage 61.1%. Twenty-nine Session/fault cases passed TSan in separate test processes. Running the whole group in one process reported a lock-order warning when tests reused stack addresses; that run cannot be recorded as warning-free. A negative control omitting physical reopen failed the failed-retry regression as expected; production recovery logic was not changed for that control. These historical results do not replace validation of subsequent commits.

Local ARM64 results for the PR #5 head follow. Linux builds ran in an Ubuntu 24.04 container; actionlint ran on the ARM64 host:

- Clang 18/libc++ strict build, GCC 13 Debug, GCC 13 Release, and TSan each passed 211/211 CTest cases.
- ASan+UBSan passed 209/209, excluding only the two native-loader fixtures under the CI rules.
- The installed libc++ consumer passed 1/1.
- Session recovery and each of the three registry handoff groups passed 20 repetitions under TSan.
- Production line coverage was 3,635/3,798 (95.7%), not branch coverage or 95% for every file.
- clang-format, clang-tidy, and actionlint 1.7.12 passed.

When a static owner obtains `Session::share()` or `Session::borrow()` in `main()` and retains it until process exit, the pre-fix ARM64 ASan control reports heap-use-after-free during registry release. After the fix, the two independent process-exit cases and full ASan suite passed. This is a process-exit reliability failure. The fix preserves registry keys, retained records, and validation behavior; it does not delete or rewrite old records.

No local x86 run was performed; amd64 was checked by the GitHub CI runs above. No hardware acceptance was performed, nor was `validation/pcan_header_abi.cpp`, which needs an external official PCAN header, compiled for this PR #5 local record. The first local Release configuration stopped before building because downloads were disabled and no Catch2 package path was supplied. With the cached fixed Catch2 3.8.1 source, the complete Release build and CTest passed. Passed, failed, and skipped items are distinct evidence.

The earlier records in this section belong to the `soulteary/linkerbot-transport-cpp` fork and do not by themselves establish upstream main's state. The release section above records the later upstream checks for the published commit. A secondary Gitea workflow is present, but no publicly verifiable act_runner run is recorded here. Its configuration, or a GitHub YAML check, is not evidence of a successful Gitea run.

## Platform and device capabilities

- Successful Linux amd64/ARM64 builds and fixtures do not establish the availability of vendor libraries or drivers for those architectures.
- SocketCAN requires a Linux CAN network interface. Unix socketpair tests check frame/resource contracts, not a physical CAN bus.
- PCAN's hardware target is Windows x64; Linux dynamic-library tests check only the loader ABI.
- MiniCANFD requires a `libcanbus.so` matching the process architecture or x64 `HCanbus.dll`. An ARM64 process cannot load an x64 library.
- SLCAN implements explicit Lawicel Classic/CANable FD profiles but remains experimental. PTY and byte-stream tests do not establish firmware compatibility.
- The current matrix does not cover macOS hardware, Windows ARM64, USB hotplug acceptance, long-duration load acceptance, or hard real-time guarantees.

<a id="historical-hardware-statements"></a>
<a id="历史实机陈述保留出处不作本仓库验收"></a>

## Historical hardware statements: provenance, not acceptance for this repository

The [initially imported README](https://github.com/soulteary/linkerbot-transport-cpp/blob/4393d831dcdb1abb1873e685964f6eb17427f121/README.md) and its backend guides claimed the following records for the companion hand SDK; some were dated 2026-10-19:

| Historical path | Statement in the imported documentation |
| --- | --- |
| Linux SocketCAN | Common features on six models: L6, O6, L20 Lite, L20, L30, O30 |
| Windows Debug PCAN Classic | L6, O6, L20 Lite, L20; PCAN-Basic 5.0.0.1115 |
| Windows Debug MiniCANFD | O30; x64 HCanbus.dll 3.3.103.0; L30 untested |
| Windows Debug CANable SLCAN | Six models, limited to the firmware and parameters in those reports |

These are **historical second-hand statements**, not hardware tests performed in this maintenance work. This transport repository contains none of the corresponding reports, raw logs, adapter serial numbers, or fixed hand SDK commits. The original text supplied only `docs/hardware-acceptance/<model>/report.md` paths in the companion repository. They cannot be independently verified from this repository, so they do not establish acceptance of current source or arbitrary hardware with the same name. For hardware support, obtain fixed hand/transport SHAs, reports, and raw evidence from the maintainers. The maintained entry points are this guide and the corresponding backend guides, not the historical dated statements.

Those statements also did not cover PCAN FD, MiniCANFD L30, Lawicel firmware, Linux native vendor backends or physical SLCAN adapters, Release hardware builds, USB hotplug, or long-duration load. New drivers, firmware, architectures, or models require separate acceptance.

## Requirements for new hardware records

Retain full transport/hand SHAs, OS and architecture, compiler/CRT, driver and DLL/firmware versions, adapter identity, interface/channel, UART/CAN/FD timing, hand model and identity reply, commands, and expected results. Record reads, sends/business acknowledgments, fault injection, disconnect recovery, and sustained load separately. Report latency distributions, dropped frames, and shutdown duration. A send receipt alone does not prove device execution; timeouts or lost acknowledgments must not automatically replay non-idempotent control commands.

CI evidence applies only to the run's commit. Check main again after merging. Before release, verify the actual package, SHA256, architecture, provenance, and installed consumer. Do not reuse an old package checksum or treat new documentation as new hardware evidence.
