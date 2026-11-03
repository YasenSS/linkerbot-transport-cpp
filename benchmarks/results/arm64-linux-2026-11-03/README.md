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
aacf536a6b54b27099a1090b2c72fc88fb31efd06ffba1ba680b7c0f439d678e  benchmarks/main.cpp
e0aaa36c30aac9e6ab94b0e3d67f8832a47603098a1e5d70bb5d41bdaf3264e3  benchmarks/CMakeLists.txt
```

After measurement, the C++ time header was changed to `<ctime>` and sorted, and
the standalone CMake entry gained a guard against re-entering the benchmark
directory. Rebuilding confirmed byte-identical measurement executables before
and after those changes. The recorded executable SHA256 values are:

```text
8047ea45f36fb5a627e95bdce17cda513801c6906b7a3473105a5e970b2b3825  before/time
3ab537d1abca0f261103085572f397355017e869c775ed9d0a7981c0290d263c  after/time
b96f938157311b7e523a2613ad358bb16e2185d625c4e25d9ffa281458d7e750  before/alloc
33d247a8c33e4d664f22f24cb50f1a958ff5d40befa62b03dfa5ff04634c3890  after/alloc
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
python3 benchmarks/summarize.py benchmarks/results/arm64-linux-2026-11-03
```

The uninstrumented median for snapshot acquisition plus invocation at 1, 8 and
64 subscribers changed from 43.60, 149.62 and 879.49 ns/op to 31.63, 118.87 and
743.79 ns/op. Empty snapshots changed from 10.84 to 15.90 ns/op. The cold
`mutation_snapshot` medians for 0, 1, 8 and 64 subscribers increased from 123.17,
146.28, 162.04 and 376.86 ns/op to 220.41, 210.06, 202.56 and 534.56 ns/op.
Steady nonempty snapshots went from one allocation per operation to zero;
cold mutation/snapshot pairs added two allocations and 96 requested bytes.

These direct registry microbenchmarks do not measure CAN throughput or application
latency. Short virtualized runs remain sensitive to scheduling and host load.
Idle scenarios retain substantial run-to-run variation; no idle implementation
was changed and these observations do not establish an idle CPU or wakeup benefit.
The cache preserves cancellation and callback-lifetime semantics, but its changed
type layout requires rebuilding downstream code.
