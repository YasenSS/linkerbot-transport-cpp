# CI performance measurements, 2026-11-04

[CI policy](ci.md) · [Machine-readable measurements and every run URL](ci-performance-2026-11-04.json)

These measurements evaluate analyzer concurrency and fixed Windows CMake caching.
They do not measure application throughput, allocations or physical hardware.
No runtime source, analyzer rules, translation-unit coverage, Ubuntu/architecture matrix,
ROS1 integration, sanitizer, coverage threshold or Windows CRT configuration was removed.

## Method

Analyzer comparisons use the same source commit
`aa9be29113cb4ce8b75571c3b4603a654811eeed`, three successful runs per setting and workflow,
on hosted `ubuntu-24.04-arm`. Every evidence record reports four available CPUs and the
requested effective concurrency, either two or four. The source configuration SHA256 is
`7f9a119cee1e4a5365ea449e89c85fdfbf7d3105ac01b52b124f787c42a94477`.
Generated analysis configurations differ only in `jobs`.

Use GitHub REST step timestamps for analysis duration. Style duration below sums its two
main clang-tidy steps; its fixture and installed-consumer checks also remain enabled.
Report duration is the complete `Generate quality report` step, including its small action
setup overhead. Wall duration is the last non-skipped job completion minus `run_started_at`;
it includes setup and scheduling, not just analysis. Different hosted VMs and network/cache
conditions introduce noise. These are paired configurations, not identical physical machines.

## Analyzer results

All durations are seconds. Lists retain all three observations, including setup outliers.

| Measurement | Two workers | Four workers | Median reduction |
| --- | --- | --- | --- |
| Style main clang-tidy steps | 165, 152, 158; median 158 | 97, 94, 91; median 94 | 40.5% |
| Style workflow wall | 213, 205, 208; median 208 | 140, 221, 161; median 161 | 22.6% |
| Full report generation | 326, 302, 327; median 326 | 186, 202, 196; median 196 | 39.9% |
| Full report workflow wall | 369, 343, 369; median 369 | 248, 245, 232; median 245 | 33.6% |

The second four-worker Style run installed tools for 75 seconds, versus 16–25 seconds in
several neighboring runs. Its wall time was slower than its two-worker counterpart even
though analysis was faster. No outlier was discarded.

All six complete reports passed A+ with clang-format 18.1.3 checking 56/56 files,
Cppcheck 2.13.0 checking 39/39 units and clang-tidy 18.1.3 checking 39/39 units.
Style also passed every original check. GNU time reports approximately 384,000 kbytes maximum
RSS for the project pass and 633,500 kbytes for the test pass in both settings. This statistic
is not an aggregate peak for all simultaneous analyzer processes; four-worker total memory
was not independently profiled. The worker count is bounded by available CPU affinity.

## Windows CMake cache

One forced-cold run and one warmed run use the same source commit
`9a7ac6428c2a1d347a02532c5d3919156323302c` and the same four native configurations.
A separate successful run populated the cache first. Runs were serial to preserve the
existing cancellation policy. This single cold/warm pair is evidence of tool-cache benefit,
not a statistically established whole-workflow speedup.

Setup below sums checksum retrieval, cache restore, full installation verification/download
and cache save where applicable. Integrity-test and source-recording steps still execute.

| Native job | Cold setup seconds | Warm setup seconds |
| --- | --- | --- |
| x64 Debug | 21 | 9 |
| x64 Release | 20 | 9 |
| ARM64 Debug | 45 | 21 |
| ARM64 Release | 44 | 20 |

The observed workflow wall times were 271 and 232 seconds. All eight native jobs passed
regression, executable/DLL machine checks, packaging and relocated consumer checks.
All four warmed artifacts recorded exact cache hits and verified reuse. Both x64 installations
verified 8,032 files against the independently authenticated CMake 3.31.8 ZIP; both ARM64
installations verified 8,525 files against CMake 4.2.3. Restore overhead is included above;
the ARM64 cache restore itself took 13–14 seconds. No MSVC build directory is cached.

## Release promotion and remaining validation

Tag pushes still build all twelve SDKs and run all eight Focal cross-distribution consumer
jobs and both Noetic node/nodelet jobs. The final job binds the original tag object, source,
attempt, successful jobs and original artifact IDs/digests in a validation record. Explicit
manual draft promotion revalidates those same bytes instead of repeating the entire matrix.
Artifacts are attempt-scoped; a complete rerun can retain older attempts, while partial or
mixed-attempt promotion is rejected. The manifest continues to point at the original
validation run. See [promotion and recovery commands](ci.md#release-workflow).

The historical v0.3.2 [tag-validation](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41822738061)
and [draft](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41823543022) runs consumed 1,482 and 1,529 cumulative
job-seconds respectively. Removing a duplicate matrix removes that class of work, but the
new seal/download/revalidation overhead has not yet been measured on a new production tag.
Do not treat those historical totals as a measured net saving of the new workflow.

A read-only smoke test of that tag-validation run downloaded the twelve real v0.3.2 artifact ZIPs (11,620,608 bytes),
validated their API digests and all 84 ELF/COFF objects. Local timings were 11.163 seconds
for metadata, 52.872 seconds for download/digest/unpack and 0.185 seconds for package
validation. These local network timings are not comparable to hosted-runner build timings.
The production reuse path correctly rejected that legacy run because it has no new seal.
Mocked-API integration regressions exercise successful seal/reuse, tag movement, artifact
changes, complete reruns retaining old packages, missing packages and mixed-attempt rejection.

The real new-tag seal-to-draft path remains an explicit post-merge validation requirement
for the next new version. No existing tag or published release was modified to test it.
Prebuilt Ubuntu/Noetic tool images, MSVC compilation caching and consumer aggregation are
deferred pending separate net-benefit measurements; all current distribution and ROS1 checks
remain enabled. The [existing complete Ubuntu/ROS1 run](https://github.com/linker-bot/linkerbot-transport-cpp/actions/runs/41821652955) took approximately two minutes,
so these changes prioritize the measured analyzer bottleneck and duplicate release work.
