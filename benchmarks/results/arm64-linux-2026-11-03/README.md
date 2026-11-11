# ARM64 Linux measurements, 2026-11-03

These CSV files compare baseline commit
`db00c561d57ce846909ccdf23513ccdb814ebb7f` with the subscriber snapshot-cache
implementation accompanying this change. The candidate had no commit SHA at
measurement time. This directory records a local experiment, not a CI speed gate.
The [English](../../../docs/en/performance.md) and
[Chinese](../../../docs/zh/performance.md) guides explain the results and tradeoffs.

## Environment

- Ubuntu 24.04 userland; kernel `7.0.9-205.fc44.aarch64`.
- Virtualized Apple ARM64, five available CPUs; process affinity `0-3`.
- Cgroup `cpu.max`: `max 100000`.
- Clang 18.1.3 with libc++, Release (`-O3 -DNDEBUG`),
  `-Wall -Wextra -Wpedantic -Werror`; no LTO or sanitizers.

Both builds used the same benchmark source. Its compile-time API detection uses
`snapshot()` on the baseline and `snapshot_view()` on the candidate. Recorded
SHA256 hashes for the benchmark files are:

```text
09a86e6a3eb8d86f254887a13cc6938c84dccf259c3d5c029b8ad6e66842e001  benchmarks/main.cpp
6d2fca9eef5674d9e92529787fb1eab79eae215c3d85aa9d4a84767aa7e8b457  benchmarks/CMakeLists.txt
```

The final source files above produced the measured binaries. Their SHA256 values are:

```text
47308e5a3346e847b0262dcca8925b9b7f49b2fb9427c532203196717d587f69  before/time
fa45f447449661f257be9bc6a6166ef132960415908f021f26bfa3c4d4876057  before/alloc
ba4e027d097abf6d4a567a349993c01c3f69f9175281fe524ae5674211e8dc1b  after/time
73973057858c1732ffbfe8ccbdcaf302f12483de465584f117580aa51b2acd5e  after/alloc
```

## Collection method and files

Timing builds used `LINKERBOT_PERF_COUNT_ALLOCATIONS=OFF`, which does not replace
global allocation functions. Five independent processes per build ran in the
order `A B B A A B B A A B`, where A is baseline and B is candidate. Each process
used `--iterations 200000 --idle-ms 500 --repetitions 1`. Files are
`before-time-0.csv` through `before-time-4.csv` and `after-time-0.csv` through
`after-time-4.csv`; each side's index follows its collection order.

Separate builds used `LINKERBOT_PERF_COUNT_ALLOCATIONS=ON` and
`--iterations 200000 --idle-ms 500 --repetitions 3`. Their output is
`before-alloc.csv` and `after-alloc.csv`. These measurements count successful C++
allocations and requested bytes, not live memory or C allocation functions.
Their instrumented timings are not used to claim speed improvements.

From the repository root, validate the CSV schema, modes and checksums and
summarize medians and ranges with:

```sh
python3 benchmarks/micro/summarize.py benchmarks/results/arm64-linux-2026-11-03
```

The uninstrumented median for snapshot acquisition plus invocation at 1, 8 and
64 subscribers changed from 63.079, 250.026 and 1656.205 ns/op to 41.996,
214.088 and 1359.029 ns/op. Empty snapshots changed from 15.906 to 18.613 ns/op.
Baseline and candidate ranges overlap at every subscriber count; these medians
are observations from this sample set, not a promise of speedup. The cold
`mutation_snapshot` medians for 0, 1, 8 and 64 subscribers increased from 199.370,
228.196, 284.172 and 750.240 ns/op to 276.054, 303.277, 378.541 and 1117.537 ns/op.
Steady nonempty snapshots went from one allocation per operation to zero;
cold mutation/snapshot pairs added two allocations and 96 requested bytes.

These direct registry microbenchmarks do not measure CAN throughput or application
latency. Short virtualized runs remain sensitive to scheduling and host load.
Idle scenarios retain substantial run-to-run variation; no idle implementation
was changed and these observations do not establish an idle CPU or wakeup benefit.
The cache preserves cancellation and callback-lifetime semantics, but its changed
type layout requires rebuilding downstream code.
