# 本地通信库 Ubuntu 压测

从通信库根目录执行：

```bash
python3 benchmarks/stress/run.py --quick
python3 benchmarks/stress/run.py
python3 benchmarks/stress/run.py --source /path/to/another/local/transport
python3 benchmarks/stress/run.py --output /path/to/results
python3 benchmarks/stress/run.py --verify /path/to/generated/archive.tar.gz
```

已保存的 2026-11-10 正式结果已翻译为英文，使用归档内的英文校验器；具体步骤见[英文结果说明](../results/stress-20261110-152431/README.md)。上述 `--verify` 命令用于本工具生成的原始格式归档。

默认测试本工具所在通信库的实际源码，不联网同步、不使用发布包。即使从其他工作目录启动也能定位源码；`--source` 可选择另一个本地 Git 工作树。需要 Ubuntu、Python 3.10+、CMake、GCC/G++、Git、iproute2、kmod 和 vcan 内核支持，以及可免交互执行 `ip netns`、`modprobe vcan`、`kill` 的 `sudo -n`。无需物理 CAN 设备。

源码按职责存放：

- `run.py`：参数扫描、数据校验、环境记录、报告、归档及资源清理。
- `src/throughput.cpp`：内存/vcan 吞吐、逐帧校验及队列边界/恢复契约。
- `src/profile.cpp`：线程/窗口对照、队列延迟、CPU 和模拟端逐帧/批量对照。
- `src/socket_probe.c`：在 CAN socket 打开/关闭时读取内核丢帧及实际接收缓冲，仅调整本次 socket，不拦截逐帧读写。
- `CMakeLists.txt`：独立构建；已有 `linkerbot::can` 目标时可复用，独立运行时构建所选本地源码。
- `tests/test_run.py`：失败判定、超时清理、归档完整性及输出目录自检。

每次在新的临时目录构建 Release，链接 `linkerbot::can`，关闭库自身测试、示例、基准、串口/Modbus及依赖下载。记录版本、HEAD、工作树、源码 SHA256 清单、编译命令、链接映射及实际库摘要。构建后和测试后复核源码，并在运行中轮询；发现变化即标记无效。允许带本地变更，报告会明确记录。

正式扫描内存/vcan × CAN 8 字节/FD+BRS 64 字节：1/2/4/8/16/32 线程，每批 1/8 帧、窗口 8 批；补测 1/8 线程、每批 32/128 帧、窗口 1 批；调整最快候选窗口为 1/4/16/32 批。每档 15 秒，最快两个无损候选复测 30 秒，择优连续两次确认 60 秒。失败档单列，不计入无损成绩。正常 vcan 两端申请 4 MiB 接收缓冲，记录实际值与内核丢帧。另做默认缓冲过载、扩大缓冲/缩小窗口对照、相同在途量线程对照，以及 FD 模拟端逐帧/批量对照。

默认结果保存到 `benchmarks/results/stress-时间戳/`，仅有 `压测报告.md`、`压测原始数据.tar.gz`。`--quick` 每档 2 秒、缩减筛选，结果放 `benchmarks/results/quick-时间戳/`，仅有 `快速验证报告.md`、`快速验证原始数据.tar.gz`，明确标记非正式。同秒启动也分配不同目录；同一源码的压测不并发执行。`--output DIR` 指定确切目录，重复指定同一目录会替换其中的两个结果文件。

结果目录已加入 Git 忽略，不会干扰源码变化检测。逐档 JSON/CSV/日志、构建、静态库及链接映射全部位于临时目录，归档逐项 SHA256 和报告指标校验后清理。包内保留实际运行的工具源码（含 src/tests 子目录）。历史正式报告与压缩包直接迁移，不修改测量内容或原始源码路径；归档中的旧路径描述当时的运行环境。

只创建本次独立网络命名空间内的 vcan，不操作物理接口或全局 sysctl。正常、超时、异常及 SIGINT/SIGTERM 均停止本次进程组、清理命名空间并归档现有证据。失败返回非零，报告标记不完整；归档失败不会宣称成功。SIGKILL/断电不可捕获，可按终端打印的命名空间名称检查。CPU/RSS 包含模拟端，短时 RSS 不证明长期无泄漏，软件吞吐不代表物理总线带宽。

自检：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s benchmarks/stress/tests -p 'test_*.py'
```
