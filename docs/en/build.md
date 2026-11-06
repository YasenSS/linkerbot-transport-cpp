# Building, installing and integrating with CMake

[简体中文](../zh/build.md) · [Documentation](../README.md) · [Windows](windows-build.md)

The canonical package/targets and retained compatibility names are described in
[name migration](naming.md). Published v0.3.2 packages retain the old names.

## Prerequisites and directories

The core library requires CMake 3.16+, a C++17 compiler and thread support. The regular test
commands below use `ctest --test-dir`; use CMake/CTest 3.20+ for those commands. The next
section gives a 3.16-compatible integration path. Catch2 3.8.1 itself declares CMake 3.16,
but its test-discovery script needs JSON support, so this project requires CMake 3.19+
with tests enabled. Check any separately installed Catch2 version's own requirements too.
Linux CI uses Ubuntu 24.04: GCC 13 covers native amd64/ARM64 Debug/Release and the 99%
line-coverage gate; Clang 18 covers libc++, ASan+UBSan and TSan on both architectures.
Local maintenance validation prioritizes Linux ARM64. Windows uses MSVC x64/ARM64 as described
in its guide. macOS is outside the current hardware-support and CI validation matrix.

Run Bash commands from the source root. Examples use Ninja, which must be installed;
omit `-G Ninja` to use the platform default generator. On Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends build-essential cmake ninja-build git ca-certificates
```

Enabling tests may download Catch2 from GitHub, requiring network access and Git. The core
library requires no vendor SDK, vendor dynamic library, CAN device or Catch2. Do not copy
vendor DLLs into the source directory as build dependencies.

## Minimal integration with CMake 3.16

This path downloads no test dependencies and requires no physical device:

```bash
cmake -S . -B build-minimum -DCMAKE_BUILD_TYPE=Release \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=OFF \
  -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=OFF \
  -DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF
cmake --build build-minimum --parallel 4
cmake --install build-minimum --prefix "$PWD/install/minimum"
cmake -S examples/quickstart -B build-quickstart-minimum \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install/minimum"
cmake --build build-quickstart-minimum --parallel 4
(cd build-quickstart-minimum && ctest --output-on-failure)
```

With older CTest, enter the build directory before running tests instead of using the
unsupported `--test-dir` option. This example verifies in-memory transport I/O and installed
package integration, not the complete regression suite or hardware acceptance.

## Complete offline regression

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON
cmake --build build-release --parallel 4
ctest --test-dir build-release --output-on-failure --no-tests=error --timeout 60
./build-release/can-example
```

For Debug, use a separate `build-debug` directory and `-DCMAKE_BUILD_TYPE=Debug`.
Single-configuration generators choose the configuration at configure time. Visual Studio
and other multi-configuration generators require `--config` / `-C` during build, CTest and
installation. Do not reuse directories from another architecture, compiler or generator.

Prefer CTest: CMake injects test-library paths for `pcan-native-loader` and
`vendor-native-loader`. Running their executables without those environment settings fails;
that does not indicate a missing physical driver. To repeat only Session recovery cases:

```bash
ctest --test-dir build-release -R '^Session recover' --repeat until-fail:20 \
  --output-on-failure --no-tests=error --timeout 60
```

GCC amd64/ARM64 Debug/Release CI also repeats three registry lifecycle interleaving tests:

```bash
ctest --test-dir build-release \
  -R '^(Session registry waiters recheck a replacement active generation|Session registry ignores waits for a published initialization attempt|Session registry rebinds a waiter to a replacement initialization attempt)$' \
  --repeat until-fail:20 --output-on-failure --no-tests=error --timeout 60 \
  --output-log registry-handoff-results.log
```

The first checks that an old wait condition does not miss a new active generation published
after cleanup. The second checks that a published initialization attempt does not produce a
false cycle through a stale wait edge. The third checks rebinding to a replacement attempt
after initialization failure. Synchronization gates fix entry into cleanup, publication and
failure paths, but public APIs do not expose exactly when internal wait edges are installed
or removed. Repetition broadens handoff scheduling coverage and timeouts prevent hangs;
the repetition count is not a deterministic proof.

