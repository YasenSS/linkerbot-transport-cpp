# Transport 0.3.4 Stress Test Report

## Ubuntu System Stress Test

### Test Information

| Item | Details |
| --- | --- |
| Test time | 2026-11-10 14:45:57+08:00 to 2026-11-10 15:24:31+08:00 |
| Status | Formal run completed; all four lossless confirmations are valid; 119 configurations, 6 failed; failed configurations are excluded from lossless results |
| System | Ubuntu 25.10, Linux 6.17.0-41-generic, x86_64 |
| CPU | AMD Ryzen 7 H 260, 8 physical cores / 16 logical CPUs |
| Build | c++ (Ubuntu 15.2.0-4ubuntu4) 15.2.0; C++17; Release (`-O3 -DNDEBUG`) |
| Tested version | Transport 0.3.4, commit `0e190032d65ed0b315337b45b3779cf4f650ed68` |
| Source state | Clean working tree; unchanged throughout the test |

Full environment details, source fingerprints and linked-library checksums are in the [raw data archive](stress-raw-data.tar.gz).

### Test Method

The selected local source was compiled directly in a fresh temporary directory, without fetching remote changes or using release packages. Memory loopback and vcan in an isolated network namespace were tested with 8-byte Classic CAN frames and 64-byte CAN FD+BRS frames.
The scan used 1/2/4/8/16/32 sender threads, 1/8 frames per batch and an initial window of 8 batches per thread. Additional runs used 1/8 threads, 32/128 frames per batch and a 1-batch window. The fastest candidate was then tested with windows of 1/4/16/32 batches (at most 4096 frames and 1024 batches in flight). Each screening run lasted 15 seconds. The two fastest lossless candidates were retested for 30 seconds, then the better candidate underwent two consecutive 60-second confirmations; the second candidate was tried if confirmation failed.
Each thread submits the batches within its window, waits for send receipts and all replies in order, then continues. Validation checks IDs, frame flags, lengths, sequence numbers and payload patterns. Throughput is the number of correctly validated replies divided by actual elapsed seconds; one request plus one reply counts as one complete round trip. All 8 Classic CAN bytes carry the sequence number and timestamp; the other 56 FD bytes are also checked individually.
Normal vcan throughput runs requested a 4 MiB receive buffer at both endpoints, changing only the sockets used by this test. Controls covered default-buffer overload at 8 threads x 8 frames x 8 batches, larger buffers, smaller windows, thread counts at equal in-flight limits, and per-frame versus batched peers with the same FD configuration.
P99 is the upper bound of a 10-microsecond histogram bucket (the final bucket uses the actual maximum). CPU 100% means one logical CPU; process CPU includes the stress driver, communication library and simulated peer, with kernel CPU time counted as system time. RSS covers the whole process. Software loopback results do not represent physical CAN bandwidth.

### Test Results

