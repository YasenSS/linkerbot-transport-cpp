# 性能测量与订阅者快照的取舍

[English](../en/performance.md) · 简体中文

[文档索引](../README.md) · [构建指南](build.md) ·
[基准源码与测量规则](../../benchmarks/micro/README.md)

ARM64 Linux 测量确认了订阅者快照每次操作的分配。在订阅关系不变时复用不可变成员快照，
消除了该分配；在这组样本中，1、8、64 个订阅者下的稳态快照／分发耗时中位数降低。
空订阅表和频繁修改订阅关系的成本不同，具体数据见下文。本轮未修改 Session 的空闲轮询间隔；
这些结果不能证明空闲 CPU 占用有所改善。

## 范围与环境

基线源码为 `db00c561d57ce846909ccdf23513ccdb814ebb7f`。`after` 文件测量的是与本文一同提交的
快照实现。两种构建使用完全相同的基准源码：基线选择 `snapshot()`，存在 `snapshot_view()`
时选择后者。两次构建之间没有修改基准循环或测量代码。

2026-11-03 记录的环境为：

- Ubuntu 24.04 用户空间，Linux `7.0.9-205.fc44.aarch64` 内核。
- Apple 硬件上的虚拟化 ARM64，5 个可用 CPU；进程亲和性为 `0-3`。
- 容器 `cpu.max` 为 `max 100000`，无 CPU 配额限制。
- Clang 18.1.3、libc++、Release `-O3 -DNDEBUG`、C++17，
  `-Wall -Wextra -Wpedantic -Werror`；未使用 LTO 或 sanitizer 插桩。

每种实现采集五个样本，交替顺序为 `A B B A A B B A A B`，A 表示基线，B 表示候选实现。
每个进程使用 `--iterations 200000 --idle-ms 500 --repetitions 1`。稳态场景执行 200,000 次
快照操作，修改订阅场景执行 20,001 次订阅／退订对；空闲场景预热 200 ms 后测量 500 ms。
分配插桩构建另行运行，每种实现采集三次重复。

原始 CSV 保存在
[`benchmarks/results/arm64-linux-2026-11-03`](../../benchmarks/results/arm64-linux-2026-11-03)。
`before-time-*.csv`、`after-time-*.csv` 是耗时样本；`before-alloc.csv`、`after-alloc.csv`
是分配样本。两种测量模式刻意分开：全局分配计数器会增加原子操作，若用其耗时对比，
会夸大消除分配的收益。

## 稳态快照

基准每次取得快照，然后调用每个订阅者一次。耗时单位为 ns／快照操作，包含这些回调。
以下为五个样本的中位数 `[最小值, 最大值]`，分配统计处于 **OFF** 状态。

| 订阅者数 | 基线 ns/op | 候选 ns/op | 中位数变化 |
| --- | --- | --- | --- |
| 0 | 15.91 [14.08, 19.04] | 18.61 [13.38, 20.60] | +17.02% |
| 1 | 63.08 [50.96, 67.53] | 42.00 [30.95, 73.78] | −33.42% |
| 8 | 250.03 [168.08, 252.08] | 214.09 [150.41, 254.45] | −14.37% |
| 64 | 1656.21 [1076.82, 1735.97] | 1359.03 [924.90, 1689.64] | −17.94% |

所有订阅数量下，基线与候选的样本范围均有重叠。1／8／64 个订阅者的中位数降低只是
该环境下的观测结果，不是置信区间，也不承诺其他系统获得相同提升。空订阅表的中位耗时
变慢。分配计数是独立的结果：两种空订阅表路径预热后均不分配，非空稳态快照的分配则被消除。

分配统计处于 **ON** 状态时，基线在 1／8／64 个订阅者下每次操作都分配一个 vector 缓冲区，
分别请求 16／128／1,024 字节；候选实现的稳态每次操作分配次数和请求字节数均为零。
这里统计的是 C++ 分配请求字节数，不是常驻内存占用。

## 修改订阅关系的成本

`mutation_snapshot` 每次订阅一个回调，取得并释放快照，退订该回调，再取得并释放另一个快照。
它强制观察两次成员变化，用于暴露缓存重建成本。下表单位是这样的一对操作，不是一帧或一次回调。
耗时测量未启用分配插桩。

