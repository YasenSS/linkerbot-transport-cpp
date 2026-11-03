# Performance measurements and snapshot tradeoffs

English · [简体中文](../zh/performance.md)

[Documentation index](../README.md) · [Build guide](build.md) ·
[Benchmark source and measurement rules](../../benchmarks/README.md)

The first ARM64 Linux baseline identified a repeatable cost in subscriber
snapshots. Reusing immutable membership between subscription changes reduced
the measured steady-state snapshot/dispatch cost with 1, 8 and 64 subscribers
and removed its per-operation allocation. Empty registries and frequent
membership changes have different costs, shown below. The Session idle polling
interval was not changed; these results do not establish an idle-CPU improvement.

## Scope and environment

The baseline source was `db00c561d57ce846909ccdf23513ccdb814ebb7f`. The `after`
files measure the snapshot implementation accompanying this document. Both
builds used the same benchmark source, which selects `snapshot()` on the baseline
and `snapshot_view()` when available. No benchmark loop or instrumentation was
changed between the two builds.

The recorded environment on 2026-11-03 was:

- Ubuntu 24.04 userspace, Linux `7.0.9-205.fc44.aarch64` kernel.
- Virtualized ARM64 on Apple hardware, 5 available CPUs; process affinity `0-3`.
- Container `cpu.max`: `max 100000` (no CPU quota).
- Clang 18.1.3, libc++, Release `-O3 -DNDEBUG`, C++17,
  `-Wall -Wextra -Wpedantic -Werror`; no LTO or sanitizer instrumentation.

Five samples per implementation were collected in alternating order
`A B B A A B B A A B`, where A is baseline and B is the candidate. Each process
ran with `--iterations 200000 --idle-ms 500 --repetitions 1`. The steady-state
cases performed 200,000 snapshot operations; mutation cases performed 20,001
subscribe/unsubscribe pairs. The idle cases measured 500 ms after a 200 ms
warmup. A separate allocation-instrumented run used three repetitions per build.

Raw CSV files are retained in
[`benchmarks/results/arm64-linux-2026-11-03`](../../benchmarks/results/arm64-linux-2026-11-03).
`before-time-*.csv` and `after-time-*.csv` contain timing samples;
`before-alloc.csv` and `after-alloc.csv` contain allocation samples. The separate
measurement modes are deliberate: global allocation counters add atomic
operations and would overstate the timing benefit of removing allocations.

## Steady-state snapshots

The benchmark acquires a snapshot and invokes every subscriber once. Times are
nanoseconds per snapshot operation, including those callbacks. Values are
median `[minimum, maximum]` over five samples, with allocation tracking **OFF**.

| Subscribers | Baseline ns/op | Candidate ns/op | Median change |
| --- | --- | --- | --- |
| 0 | 10.84 [9.58, 26.31] | 15.90 [10.83, 21.49] | +46.8% |
| 1 | 43.60 [36.97, 64.04] | 31.63 [23.81, 35.93] | −27.5% |
| 8 | 149.62 [124.70, 214.06] | 118.87 [104.27, 124.25] | −20.6% |
| 64 | 879.49 [803.85, 918.87] | 743.79 [663.08, 745.17] | −15.4% |

The 1/8/64-subscriber sample ranges did not overlap in this run. This is an
observed result in this environment, not a confidence interval or a promised
speedup on other systems. The empty-registry median became slower; its ranges
overlapped and both paths allocated zero times after warmup.

With allocation tracking **ON**, every 1/8/64-subscriber baseline operation
allocated one vector buffer, requesting 16/128/1,024 bytes respectively. The
candidate performed zero allocations and requested zero bytes per steady-state
operation. These are requested C++ allocation bytes, not resident-memory usage.

## Membership-change cost

`mutation_snapshot` subscribes one callback, acquires/releases a snapshot,
unsubscribes that callback, then acquires/releases another snapshot. It forces
both membership changes to be observed, exposing cache rebuild costs. The unit
below is one such pair, not one frame or callback. Timing instrumentation is OFF.

| Existing subscribers | Baseline median ns/pair | Candidate median ns/pair |
| --- | --- | --- |
| 0 | 123.17 | 220.41 |
| 1 | 146.28 | 210.06 |
| 8 | 162.04 | 202.56 |
| 64 | 376.86 | 534.56 |