## Build options

| Setting | Default | Purpose |
| --- | --- | --- |
| `LINKERBOT_TRANSPORT_BUILD_TESTS` | ON at top level, OFF as subdirectory | Catch2 tests and native ABI fixtures |
| `LINKERBOT_TRANSPORT_BUILD_EXAMPLES` | ON at top level, OFF as subdirectory | Build `can-example` |
| `LINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS` | ON | Download v3.8.1 if Catch2 3 is missing |
| `LINKERBOT_TRANSPORT_ENABLE_COVERAGE` | OFF | Linux/GCC only; requires tests ON; adds `--coverage -fprofile-update=atomic -O0 -g` |
| `LINKERBOT_TRANSPORT_ENABLE_SERIAL` | ON | Build and export `linkerbot::serial` |
| `LINKERBOT_TRANSPORT_ENABLE_MODBUS` | ON | Build and export `linkerbot::modbus`; requires serial |
| `LINKERBOT_TRANSPORT_BUILD_FUZZERS` | OFF | Build the Clang RTU codec fuzzer; requires Modbus |
| `LINKERBOT_TRANSPORT_BUILD_BENCHMARKS` | OFF | Build the Linux-only performance harness with the root project |
| `LINKERBOT_PERF_COUNT_ALLOCATIONS` | OFF | Instrument C++ allocations in the benchmark; available when its subdirectory is enabled |
| `Catch2_DIR` | Located by CMake | Installed Catch2 3 CMake configuration directory |
| `CMAKE_INSTALL_PREFIX` | Platform CMake default | Installation prefix; install-time `--prefix` is also supported |
| `LINKERBOT_TRANSPORT_SOURCE_REVISION` | Git automatically, otherwise unknown | Supply a full source SHA for exported source builds |
| `CMAKE_MSVC_RUNTIME_LIBRARY` | Dynamic CRT on MSVC | Default Release `/MD`, Debug `/MDd`; match the consumer |

These are CMake cache variables, not runtime environment variables. Disabling downloads
does not remove cached Catch2 or supply missing dependencies. `BUILD_SHARED_LIBS` does not
make this a shared library: the core target is explicitly STATIC.

To prohibit dependency downloads and build only the core:

```bash
cmake -S . -B build-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=OFF -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=OFF \
  -DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF \
  -DLINKERBOT_TRANSPORT_ENABLE_SERIAL=OFF -DLINKERBOT_TRANSPORT_ENABLE_MODBUS=OFF
cmake --build build-core --parallel 4
```

For offline tests, install Catch2 3 first, add
`-DCatch2_DIR=/absolute/path/to/lib/cmake/Catch2` to the complete regression configure command
and disable downloads. Catch2 2 does not satisfy the requirement.

## Performance benchmarks

On Linux, build the standalone harness without Catch2 or physical hardware:

```bash
cmake -S benchmarks -B build/perf -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/perf --parallel 4
./build/perf/linkerbot-perf --iterations 200000 --idle-ms 500 --repetitions 5
```

Alternatively, set `-DLINKERBOT_TRANSPORT_BUILD_BENCHMARKS=ON` when configuring the root
project; the executable is then under `build/benchmarks/`. Keep allocation tracking OFF
for timings and use a separate build with `-DLINKERBOT_PERF_COUNT_ALLOCATIONS=ON`
for allocation counts. See [Performance](performance.md) for measurement boundaries,
reproduction commands and recorded ARM64 results.

The Linux CI `benchmark-smoke` job builds both allocation modes with Clang 18/libc++
on ARM64 and runs short scenarios, retaining CSV artifacts. It checks that the harness
builds and runs, including its checksum checks; it is not a performance threshold or
a substitute for controlled before/after measurements.