- **Memory loopback Classic CAN:** Two confirmations: **1.1358 / 1.1351 million complete round trips/s**; 8 threads, 128 frames/batch, a 4-batch window per thread, at most 4096 frames in flight. Validated counts: **68150272 / 68111360**; P99: **4.249 / 4.289 ms**; CPU: **2.31 / 2.31 logical CPUs**; RSS sample range: **7.36-7.64 MiB**; library overflows: **0 / 0**; kernel drops: **Not applicable (no kernel sockets)**. Raw records: `020-confirm-memory-can-t8-b128-w4`, `021-confirm-memory-can-t8-b128-w4`.
- **Memory loopback CAN FD:** Two confirmations: **1.2885 / 1.2773 million complete round trips/s**; 1 thread, 128 frames/batch, a 16-batch window per thread, at most 2048 frames in flight. Validated counts: **77314048 / 76638208**; P99: **1.909 / 1.919 ms**; CPU: **2.03 / 2.03 logical CPUs**; RSS sample range: **5.97-6.10 MiB**; library overflows: **0 / 0**; kernel drops: **Not applicable (no kernel sockets)**. Raw records: `080-confirm-memory-fd-t1-b128-w16`, `081-confirm-memory-fd-t1-b128-w16`.
- **vcan Classic CAN:** Two confirmations: **0.3306 / 0.3337 million complete round trips/s**; 8 threads, 8 frames/batch, a 8-batch window per thread, at most 512 frames in flight. Validated counts: **19838848 / 20024256**; P99: **2.009 / 2.029 ms**; CPU: **3.59 / 3.61 logical CPUs**; RSS sample range: **5.96-6.01 MiB**; library overflows: **0 / 0**; kernel drops: **Session 0 / 0, peer 0 / 0**. Raw records: `045-confirm-vcan-can-t8-b8-w8`, `046-confirm-vcan-can-t8-b8-w8`.
- **vcan CAN FD:** Two confirmations: **0.3258 / 0.3265 million complete round trips/s**; 8 threads, 32 frames/batch, a 1-batch window per thread, at most 256 frames in flight. Validated counts: **19548352 / 19591904**; P99: **0.979 / 0.959 ms**; CPU: **3.29 / 3.28 logical CPUs**; RSS sample range: **5.88-6.04 MiB**; library overflows: **0 / 0**; kernel drops: **Session 0 / 0, peer 0 / 0**. Raw records: `103-confirm-vcan-fd-t8-b32-w1`, `104-confirm-vcan-fd-t8-b32-w1`.

Actual socket receive buffers (kernel-reported values, which may be twice the requested size):

- vcan Classic CAN: session=8388608 B/peer=8388608 B; session=8388608 B/peer=8388608 B.
- vcan CAN FD: session=8388608 B/peer=8388608 B; session=8388608 B/peer=8388608 B.

### Performance Analysis

#### 1. Frame Loss Under High Concurrency

- Classic CAN `047-overload-vcan-can-t8-b8-w8`: FAIL; 8 x 8 x 8, default buffers (session=212992 B, peer=212992 B); submitted/library TX/peer RX/peer TX/library RX/validated=1408/1408/1331/1331/1331/1262; kernel drops: Session=0, peer=77; library overflows=0.
- Classic CAN `048-overload-vcan-can-t8-b8-w8`: FAIL; 8 x 8 x 8, default buffers (session=212992 B, peer=212992 B); submitted/library TX/peer RX/peer TX/library RX/validated=3648/3648/3640/3640/3640/3572; kernel drops: Session=0, peer=8; library overflows=0.
- Classic CAN `049-overload-vcan-can-t8-b8-w8`: FAIL; 8 x 8 x 8, default buffers (session=212992 B, peer=212992 B); submitted/library TX/peer RX/peer TX/library RX/validated=5696/5696/5674/5674/5674/5593; kernel drops: Session=0, peer=22; library overflows=0.
- Classic CAN `050-buffer-control-vcan-can-t8-b8-w8`: PASS; 8 x 8 x 8, large buffers (session=8388608 B, peer=8388608 B); submitted/library TX/peer RX/peer TX/library RX/validated=9973888/9973888/9973888/9973888/9973888/9973888; kernel drops: Session=0, peer=0; library overflows=0.
- Classic CAN `051-window-control-vcan-can-t8-b8-w1`: PASS; 8 x 8 x 1, default buffers (session=212992 B, peer=212992 B); submitted/library TX/peer RX/peer TX/library RX/validated=4894744/4894744/4894744/4894744/4894744/4894744; kernel drops: Session=0, peer=0; library overflows=0.

Classic CAN: Kernel counters confirmed socket receive-queue drops at the simulated peer receiver. Both larger-buffer and smaller-window controls passed, supporting burst receive-queue overload as the explanation. No library overflows were observed in these overload runs.

