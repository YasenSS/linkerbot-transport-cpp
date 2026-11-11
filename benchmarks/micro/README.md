# Linux performance baselines

Run these build commands from a complete source checkout. Installed packages
include the measurement notes, recorded baseline CSVs and summary script, but not
the benchmark build sources. The source lives in `benchmarks/micro/`; run build
commands from the repository root. Configure this standalone project in Release mode;
it needs no Catch2 or hardware:

```sh
cmake -S benchmarks -B build/perf -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/perf --parallel 4
./build/perf/linkerbot-perf --iterations 200000 --idle-ms 500 --repetitions 5
```

The default `LINKERBOT_PERF_COUNT_ALLOCATIONS=OFF` build is for timing and CPU
measurements. It does not override global allocation functions or update allocation
counters; its CSV rows have `allocation_tracking=0` and `NA` in both allocation
columns. Build a separate instrumented executable for allocation measurements:

```sh
cmake -S benchmarks -B build/perf-alloc -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLINKERBOT_PERF_COUNT_ALLOCATIONS=ON
cmake --build build/perf-alloc --parallel 4
./build/perf-alloc/linkerbot-perf --iterations 200000 --idle-ms 500 --repetitions 5
```

Instrumented rows have `allocation_tracking=1`. Use them for allocation counts,
not for the primary performance comparison: atomic counter updates add overhead
only to operations that allocate, and can exaggerate the benefit of removing
allocations. Compare before/after timing with instrumentation OFF, and compare
before/after allocation counts with it ON. Keep all remaining build and run
conditions the same within each comparison.

Redirect stdout to a CSV file in a new ignored `benchmarks/results/micro-YYYYMMDD-HHMMSS/` directory. Keep the source commit,
compiler/standard-library versions, flags, kernel, CPU architecture, affinity and
container CPU limits with each result. Run before and after binaries under the
same conditions, preferably in alternating order. Do not compare sanitizer builds.

`snapshot_copy` or `snapshot_view` measures one registry snapshot plus invocation of every subscriber;
`operations` is the number of snapshots, not callbacks. `checksum` must equal
`subjects * operations`. `mutation` measures a subscribe/unsubscribe pair while
holding the stated number of existing subscribers. These direct registry cases
isolate software overhead and do not measure CAN throughput or scheduler latency.
`mutation_snapshot` additionally acquires and releases a snapshot after each
subscribe and each unsubscribe, exposing cold snapshot-rebuild costs. Each
operation is still one subscribe/unsubscribe pair; its checksum must equal
`operations * (2 * subjects + 1)`. Plain `mutation` does not acquire snapshots.
The same source automatically selects `snapshot_view()` when the revision provides
it, otherwise `snapshot()`. The CSV scenario records the selected path; compare
these rows by subscriber count and repetition. The invocation loop and all other
instrumentation remain identical.

Alternatively, the parent project may enable its optional benchmark target via
`-DLINKERBOT_TRANSPORT_BUILD_BENCHMARKS=ON`; the standalone project does not force the
parent project's test/example choices when the library target already exists.

`idle_unrouted` creates 0/1/8/32 Sessions with no inbox; `idle_routed` also retains
one subscribed inbox per Session, without injecting any frames. Each Session
owns one passive transport. Its reader
uses a 10 ms condition-variable timeout. The zero-Session case measures the
baseline cost of the measuring process. A 200 ms warmup precedes each sample;
creation and shutdown are excluded. `transport_reads` separately identifies
reader activity, so it must not be mistaken for writer wakeups.

All times are nanoseconds. `cpu_ns` and context switches come from process-wide
Linux counters and include every benchmark thread. Voluntary switches are a
proxy for scheduling activity, not an exact count of Session wakeups. Allocation
counts/bytes include successful C++ global new/new[] calls (including aligned
forms) during measurement; requested bytes do not include allocator metadata,
direct C malloc, kernel allocations or resident memory. Atomic instrumentation
itself adds overhead. The instrumented benchmark does not intercept deallocation
or measure peak live memory. Never compare an instrumented binary's time with an
uninstrumented binary's time.

Warmup, setup, output and cleanup are outside allocation measurement. CSV keeps
raw per-repetition results; report medians and dispersion rather than one fastest
sample. Preserve callback ordering, cancellation barriers, self-unsubscribe,
concurrent unsubscribe/clear, and cleanup/recovery behavior when optimizing.