Allocation runs showed **two additional allocations and 96 additional requested
bytes per pair** for the candidate at every tested subscriber count. Plain
subscribe/unsubscribe without acquiring snapshots retained two allocations and
264 requested bytes per pair on both implementations. The optimization trades
membership-rebuild work for lower repeated-dispatch cost; it is intended for
subscriptions that stay in place across many received frames. Applications with
frequent subscription changes should measure their own workload.

## Idle measurements

The idle benchmark creates 0/1/8/32 Sessions with either no route or one retained
inbox per Session. A passive transport waits on a condition variable for 10 ms
and never supplies a frame. It records process CPU time, process context switches
and transport read calls; none of those individually counts Session writer
wakeups. Session creation, warmup and shutdown are outside the measured window.

For example, the 32-Session results, normalized to 500 ms of measured wall time,
were:

| Routes | Baseline CPU ms: median [min, max] | Candidate CPU ms: median [min, max] |
| --- | --- | --- |
| None | 17.627 [9.688, 39.346] | 17.139 [7.996, 37.897] |
| One inbox per Session | 49.523 [7.477, 56.148] | 19.556 [7.273, 25.621] |

The ranges are wide and overlap. Voluntary context-switch medians at 32 Sessions
remained about 8,855 → 8,849 without routes and 8,540 → 8,725 with routes;
transport-read medians remained about 1,588 and 1,567 respectively. The unchanged
idle path and these variations do not support a claim of reduced idle CPU or
fewer wakeups. No idle-loop optimization is included in this change.

## Compatibility and limits

The existing `snapshot()` copy API remains available. Dispatchers and managed
buses use an immutable shared membership snapshot, rebuilt after subscription
changes. Entry state still uses its own mutex and cancellation barrier. Callback
order, self-unsubscribe, external unsubscribe waiting for in-flight callbacks,
concurrent clear/unsubscribe, and automatic disabling after callback failures
retain their existing semantics. This performance result does not justify
relaxing any of those guarantees.

The registry has additional cache state, changing the layout of classes that
embed it. Downstream code must rebuild against matching headers and libraries;
do not mix old compiled consumers with the new headers/library. There are no
configuration or persistent-record migrations.

These are direct registry microbenchmarks, not end-to-end CAN throughput,
device latency or a hardware acceptance result. The test uses one snapshot
reader, no concurrent subscriber mutation during steady-state samples and a
virtualized host. Five samples are insufficient for broad statistical claims.
Allocator instrumentation counts successful global C++ new/new[] calls, not
direct C allocation, kernel memory, deallocation or peak live memory. See the
benchmark README for the full measurement boundary.

## Reproduce and inspect

Local validation on ARM64 Linux passed 217 offline tests with Clang 18/libc++,
217 with TSan, and 215 with ASan/UBSan (the two native loader fixtures are
excluded from the ASan run, as in CI). The single-threaded allocation-failure
fixture disables TSan because both it and TSan replace global allocation
functions; concurrent snapshot and lifecycle tests remain instrumented.
Nineteen TSan snapshot, cancellation and Session lifecycle cases also passed
20 consecutive repetitions each. GCC production line coverage was
3,655/3,816 lines (95.8%). These are local results; PR CI is a separate check.

The allocation-failure regression returned failure on the baseline and passed
after the fix: a failed allocation while preparing an unsubscribe must leave
the subscription live and retryable. Installed-package consumption, formatting
and the changed C++ translation units' clang-tidy checks were also verified.

Use two source trees containing the same benchmark source and the desired
baseline/candidate implementations. Build timing binaries with allocation
tracking OFF; build separate allocation binaries with it ON:

```sh
cmake -S benchmarks -B build/perf -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror' \
  -DLINKERBOT_PERF_COUNT_ALLOCATIONS=OFF
cmake --build build/perf --parallel 4
taskset -c 0-3 ./build/perf/linkerbot-perf \
  --iterations 200000 --idle-ms 500 --repetitions 1

cmake -S benchmarks -B build/perf-alloc -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror' \
  -DLINKERBOT_PERF_COUNT_ALLOCATIONS=ON
cmake --build build/perf-alloc --parallel 4
taskset -c 0-3 ./build/perf-alloc/linkerbot-perf \
  --iterations 200000 --idle-ms 500 --repetitions 3

python3 benchmarks/summarize.py benchmarks/results/arm64-linux-2026-11-03
```

Keep each process's CSV output, tool versions, affinity and run order. The summary
validates callback/membership checksums and compares timing only where
`allocation_tracking=0`; allocation totals use only `allocation_tracking=1`.
