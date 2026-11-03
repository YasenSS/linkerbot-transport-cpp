# Quality-report diagnostic review

English · [简体中文](../zh/quality-review.md)

[Documentation index](../README.md) · [Build and analysis](build.md#c-report-card) ·
[Diagnostic ledger](../quality-diagnostics-2026-11-03.csv)

## Baseline and scope

This review starts from commit
`7f78cedb90deca23453b725fa64a94241ef87b58` and its completed ARM64
[C++ Report Card run](https://github.com/soulteary/linkerbot-transport-cpp/actions/runs/41606583858).
The ledger preserves all **156 diagnostic records**: 17 from Cppcheck and 139
from clang-tidy. These are not 156 confirmed bugs. Multiple records can share a
cause, and many describe intentional contracts or analyzer limitations.

Each CSV row records the baseline file and line, rule, classification,
resolution, and supporting evidence. Line numbers refer to the baseline;
later edits can move the corresponding code. The review does not change the
enabled rule groups, report scoring, or the scope of analyzed project files to
improve the grade.

| Classification | Records | Treatment |
| --- | ---: | --- |
| Test robustness | 95 | Keep the assertions and make optional-value access explicitly checked. |
| Intentional exception boundary | 22 | Express the existing cleanup or completion action without changing error precedence. |
| False positive | 11 | Make the existing semantics visible to the analyzer and retain focused evidence. |
| Analyzer configuration | 10 | Model the Catch2 macros that Cppcheck cannot parse; continue analyzing test bodies and operands. |
| Defensive maintenance | 7 | Distinguish argument domains and use container-sized indices. |
| Fixed ABI | 3 | Preserve vendor argument types and order; explain the exact local diagnostic. |
| Intentional contract test | 3 | Preserve tests of moved-from or failed-move behavior, with a local explanation. |
| Avoidable copy | 2 | Prepare constructor dependencies in member-initialization order before moving the configuration. |
| Intentional invalid-input test | 2 | Preserve rejection tests for unnamed, representable enum values. |
| Runtime reliability | 1 | Handle an exception escaping the installed consumer's `main()`. |

## Confirmed runtime failure

The installed-package consumer could terminate when an allocation failed after
entering `main()`. Its exception escaped the program boundary instead of
producing the consumer's ordinary failure result. The controlled ARM64 Linux
reproducer exited with status 134 before the fix; the corrected consumer prints
an error and exits with status 1.

This is a reliability defect in the validation executable. The reproducer does
not establish a failure in every application using the library, and it requires
no CAN hardware. The fix moves the consumer work into `run_consumer()` and
catches `std::exception` at `main()`, retaining the existing success and explicit
failure return values.

The permanent `comm-consumer-allocation-failure` test runs on Linux with GNU or
Clang. Linker wrapping injects a failure at the first allocation after entry to
`main()`; subsequent allocations use the real allocator, including its sanitizer
hooks. The test requires exit 1, the failure diagnostic, and no success message.
An abort does not satisfy the test. The normal installed-consumer test remains
separate.

## Contracts and false positives

Destructors intentionally close their own class's resources. Explicitly
qualifying those calls preserves the dispatch that C++ already uses during
destruction. `CANBusLease` move assignment intentionally permits a failed close
to throw before either owner changes; `noexcept(false)` states that existing
contract. Making the operation nonthrowing would lose the caller's opportunity
to retry and would be the wrong fix.

The FakeTransport error setters already stored the requested errors. A focused
runtime probe checks all three setters, including replacement with a long
message. Explicit optional emplacement makes that operation clear to Cppcheck;
compile-time checks establish nonthrowing move construction and assignment for
the default-allocator `std::string` used here.

Test assertions remain in place. `tests/optional_value.hpp` supplies
`test_support::require_value()`: it checks engagement with an ordinary `if`,
throws on an absent value, and returns the contained object by value instead of
a reference into the optional. Named and temporary optionals therefore do not
leave a reference to destroyed optional storage. The helper itself remains in
the analyzer's input; it is not a bypass for unchecked access.

Vendor ABI declarations and tests of deliberately invalid or moved-from states
remain intact. Where the diagnostic conflicts with that required behavior, a
comment identifies the exact rule and reason at the affected declaration or
assertion. Those local annotations do not disable a rule for another file or
change the report's scoring. Runtime tests still verify the relevant contract.

## Catch2 analysis model

Cppcheck 2.13 cannot parse the token-pasted string literals in the Catch2 3.8
macro implementation. `validation/catch2-cppcheck.cfg` models the public macros
used by this project. It is used only by Cppcheck; normal builds and tests still
use the real Catch2 headers and runtime.

The model keeps assertion operands and test bodies visible, preserves separate
test cases and sections, and distinguishes fatal assertions from nonfatal
checks. `validation/test-cppcheck-model.py` verifies it with **25 static
sentinels** and **two runtime probes**: one using real Catch2, the other using
the model. The static cases require detection of deliberate null and bounds
errors, including errors in later test cases and sections. The runtime probes
verify that a fatal exception-type mismatch stops the test while a nonfatal
mismatch continues, and that section names are evaluated. A separate registration
guard accepts only literal TEST_CASE names and tags; dynamic registration
expressions require extending the model before analysis can pass.

These checks cover the modeled constructs used by the project; they are not a
claim of complete equivalence with every Catch2 feature. Unknown macros,
preprocessing failures, and other analysis failures remain errors. See the
[build guide](build.md#c-report-card) for the model check command and analysis
scope.

## Validation and report provenance

The local review and failure reproducer use ARM64 Linux. The local Docker
service later stopped responding, so pending final project-wide checks must be
established by the GitHub ARM64 jobs rather than described as locally passed.
CI reports are tied to the commit they analyze. A result from an earlier commit
does not certify later edits.

The workflow fails on findings. A separate report gate checks the actual A+
grade, complete tool counts and empty diagnostic lists; a successful action
process alone cannot satisfy it. This gate changes neither rules nor scoring.

Download `cppreportcard-ubuntu-24.04-arm` from the corresponding workflow run to
inspect `report.json`, `report.md`, and `badge.svg`. A successful workflow alone
does not establish zero findings or an A+ grade. An A+ claim must match a
completed report produced by the unchanged analysis and scoring rules. The
checked-in badge is a snapshot, with its source commit and run recorded in the
[snapshot notes](../../.github/cppreportcard/README.md); update the notes and SVG
together from the actual report.

The changes retain the public APIs, vendor ABIs, cancellation barriers, close
retry ownership, and Session lifecycle behavior. No persistent-record or runtime
configuration migration is introduced. Rebuild and run the same checks when
upgrading or rolling back; do not infer compatibility from the quality grade.