## Line coverage and verifiable results

Install `gcovr` and use a new build directory to avoid mixing `.gcda` counts from old source:

```bash
sudo apt-get install -y --no-install-recommends gcovr
cmake -S . -B build-coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON -DLINKERBOT_TRANSPORT_ENABLE_COVERAGE=ON
cmake --build build-coverage --parallel 4
ctest --test-dir build-coverage --output-on-failure --no-tests=error --timeout 60
gcovr --root . --filter '(src|include)/' --gcov-exclude-directories 'build-coverage/_deps' \
  --fail-under-line 99 --txt --json coverage.json --json-summary coverage-summary.json build-coverage
```

99% means aggregate production **line coverage** in the Linux build, not 99% per file or
per branch. It includes `src/` and `include/`, including public FakeTransport, and excludes
test directories and Catch2. Production lines are not selectively excluded to improve the
result. Windows code omitted by Linux conditional compilation is outside the denominator.
See [Validation](validation.md) for executed fixed-commit results; a CI file alone is not a pass.

The Linux Debug jobs retain `coverage.json` alongside the summary. The full report lists
executable lines, execution counts and branch records for each measured file, so remaining
gaps can be inspected against the source revision of that run. Use a fresh build and the
same test sequence when comparing results; do not compare rounded percentages alone.

100% production line coverage would mean every executable line in this measurement was
executed. It is a narrower target than 100% branch coverage, coverage of all supported
platforms, or validation with physical devices. Linux preprocessing omits Windows native
serial and DLL-loading paths. A passing line counter also does not establish that every
error outcome or concurrent interleaving has been checked.

Before raising the gate to 100%, classify each remaining line: a reachable behavior needing
a test, a failure path needing controlled fault injection, or a path whose reachability
needs investigation. The existing Linux syscall wrappers, native ABI fixture libraries and
allocation-failure executables provide fault-injection mechanisms, but do not establish
that every remaining path can be exercised deterministically. Keep production lines in
the denominator and preserve defensive behavior. A 100% gate needs repeatable evidence
on both Linux CI architectures; the current gate remains strictly above 99% before rounding.

Some current paths cannot be covered practically through ordinary public API calls:
`RtuSession::recover` guards exhaustion of a 64-bit generation counter, and the Session
registry handles wraparound of 64-bit generation and initialization counters. The registry
also rejects an already-corrupted dependency cycle, and pool cleanup terminates if its
internal move invariant is broken. These need a justified internal test seam or a separate
reachability argument; repeated public operations are not a practical way to reach them.
The vendor transmit encoder validates a message before converting its length to a DLC,
so an invalid public payload is rejected before the conversion helper's defensive throw.
These constraints prevent a promise that adding public API tests alone will produce 100%.

Consumers of coverage-instrumented static libraries also need `--coverage` at link time,
for example `-DCMAKE_CXX_FLAGS=--coverage`. Normal Release builds keep coverage OFF.

## Clang, a second standard library and sanitizers

The Linux amd64/ARM64 Clang matrix uses three independent Debug builds:

- `libcxx`: Clang 18 and libc++, all CTests including both native ABI fixtures, compiling
  the library, tests, fixtures and example with `-Wall -Wextra -Wpedantic -Werror`.
  It runs the example and checks an installed libc++ consumer with the same warning policy.
- `asan-ubsan`: libstdc++ and `-fsanitize=address,undefined`, leak detection enabled,
  failing at the first ASan/UBSan error. Individual CTests run in separate processes;
  only the two native ABI fixtures described below are excluded.
- `tsan`: libstdc++ and `-fsanitize=thread`, deadlock detection enabled, all tests run
  as separate CTest processes.

