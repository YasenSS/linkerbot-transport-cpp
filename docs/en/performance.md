# Performance measurements and snapshot tradeoffs

English · [简体中文](../zh/performance.md)

[Documentation index](../README.md) · [Build guide](build.md) ·
[Benchmark source and measurement rules](../../benchmarks/micro/README.md)

The ARM64 Linux measurements identified per-operation allocation in subscriber
snapshots. Reusing immutable membership between subscription changes removed
that allocation and lowered the measured median steady-state snapshot/dispatch
time with 1, 8 and 64 subscribers in this sample set. Empty registries and frequent
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
| 0 | 15.91 [14.08, 19.04] | 18.61 [13.38, 20.60] | +17.02% |
| 1 | 63.08 [50.96, 67.53] | 42.00 [30.95, 73.78] | −33.42% |
| 8 | 250.03 [168.08, 252.08] | 214.09 [150.41, 254.45] | −14.37% |
| 64 | 1656.21 [1076.82, 1735.97] | 1359.03 [924.90, 1689.64] | −17.94% |

The baseline and candidate sample ranges overlap at every subscriber count.
The lower 1/8/64-subscriber medians are observations in this environment, not
confidence intervals or promised speedups. The empty-registry median became
slower. Allocation counts provide a separate result: both empty paths allocated
zero times after warmup, while nonempty steady-state allocations were eliminated.

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
| 0 | 199.37 | 276.05 |
| 1 | 228.20 | 303.28 |
| 8 | 284.17 | 378.54 |
| 64 | 750.24 | 1117.54 |

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
| None | 23.978 [17.171, 25.922] | 28.655 [16.303, 43.576] |
| One inbox per Session | 22.776 [20.268, 32.809] | 24.990 [19.911, 29.540] |

The ranges are wide and overlap. Voluntary context-switch medians at 32 Sessions
remained about 8,646 → 8,719 without routes and 8,590 → 8,714 with routes;
transport-read medians remained about 1,567 in both cases. The unchanged
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

python3 benchmarks/micro/summarize.py benchmarks/results/arm64-linux-2026-11-03
```

Keep each process's CSV output, tool versions, affinity and run order. The summary
validates callback/membership checksums and compares timing only where
`allocation_tracking=0`; allocation totals use only `allocation_tracking=1`.