- CAN FD `105-overload-vcan-fd-t8-b8-w8`: FAIL; 8 x 8 x 8, default buffers (session=212992 B, peer=212992 B); submitted/library TX/peer RX/peer TX/library RX/validated=3776/3776/3711/3711/3711/3450; kernel drops: Session=0, peer=65; library overflows=0.
- CAN FD `106-overload-vcan-fd-t8-b8-w8`: FAIL; 8 x 8 x 8, default buffers (session=212992 B, peer=212992 B); submitted/library TX/peer RX/peer TX/library RX/validated=1856/1856/1831/1831/1815/1686; kernel drops: Session=16, peer=25; library overflows=0.
- CAN FD `107-overload-vcan-fd-t8-b8-w8`: FAIL; 8 x 8 x 8, default buffers (session=212992 B, peer=212992 B); submitted/library TX/peer RX/peer TX/library RX/validated=1472/1472/1472/1472/1447/1367; kernel drops: Session=25, peer=0; library overflows=0.
- CAN FD `108-buffer-control-vcan-fd-t8-b8-w8`: PASS; 8 x 8 x 8, large buffers (session=8388608 B, peer=8388608 B); submitted/library TX/peer RX/peer TX/library RX/validated=9763648/9763648/9763648/9763648/9763648/9763648; kernel drops: Session=0, peer=0; library overflows=0.
- CAN FD `109-window-control-vcan-fd-t8-b8-w1`: PASS; 8 x 8 x 1, default buffers (session=212992 B, peer=212992 B); submitted/library TX/peer RX/peer TX/library RX/validated=4938600/4938600/4938600/4938600/4938600/4938600; kernel drops: Session=0, peer=0; library overflows=0.

CAN FD: Kernel counters confirmed socket receive-queue drops at the simulated peer receiver, Session receiver. Both larger-buffer and smaller-window controls passed, supporting burst receive-queue overload as the explanation. No library overflows were observed in these overload runs.

#### 2. Performance Bottlenecks

- Classic CAN: 2 threads x 8 frames x 8 batches, at most 128 frames in flight, 2 valid control runs; mean 0.3283 million round trips/s, mean P99 0.539 ms, CPU 3.49 logical CPUs, peer CPU 0.99 logical CPUs.
- Classic CAN: 8 threads x 8 frames x 2 batches, at most 128 frames in flight, 2 valid control runs; mean 0.3299 million round trips/s, mean P99 0.509 ms, CPU 3.64 logical CPUs, peer CPU 0.99 logical CPUs.
- Classic CAN: 8 threads x 8 frames x 8 batches, at most 512 frames in flight, 2 valid control runs; mean 0.3333 million round trips/s, mean P99 1.989 ms, CPU 3.64 logical CPUs, peer CPU 0.99 logical CPUs.

Classic CAN: At the same 128-frame in-flight limit, increasing threads from 2 to 8 changed throughput by +0.5%. With 8 threads, increasing the limit from 128 to 512 frames changed throughput by +1.0%, while mean P99 increased from 0.509 to 1.989 ms. Scheduling and run-to-run variation affect these differences; they support only comparisons between the tested configurations.

- CAN FD: 2 threads x 8 frames x 8 batches, at most 128 frames in flight, 2 valid control runs; mean 0.3292 million round trips/s, mean P99 0.529 ms, CPU 3.51 logical CPUs, peer CPU 0.99 logical CPUs.
- CAN FD: 8 threads x 8 frames x 2 batches, at most 128 frames in flight, 2 valid control runs; mean 0.3327 million round trips/s, mean P99 0.504 ms, CPU 3.66 logical CPUs, peer CPU 0.99 logical CPUs.
- CAN FD: 8 threads x 8 frames x 8 batches, at most 512 frames in flight, 2 valid control runs; mean 0.3359 million round trips/s, mean P99 2.009 ms, CPU 3.64 logical CPUs, peer CPU 0.99 logical CPUs.