ASan+UBSan is not combined with Ubuntu's system libc++: ARM64 observations reported
`operator new`/`free` mismatch in libc++/libc++abi exception-object allocation. That observed
report is not attributed to project code. The two native-loader fixtures are also excluded
from ASan+UBSan: to model third-party C++ libraries without headers, each side deliberately
declares layout-compatible ABI structures with distinct type identities. UBSan metadata on
both sides reports function-type mismatches. CI does not disable those checks to suppress
reports; GCC, Clang/libc++ and TSan still cover the fixtures.

Retain compiler version, complete diagnostics and exit status for thread checks. Running
all Catch2 cases in one process has produced cross-case lock-graph warnings when different
tests reuse stack addresses. CI therefore uses individual processes generated by
`catch_discover_tests`; globally disabling deadlock detection or suppressing warnings is
not a substitute for diagnosis.

To reproduce the sanitizer jobs on Ubuntu 24.04 ARM64, install Clang 18, LLVM's
symbolizer and the sanitizer runtime, then use separate build directories. These
commands use libstdc++, as in CI, and run each discovered CTest separately:

```bash
sudo apt-get install -y --no-install-recommends \
  clang-18 llvm-18 libclang-rt-18-dev
cmake -S . -B build-asan-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined' \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON
cmake --build build-asan-ubsan --parallel 4
env ASAN_SYMBOLIZER_PATH="$(command -v llvm-symbolizer-18)" \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan-ubsan \
    -E '^(pcan-native-loader|vendor-native-loader)$' \
    --output-on-failure --no-tests=error --timeout 90

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -fsanitize=thread -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread' \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON
cmake --build build-tsan --parallel 4
env TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:external_symbolizer_path=$(command -v llvm-symbolizer-18)" \
  ctest --test-dir build-tsan --output-on-failure --no-tests=error --timeout 120
```

## Clang formatting and static analysis

### C++ Report Card