| 原有订阅者数 | 基线中位 ns/对 | 候选中位 ns/对 |
| --- | --- | --- |
| 0 | 199.37 | 276.05 |
| 1 | 228.20 | 303.28 |
| 8 | 284.17 | 378.54 |
| 64 | 750.24 | 1117.54 |

分配样本表明，在所有被测订阅数量下，候选实现每对操作**多两次分配、多请求 96 字节**。
只订阅／退订而不取得快照的场景，两种实现都保持每对两次分配、请求 264 字节。
该优化将部分成本转移到成员快照重建，适合订阅关系在多帧接收期间保持稳定的负载。
频繁改变订阅关系的应用应测量自己的使用方式。

## 空闲测量

空闲场景创建 0／1／8／32 个 Session，分别测试无路由和每个 Session 保留一个 inbox。
被动 transport 使用 10 ms 条件变量等待，始终不提供帧。基准记录进程 CPU 时间、
进程上下文切换和 transport 读取次数；其中任何一项都不能单独等同于 Session writer 唤醒次数。
Session 创建、预热和退出不计入测量窗口。

例如，32 个 Session 的结果按实际墙钟时间归一化到 500 ms 后为：

| 路由 | 基线 CPU ms：中位数 [最小, 最大] | 候选 CPU ms：中位数 [最小, 最大] |
| --- | --- | --- |
| 无 | 23.978 [17.171, 25.922] | 28.655 [16.303, 43.576] |
| 每个 Session 一个 inbox | 22.776 [20.268, 32.809] | 24.990 [19.911, 29.540] |

样本范围较宽且相互重叠。32 个 Session 的自愿上下文切换中位数，无路由时约为
8,646 → 8,719，有路由时约为 8,590 → 8,714；transport 读取中位数保持在约
1,567（两种路由配置均如此）。空闲路径未变，结合这些波动，不能据此声称空闲 CPU 或唤醒次数减少。
本次改动不包含空闲循环优化。

## 兼容性与限制

原有复制式 `snapshot()` 接口保留。Dispatcher 和 managed bus 使用不可变共享成员快照，
订阅变化后重新生成。Entry 状态继续使用自身互斥锁和取消屏障。回调顺序、自退订、
外部退订等待在途回调结束、并发 clear／unsubscribe，以及回调连续失败后的禁用语义保持不变。
性能数据不能成为放宽这些保证的理由。

Registry 增加了缓存状态，嵌入该对象的类布局随之变化。下游必须使用匹配的头文件和库重新编译，
不能将旧编译产物与新头文件／库混用。没有配置迁移或持久记录迁移。

这些数据来自直接调用 registry 的微基准，不是端到端 CAN 吞吐、设备延迟或硬件验收结果。
测试仅使用一个快照读取者，稳态测量期间没有并发订阅修改，并运行在虚拟化宿主环境。
五个样本不足以支持广泛的统计结论。分配插桩只统计成功的全局 C++ new/new[] 调用，
不包括直接 C 分配、内核内存、释放或峰值存活内存。完整测量边界见基准 README。

## 复现与核对

ARM64 Linux 本地验证通过了 Clang 18/libc++ 的 217 项离线测试、TSan 的
217 项测试，以及 ASan/UBSan 的 215 项测试（与 CI 一样，ASan 排除两个原生
动态库加载 fixture）。单线程分配失败 fixture 需要替换全局分配函数，与 TSan
的替换函数冲突，因此只对该 fixture 关闭 TSan；并发快照和生命周期测试仍启用
TSan。19 项 TSan 快照、取消和 Session 生命周期用例还分别连续通过了 20 次。
GCC 生产代码行覆盖率为 3,655/3,816（95.8%）。这些是本地结果，PR CI 另行检查。

分配失败回归在基线返回失败，修复后通过：退订准备阶段的分配失败必须保留
有效订阅，并允许重试。安装包接入、格式及本轮涉及的 C++ 编译单元的
clang-tidy 检查也已验证。

准备两份源码树，放入相同的基准源码及各自的基线／候选实现。耗时构建关闭分配统计，
分配构建单独开启：

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

保留每个进程的 CSV、工具版本、亲和性和运行顺序。汇总脚本校验回调／成员数量校验值，
只用 `allocation_tracking=0` 的样本比较耗时，只用 `allocation_tracking=1` 的样本统计分配。