CAN FD: At the same 128-frame in-flight limit, increasing threads from 2 to 8 changed throughput by +1.1%. With 8 threads, increasing the limit from 128 to 512 frames changed throughput by +1.0%, while mean P99 increased from 0.504 to 2.009 ms. Scheduling and run-to-run variation affect these differences; they support only comparisons between the tested configurations.

- FD, identical 2 x 8 x 8 configuration with large buffers, library peer, per-frame: 0.3279 million round trips/s, P99 0.549 ms, peer CPU 0.99 logical CPUs (`116-peer-comparison-vcan-fd-t2-b8-w8`).
- FD, identical 2 x 8 x 8 configuration with large buffers, raw socket peer, per-frame: 0.3515 million round trips/s, P99 0.489 ms, peer CPU 0.99 logical CPUs (`117-peer-comparison-vcan-fd-t2-b8-w8`).
- FD, identical 2 x 8 x 8 configuration with large buffers, raw socket peer, batches of 32 frames: 0.3972 million round trips/s, P99 0.419 ms, peer CPU 0.98 logical CPUs (`118-peer-comparison-vcan-fd-t2-b8-w8`).

Changing only the raw socket peer from per-frame to batched processing changed throughput by +13.0% in this run. The test peer implementation affects observed throughput.
No function-level or lock profiling evidence was collected, so a unique bottleneck cannot be identified. The best result in this finite parameter search is not an absolute throughput limit of the communication library. RSS describes only the observed finite duration and does not prove freedom from long-term leaks.

### Failed Configurations and Integrity

- `047-overload-vcan-can-t8-b8-w8`: Exit code 1; Stress validation failed or JSON missing; peer kernel drops 77. Raw stdout/log: `047-overload-vcan-can-t8-b8-w8.json` / `047-overload-vcan-can-t8-b8-w8.log`.
- `048-overload-vcan-can-t8-b8-w8`: Exit code 1; Stress validation failed or JSON missing; peer kernel drops 8. Raw stdout/log: `048-overload-vcan-can-t8-b8-w8.json` / `048-overload-vcan-can-t8-b8-w8.log`.
- `049-overload-vcan-can-t8-b8-w8`: Exit code 1; Stress validation failed or JSON missing; peer kernel drops 22. Raw stdout/log: `049-overload-vcan-can-t8-b8-w8.json` / `049-overload-vcan-can-t8-b8-w8.log`.
- `105-overload-vcan-fd-t8-b8-w8`: Exit code 1; Stress validation failed or JSON missing; peer kernel drops 65. Raw stdout/log: `105-overload-vcan-fd-t8-b8-w8.json` / `105-overload-vcan-fd-t8-b8-w8.log`.
- `106-overload-vcan-fd-t8-b8-w8`: Exit code 1; Stress validation failed or JSON missing; session kernel drops 16; peer kernel drops 25. Raw stdout/log: `106-overload-vcan-fd-t8-b8-w8.json` / `106-overload-vcan-fd-t8-b8-w8.log`.
- `107-overload-vcan-fd-t8-b8-w8`: Exit code 1; Stress validation failed or JSON missing; session kernel drops 25. Raw stdout/log: `107-overload-vcan-fd-t8-b8-w8.json` / `107-overload-vcan-fd-t8-b8-w8.log`.

Raw data: `stress-raw-data.tar.gz` in the same directory. The archive contains this report, per-configuration JSON/CSV/logs, socket counters, environment records, source fingerprints, tool sources, compiler options and the linked library. The original collection process verified per-file SHA256 checksums before removing temporary files.

This English edition translates the report, diagnostic descriptions and the archived tool snapshot. Measurement values, CSV samples, logs, environment metadata and compiled artifacts are unchanged. Original hashes and the translation scope are recorded in `localization.json`; `manifest.json` contains the updated archive checksums. The translated tool snapshot is not byte-identical to the tool used for the original measurement.