The separate `C++ Report Card` workflow runs
[cppreportcard-action](https://github.com/soulteary/cppreportcard-action) v1.0.2,
pinned to a full commit SHA, on Linux ARM64. It runs clang-format 18, clang-tidy 18
and Cppcheck in `full` mode after building the project, quickstart, installed
consumer, shared-library consumer and installed-library Python validation probe.
Their five compilation databases are combined so all standalone consumers and the
probe are analyzed with the main project's sources and tests.

The configuration is `.cppreportcard.json`. Platform-dependent exclusions are
`tests/vendor_fixture_dependency.cpp` (Windows-only), `validation/pcan_header_abi.cpp`
(external vendor header), and `validation/ros1-consumer` (external Noetic SDK absent
from this report environment). ROS fixtures instead compile with warnings as errors
and execute node/nodelet tests in the Noetic workflow; the separate global style
job still formats every tracked C++ source. Build/output directories and third-party
dependencies are also excluded.
The report uses `clang-analyzer-*` and `bugprone-*`; it does not replace the
existing style workflow's different rule set or platform checks.

`validation/prepare-report-db.py` merges the databases and exposes project
`-isystem` directories as `-I` directories for Cppcheck. The workflow also records
the compiler's actual target macros. Cppcheck preincludes the installed multiarch
Python configuration to traverse CPython's same-name forwarding header; no probe
source or preprocessor error is excluded. Through
`validation/cppcheck-report.sh`, Cppcheck uses its `std` library model and
`validation/catch2-cppcheck.cfg` for the Catch2 macros used by the project.
The wrapper suppresses only
`missingIncludeSystem`: compiler standard headers are not Cppcheck input.
The full-mode clang-tidy pass still checks every compilation entry for missing
headers, including project headers written with angle brackets. Other Cppcheck
preprocessor and syntax errors are not suppressed.

The Catch2 model addresses a Cppcheck 2.13 parser limitation; it does not remove
test bodies or assertion operands from analysis. Real builds continue to use
Catch2. Before generating the report, the workflow runs 29 static sentinels and
two runtime probes that compare fatal/nonfatal assertion behavior in real Catch2
and the model. The sentinels require detection of null and bounds errors inside
assertions, section names and bodies, and later test cases. A registration guard
requires literal test names and tags; it rejects expressions the model cannot
analyze. After building the three analysis
inputs and generating the combined database as in the workflow, run the same
model check from the source root:

```bash
python3 validation/test-cppcheck-model.py --compile-commands build/report-compile-commands.json
```

CI reports are tied to the commit they analyze.

Download the `cppreportcard-ubuntu-24.04-arm` artifact from a workflow run for
`report.json`, `report.md` and `badge.svg`, or read its job summary. CI writes these
files under `.github/cppreportcard/`. Both READMEs display the checked-in badge
snapshot; its source commit and run are recorded in
[the snapshot notes](../../.github/cppreportcard/README.md). Refresh the badge and
notes together in a signed maintenance commit. Generated JSON and Markdown reports
remain artifacts. The workflow has read-only repository access and uses
`fail_on: findings`. `validation/check-quality-report.py` additionally requires
an A+ grade, complete full-mode analysis by all three tools, and no diagnostics.
Its negative controls reject incomplete, missing-tool and misleading-grade
reports. This tightens the CI gate without changing analyzer rules or scoring.
The score is not test coverage or hardware validation. Cppcheck comes from the
Ubuntu package repository, so its version can change between runner updates;
compare the tool versions recorded in reports.

### Existing Clang checks

An independent Linux ARM64 workflow uses clang-format 18 on all tracked C++ sources and
headers. clang-tidy 18 checks project, example and test translation units in the compilation
database and the separately checked files below; headers are checked through those units.
The main compilation database includes `examples/shared_bus.cpp`, but not the separately
configured `examples/quickstart/main.cpp`; the current style job does not run clang-tidy
on that quickstart. The formatting check still includes both tracked example sources.
Local maintenance validation also uses ARM64; GitHub CI provides amd64 results. Local x86
runs do not replace ARM64 evidence. On Ubuntu 24.04:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  cmake ninja-build git ca-certificates \
  clang-18 clang-format-18 clang-tidy-18 clang-tools-18 \
  libc++-18-dev libc++abi-18-dev
```

From the source root, check formatting:

```bash
git ls-files -z -- '*.cpp' '*.hpp' '*.h' \
  | xargs -0 clang-format-18 --dry-run --Werror
```

Generate the compilation database with tests/examples, then use the CI clang-tidy scope:

```bash
cmake -S . -B build-tidy -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-stdlib=libc++' \
  '-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-stdlib=libc++' \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON \
  -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON

run-clang-tidy-18 -p build-tidy -j 2 -quiet \
  -warnings-as-errors='*' \
  "^${PWD}/(src|examples)/.*[.]cpp$|^${PWD}/tests/(?!test_).*[.]cpp$"

run-clang-tidy-18 -p build-tidy -j 2 -quiet \
  -checks=-cert-err58-cpp -warnings-as-errors='*' \
  "^${PWD}/tests/test_.*[.]cpp$"

clang-tidy-18 tests/vendor_fixture_dependency.cpp \
  -warnings-as-errors='*' -- -std=c++17

clang-tidy-18 validation/installed-consumer/main.cpp \
  -warnings-as-errors='*' \
  -- -std=c++17 -stdlib=libc++ -Iinclude -Icompat
```

`.clang-tidy` ignores `cppcoreguidelines-avoid-do-while` only for macro expansions.
Catch2 `TEST_CASE` generates static registration objects and `cert-err58-cpp` has no macro
ignore option, so only `tests/test_*.cpp` excludes that check in the second pass. Project
sources, test-support units and all other enabled rules still treat warnings as errors.
`tests/vendor_fixture_dependency.cpp` is built only on Windows and is absent from the Linux
compilation database, so Linux checks its portable branch separately.
`validation/pcan_header_abi.cpp` needs the external official PCAN header and is outside the
generic style job; record explicitly whether that check ran or was skipped.

Formatting/static-analysis success does not establish build, CTest, sanitizer, hardware or
release success; retain each result separately.

## Source-tree and installed integration

For source-tree integration, define the consumer's `your-sdk` target first:

```cmake
add_subdirectory(../linkerbot-transport-cpp linkerbot-transport-cpp)
target_link_libraries(your-sdk PRIVATE linkerbot::can)
```

Tests and examples default OFF as a subdirectory. If explicitly enabling tests, the top-level
project must also enable CTest discovery. Install and check an independent consumer:

```bash
cmake --install build-release --prefix "$PWD/install/transport"
cmake -S install/transport/share/linkerbot-transport/validation/installed-consumer \
  -B build-consumer -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/install/transport"
cmake --build build-consumer --parallel 4
ctest --test-dir build-consumer --output-on-failure --no-tests=error --timeout 60
```

Applications use `find_package(linkerbot-transport 0.3 CONFIG REQUIRED)`:

- `linkerbot::can`: implementation, headers, C++17 and thread-link dependencies.
- `linkerbot::can-compat`: legacy flat header paths plus implementation linkage.
- `linkerbot::headers`: header paths and C++17 only; link the module that implements the APIs used.
- `linkerbot::serial`: serial byte streams (when enabled).
- `linkerbot::modbus`: RTU transactions and serial dependency (when enabled).

`SameMinorVersion` requires matching major/minor and an installed version at least as high
as the requested version. The companion hand SDK's patch-version requirement still applies;
successful `find_package(... 0.3 ...)` alone does not establish compatibility. Recompile
matching components before mixing architectures, CRTs, compilers or library versions; see
[Upgrade order](operations.md).

## Packaging and provenance

Top-level Linux builds produce TGZ by default:

```bash
cpack --config build-release/CPackConfig.cmake -B "$PWD/dist"
```

CPack generates adjacent SHA256 files. New Linux filenames include version, OS, architecture,
build distribution, compiler, standard library and configuration. Keep separate build directories
for different configurations. Existing release assets retain their original names.
See the Windows guide for ZIP naming and CRT details.

`share/linkerbot-transport/build-info.json` contains version, `transport_sha`, `source_dirty`, OS,
architecture, compiler, toolset, configuration and CRT. Provenance is generated at CMake
configuration time; reconfigure after changing source commits. Metadata from an old build
directory does not validate a new commit. Source exports without Git may supply a full SHA
explicitly, but that is caller-provided provenance, not a content checksum or signature.
`source_dirty` includes tracked changes and untracked non-ignored files from Git status; it
does not inspect ignored files or separately installed vendor binaries. See [CI packages and
compatibility](ci.md) for strict release inputs, extended metadata and validation targets.

Before distribution, check clean source, version, configuration, architecture and SHA256,
then rerun installed-consumer validation. Building does not automatically create a GitHub
Release, signed tag or image. The separate release workflow can create a draft only after an
explicit manual request and its validation gates; CI and packaging success are not release completion.

The serial and RTU modules export `linkerbot::serial` and `linkerbot::modbus`.
Link their targets explicitly; `linkerbot::can` does not pull them in. These
modules require 0.3.3 or later; the published v0.3.2 package does not include them.
See [serial and RTU](modbus-rtu.md).

The allocation-failure lifecycle fixture replaces global allocation functions and
is omitted from TSan builds, which supply their own replacements. It remains in
GCC coverage, strict libc++ and ASan/UBSan builds; ordinary concurrency tests run
under TSan. Linux syscall wrappers are isolated from the ordinary PTY tests.

The CI threshold uses the original `line_covered` and `line_total` counts and
requires `line_covered * 100 > line_total * 99`. A rounded gcovr percentage of
99.0 is insufficient when the underlying ratio is below the threshold.

In MSVC Debug builds the allocation fixture leaves iterator-proxy-sized requests
unarmed: the STL allocates those proxies from `noexcept` empty-container
constructors. This size-based limitation also affects any same-size payload
allocation in that fixture. Linux coverage injection is unchanged.
