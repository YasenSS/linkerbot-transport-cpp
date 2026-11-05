# Contributing and reporting issues

English · [简体中文](CONTRIBUTING.zh-CN.md)

Use Issues for transport reports when enabled; otherwise submit a pull request with a minimal reproducer. Report hand protocol, joint-angle, and `HandDevice` interface problems to the companion hand SDK. Start with the validation scope in the [README](README.md) and the relevant backend guide.

## Issue reports

Reproduce on a specified version, then check whether current `main` already includes a fix. Include at least:

- full transport and hand commits, local modifications, and the `main` commit compared;
- OS, architecture, compiler, CMake, Debug/Release, and Windows CRT;
- backend, adapter, firmware, driver/DLL, channel, UART/CAN rates, and FD timing;
- trigger, prerequisites, required privileges, actual effect, and unaffected boundary;
- minimal reproducer, exact commands, repetitions, expected/actual results, and full errors;
- whether the issue concerns reliability, configuration, build, ABI, performance, or another category.

Remove tokens, passwords, verification codes, internal server addresses, and personal directory information before uploading logs. Do not submit vendor DLLs, drivers, proprietary headers, or captures that cannot be published.

## Change principles

Keep each change focused on one problem. Retain evidence of failure before fixing it, then prove success with a regression that also covers normal and compatible behavior. Do not hide a failure by deleting records, replacing idempotency keys, relaxing validation, or disabling detection. For transport/lifetime changes prioritize concurrency, process exit, timeouts, cancellation, lost acknowledgments, retries, and compatibility with old application records.

Performance changes need before/after benchmarks with the same environment and inputs, including duration, allocations, variation, and compatibility. Document licenses, pinned versions, download methods, and package inclusion for new dependencies. Hardware tests are not required without a real adapter; fixture, PTY, and in-memory transport results must not be reported as hardware acceptance.

## Local ARM64 validation

See the [build guide](docs/en/build.md) for complete build, coverage, sanitizer, and clang-tidy commands. Use Linux ARM64 for local maintenance validation; local x86 results do not replace ARM64 evidence. GitHub CI checks amd64 separately. At minimum, run the formatting, strict compilation, and CTest checks relevant to the change:

```bash
git ls-files -z -- '*.cpp' '*.hpp' '*.h' \
  | xargs -0 clang-format-18 --dry-run --Werror

cmake -S . -B build-libcxx -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror' \
  '-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-stdlib=libc++' \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON \
  -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON
cmake --build build-libcxx --parallel 4
ctest --test-dir build-libcxx --output-on-failure --no-tests=error --timeout 90
./build-libcxx/can-example
```

Linux aggregate production line coverage must exceed 99% before rounding over `src/` and `include/`. This is not a per-file threshold, branch coverage requirement, or hardware acceptance. Use TSan or other applicable tools for concurrency changes; explain skipped checks. Go race/vet, Redis, and database tests apply only to relevant application projects; this transport library has no such dependencies.

Installation, export, or CMake package changes also need an independent `find_package` consumer. Check interfaces, defaults, commands, links, and installed relative paths for documentation changes. Mark historical material explicitly and link the maintained guides.

## Commits and pull requests

Create an independent branch from current `main`, use signed commits, and verify the signature before pushing:

```bash
git commit -S
git verify-commit HEAD
```

Write restrained, accurate English PRs explaining reproduction, root cause, fix, compatibility, configuration, old records, upgrade order, rollback limits, and passed/failed/skipped validation. Check CI, review, and unresolved threads for the **latest head**. Creating a PR does not mean CI passed; pushing a tag does not mean a release is complete.

Merge only after checks pass and authorization is given. Synchronize `main`, verify its new CI, then clean up task remote/local branches and temporary worktrees. Preserve existing user files. The project uses the [MIT license](LICENSE); confirm that code, documentation, and test material can be published before submitting them.
