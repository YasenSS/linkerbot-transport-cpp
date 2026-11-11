# 性能测试 / Benchmarks

本目录包含两类测试，均需完整源码仓库；软件测试结果不代表物理 CAN 带宽。

- [`micro/`](micro/README.md)：内部操作基准，测订阅快照、订阅变更和 Session 空闲开销。`main.cpp` 输出 CSV，`summarize.py` 比较修改前后耗时、CPU 和分配次数。
- [`stress/`](stress/README.md)：完整通信链路压测，扫描内存/vcan 的经典 CAN 与 CAN FD，逐帧校验，输出吞吐、P99、CPU、RSS、丢帧及性能对照报告。
- `results/`：结果集中存放。每次完整压测自动新建目录，避免覆盖；本机生成结果由 Git 忽略。已跟踪的基准记录和选定正式压测结果保留。

已保存的 Ubuntu 正式结果：[Transport 0.3.4 压测报告（英文）](results/stress-20261110-152431/stress-report.md) · [原始数据包](results/stress-20261110-152431/stress-raw-data.tar.gz) · [英文归档校验说明](results/stress-20261110-152431/README.md)。测试于 2026-11-10 完成，共 119 档，四类吞吐均通过两次 60 秒确认。被测通信库提交为 `0e190032d65ed0b315337b45b3779cf4f650ed68`，运行发生在工具迁入仓库之前；报告和压缩包保留当时的版本、路径及计数，中文说明已翻译为英文。

从通信库根目录运行完整压测：

```bash
python3 benchmarks/stress/run.py --quick   # 快速验证，非正式结果
python3 benchmarks/stress/run.py           # 正式流程，约 40 分钟
```

默认结果为 `results/quick-YYYYMMDD-HHMMSS/` 或 `results/stress-YYYYMMDD-HHMMSS/`（相对于 benchmarks）。每个结果目录仅保留报告和原始数据压缩包；构建和展开采样均使用临时目录。允许用 `--output DIR` 指定其他位置。

内部基准的现有构建方式保持不变：

```bash
cmake -S benchmarks -B build/perf -DCMAKE_BUILD_TYPE=Release
cmake --build build/perf --parallel 4
mkdir -p benchmarks/results/micro-example
./build/perf/linkerbot-perf --iterations 200000 --idle-ms 500 --repetitions 5 \
  > benchmarks/results/micro-example/before-timing.csv
```

完整的两种分配计数模式与 before/after 比较流程见 [micro/README.md](micro/README.md)。需要同时构建完整压测程序时，可加 `-DLINKERBOT_PERF_BUILD_STRESS=ON`；通常由 `stress/run.py` 单独构建，保证每次使用新的 Release 产物。

安装包仅包含测量说明、micro 汇总脚本和已跟踪的 ARM64 基准记录，不包含测试构建源码、本机 stress/quick 结果或 `results/history/`。

历史结果整理至 `results/history/`，保留当时的版本、数值和命令。新结果不替代历史证据。原有压缩备份仍位于工作区的 `archives/`，不纳入通信库发布包。
