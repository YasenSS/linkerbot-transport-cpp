# 构建、安装与 CMake 接入

[English](../en/build.md) · [文档索引](../README.md) · [Windows 专项说明](windows-build.md)

规范包名、链接目标及保留的兼容入口见[命名迁移](naming.md)。已发布的 v0.3.2 包保留旧名称。

## 前置条件与目录约定

核心库需要 CMake 3.16+、C++17 编译器和线程支持。本文常规测试命令使用
`ctest --test-dir`，执行这些命令请使用 CMake/CTest 3.20+；3.16 的接入命令见下节。
Catch2 3.8.1 本体声明最低 CMake 3.16，但其测试发现脚本需要 JSON 支持，因此本项目
启用测试时要求 CMake 3.19+；其他已安装 Catch2 版本还须核对其自身要求。Linux CI 使用 Ubuntu 24.04，主矩阵以
GCC 13 覆盖原生 amd64、ARM64 的 Debug/Release 与 99% 行覆盖率；Clang 18 质量矩阵
在两个架构上覆盖 libc++、ASan+UBSan 和 TSan。本地维护验证优先使用 Linux ARM64。
Windows 使用 MSVC x64/ARM64，见专项说明。macOS 不在当前实机支持和 CI 验证矩阵内。

以下 Bash 命令都从源码根目录执行。示例使用 Ninja，需预先安装；也可去掉 `-G Ninja`
使用平台默认生成器。Ubuntu 构建工具可这样安装：

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends build-essential cmake ninja-build git ca-certificates
```

首次启用测试时可能从 GitHub 下载 Catch2，需网络和 Git；核心库不需要厂商 SDK、
厂商动态库、真实 CAN 设备或 Catch2。不要把厂商 DLL 复制到源码目录当作构建依赖。

## CMake 3.16 最小接入

以下路径不下载测试依赖，也不需要真实设备。从源码根目录执行：

```bash
cmake -S . -B build-minimum -DCMAKE_BUILD_TYPE=Release \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=OFF \
  -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=OFF \
  -DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF
cmake --build build-minimum --parallel 4
cmake --install build-minimum --prefix "$PWD/install/minimum"
cmake -S examples/quickstart -B build-quickstart-minimum \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install/minimum"
cmake --build build-quickstart-minimum --parallel 4
(cd build-quickstart-minimum && ctest --output-on-failure)
```

在旧版 CTest 中进入构建目录再执行测试，避免使用不受支持的 `--test-dir` 参数。
该示例测试只验证内存 transport 的收发和安装包接入，不是完整回归或实机验收。

## 完整离线回归

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON
cmake --build build-release --parallel 4
ctest --test-dir build-release --output-on-failure --no-tests=error --timeout 60
./build-release/can-example
```

Debug 使用另一个目录 `build-debug` 和 `-DCMAKE_BUILD_TYPE=Debug`。单配置生成器需要在
配置时指定构建类型；Visual Studio 等多配置生成器在构建、CTest、安装时使用 `--config` / `-C`。
不要复用其他架构、编译器或生成器产生的构建目录。

优先使用 CTest。`pcan-native-loader` 与 `vendor-native-loader` 由 CMake 注入测试动态库
路径，直接运行对应测试程序而没有这些环境变量会失败；这不代表缺少真实驱动。
只运行 Session 恢复用例并重复检查：

```bash
ctest --test-dir build-release -R '^Session recover' --repeat until-fail:20 \
  --output-on-failure --no-tests=error --timeout 60
```

GCC 的 amd64/ARM64 Debug/Release CI 还会分别重复三个 registry 生命周期交错回归：

```bash
ctest --test-dir build-release \
  -R '^(Session registry waiters recheck a replacement active generation|Session registry ignores waits for a published initialization attempt|Session registry rebinds a waiter to a replacement initialization attempt)$' \
  --repeat until-fail:20 --output-on-failure --no-tests=error --timeout 60 \
  --output-log registry-handoff-results.log
```

第一个用例检查清理结束后已发布的新 active generation 不会被旧等待条件遗漏；第二个用例
检查已经发布的初始化尝试不会以陈旧等待边误报循环；第三个用例检查初始化失败并更换
initializer 后，等待边会绑定到新的初始化尝试。同步门固定进入清理、发布和失败路径，但
公共 API 不暴露内部等待边的安装和清除时刻；重复运行只用于扩大 handoff 调度覆盖，硬超时
用于防止回归挂死，不能把重复次数当作确定性证明。

## 构建选项

| 配置项 | 默认值 | 作用 |
| --- | --- | --- |
| `LINKERBOT_TRANSPORT_BUILD_TESTS` | 顶层 ON，子目录 OFF | 构建 Catch2 测试及原生 ABI fixture |
| `LINKERBOT_TRANSPORT_BUILD_EXAMPLES` | 顶层 ON，子目录 OFF | 构建 `can-example` |
| `LINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS` | ON | 缺少 Catch2 3 时允许下载 v3.8.1 |
| `LINKERBOT_TRANSPORT_ENABLE_COVERAGE` | OFF | 仅 Linux/GCC；要求测试 ON；加入 `--coverage -fprofile-update=atomic -O0 -g` |
| `LINKERBOT_TRANSPORT_ENABLE_SERIAL` | ON | 构建和导出 `linkerbot::serial` |
| `LINKERBOT_TRANSPORT_ENABLE_MODBUS` | ON | 构建和导出 `linkerbot::modbus`；依赖 serial |
| `LINKERBOT_TRANSPORT_BUILD_FUZZERS` | OFF | 构建 Clang RTU 编解码模糊测试；依赖 Modbus |
| `LINKERBOT_TRANSPORT_BUILD_BENCHMARKS` | OFF | 在根项目中构建仅支持 Linux 的性能测量程序 |
| `LINKERBOT_PERF_COUNT_ALLOCATIONS` | OFF | 为基准程序启用 C++ 分配计数；启用基准子目录后可用 |
| `Catch2_DIR` | 由 CMake 查找 | 已安装的 Catch2 3 CMake 配置目录 |
| `CMAKE_INSTALL_PREFIX` | CMake 平台默认 | 安装前缀；也可在安装时用 `--prefix` |
| `LINKERBOT_TRANSPORT_SOURCE_REVISION` | 自动读 Git；不可用时 unknown | 导出源码构建时人工提供完整来源 SHA |
| `CMAKE_MSVC_RUNTIME_LIBRARY` | MSVC 默认为动态 CRT | 默认 Release `/MD`、Debug `/MDd`；须和消费者一致 |

这些是 CMake 缓存变量，不是运行时环境变量。修改下载开关不会删除已经存在的 Catch2
缓存，也不能替代预先提供依赖。`BUILD_SHARED_LIBS` 不会把本项目变为共享库，核心目标显式为 STATIC。

完全禁止依赖下载且只构建核心库：

```bash
cmake -S . -B build-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=OFF -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=OFF \
  -DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF \
  -DLINKERBOT_TRANSPORT_ENABLE_SERIAL=OFF -DLINKERBOT_TRANSPORT_ENABLE_MODBUS=OFF
cmake --build build-core --parallel 4
```

需要离线运行测试时，先安装 Catch2 3，将 `-DCatch2_DIR=/absolute/path/to/lib/cmake/Catch2`
加到完整回归的配置命令，并关闭下载。Catch2 2 不满足要求。

## 性能基准

在 Linux 上独立构建测量程序，无须 Catch2 或真实硬件：

```bash
cmake -S benchmarks -B build/perf -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/perf --parallel 4
./build/perf/linkerbot-perf --iterations 200000 --idle-ms 500 --repetitions 5
```

也可在配置根项目时指定 `-DLINKERBOT_TRANSPORT_BUILD_BENCHMARKS=ON`，此时可执行文件位于
`build/benchmarks/`。计时保持分配计数 OFF；分配次数使用独立构建，并指定
`-DLINKERBOT_PERF_COUNT_ALLOCATIONS=ON`。测量边界、复现命令和已记录的 ARM64 结果见
[性能指南](performance.md)。

Linux CI 的 `benchmark-smoke` 任务在 ARM64 上使用 Clang 18/libc++ 构建两种分配计数模式，
运行缩短的场景并保留 CSV 附件。它验证测量程序能够构建和运行（包括内部校验和检查），
不设置性能阈值，也不能替代受控条件下的优化前后对比。

## 行覆盖率与可核对结果

先安装 `gcovr`，使用新的构建目录，避免源码变化前的 `.gcda` 计数混入报告：

```bash
sudo apt-get install -y --no-install-recommends gcovr
cmake -S . -B build-coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON -DLINKERBOT_TRANSPORT_ENABLE_COVERAGE=ON
cmake --build build-coverage --parallel 4
ctest --test-dir build-coverage --output-on-failure --no-tests=error --timeout 60
gcovr --root . --filter '(src|include)/' --gcov-exclude-directories 'build-coverage/_deps' \
  --fail-under-line 99 --txt --json coverage.json --json-summary coverage-summary.json build-coverage
```

99% 是整个 Linux 构建中生产代码的汇总**行覆盖率**，不要求每个文件或分支均为 99%。
统计包含 `src/` 与 `include/`（包括对外提供的 FakeTransport），排除测试目录和 Catch2，
不排除某些生产行来提高结果。平台条件编译未进入 Linux 构建的 Windows 代码不在分母中。
固定提交的已执行结果见 [验证记录](validation.md)，CI 文件存在本身不算通过。

Linux Debug 任务同时保留 `coverage.json` 和汇总报告。完整报告包含各被测文件的可执行行、
执行次数和分支记录，可结合该次运行的源码版本逐项检查缺口。比较结果时使用全新构建和相同的
测试顺序，不只比较舍入后的百分比。

生产代码行覆盖率达到 100%，仅表示本次统计中的每一条可执行行均被执行。分支覆盖率、所有
支持平台的代码覆盖和实机验证分别需要独立证据。Linux 预处理会省略 Windows 原生串口和
DLL 加载路径；行计数达标也不能证明已经验证所有错误结果或并发交错。

将门槛提高到 100% 前，应逐行确认剩余缺口：可通过补充测试覆盖的行为、需要受控故障注入的
失败路径，或尚需分析可达性的路径。现有 Linux 系统调用包装、原生 ABI 替身库和分配失败
可执行程序提供了故障注入手段，但不能据此认定所有剩余路径都能稳定触发。保留生产代码统计
范围和防御行为。100% 门槛需要两个 Linux CI 架构上的可重复证据；当前仍要求舍入前严格
超过 99%。

部分现有路径无法通过普通公开 API 调用在现实时间内覆盖：`RtuSession::recover` 检查 64 位
代次计数器耗尽，Session 注册表处理 64 位代次及初始化计数器回绕。注册表还会拒绝已经损坏的
依赖环；Pool 清理在内部移动不变量被破坏时终止进程。这些路径需要有依据的内部测试接缝，
或单独论证其可达性，不能靠重复公开操作来实际触发。厂商发送编码器在长度转换为 DLC 前已
校验消息，因此无效的公开载荷会先被拒绝，无法进入转换辅助函数的防御性抛错。基于这些约束，
不能承诺仅补充公开 API 测试即可达到 100%。

覆盖率静态库的消费者也须在链接时加 `--coverage`，例如配置消费者时传
`-DCMAKE_CXX_FLAGS=--coverage`。普通 Release 构建保持 coverage OFF。

## Clang、第二标准库与 sanitizer

Clang 质量矩阵在 Linux amd64 与 ARM64 上执行以下三种独立 Debug 构建：

- `libcxx`：使用 Clang 18 与 libc++ 运行全部 CTest，包括两个原生 ABI fixture；该构建以
  `-Wall -Wextra -Wpedantic -Werror` 编译生产库、测试、fixture 和示例，运行示例，并以
  相同告警门槛验证安装后的 libc++ 消费者。
- `asan-ubsan`：使用 libstdc++ 和 `-fsanitize=address,undefined`，启用泄漏检测并在首个
  ASan/UBSan 错误处失败。普通 CTest 逐例运行；仅排除下述两个原生 ABI fixture。
- `tsan`：使用 libstdc++ 和 `-fsanitize=thread`，不关闭死锁检查，逐 CTest 进程运行全部测试。

ASan+UBSan 不与 Ubuntu 的系统 libc++ 组合：ARM64 实测会在 libc++/libc++abi 的异常对象
分配路径报告 `operator new`/`free` 不匹配；该次观测未将报告归因于项目代码。两个 native-loader fixture
也不进入 ASan+UBSan 运行：它们为模拟无头文件的第三方 C++ 动态库，刻意各自声明布局相同、
类型身份不同的 ABI 结构；两侧都带 UBSan 元数据时会报告函数类型不匹配。CI 没有关闭相应
sanitizer 检查来隐藏报告；这些 fixture 仍由 GCC 主矩阵、Clang+libc++ 和 TSan 全量覆盖。

线程检查应保留编译器版本、完整告警和退出码。把所有 Catch2 用例塞入同一进程会因不同测试
复用栈地址而产生跨用例锁图告警，因此 CI 使用 `catch_discover_tests` 生成的逐例 CTest 进程，
不能用全局关闭死锁检查或压掉告警替代诊断。

在 Ubuntu 24.04 ARM64 复现 sanitizer 任务时，先安装 Clang 18、LLVM 符号化工具和
sanitizer 运行库，再使用独立构建目录。以下命令与 CI 一样使用 libstdc++，逐个运行发现的 CTest：

```bash
sudo apt-get install -y --no-install-recommends \
  clang-18 llvm-18 libclang-rt-18-dev
cmake -S . -B build-asan-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined' \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON
cmake --build build-asan-ubsan --parallel 4
env ASAN_SYMBOLIZER_PATH="$(command -v llvm-symbolizer-18)" \
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan-ubsan \
    -E '^(pcan-native-loader|vendor-native-loader)$' \
    --output-on-failure --no-tests=error --timeout 90

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -fsanitize=thread -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread' \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON
cmake --build build-tsan --parallel 4
env TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:external_symbolizer_path=$(command -v llvm-symbolizer-18)" \
  ctest --test-dir build-tsan --output-on-failure --no-tests=error --timeout 120
```

## Clang 格式与静态检查

### C++ Report Card

独立的 `C++ Report Card` 工作流在 Linux ARM64 上运行固定完整提交 SHA 的
[cppreportcard-action](https://github.com/soulteary/cppreportcard-action) v1.0.2。
构建项目、quickstart、安装后消费者、共享库消费者和使用安装库的 Python 验证模块后，
合并五份编译数据库，以 `full` 模式运行 clang-format 18、clang-tidy 18 和 Cppcheck，
覆盖主工程源码、测试、独立消费者和验证模块。

配置位于 `.cppreportcard.json`。平台依赖排除项包括 Windows 专用文件
`tests/vendor_fixture_dependency.cpp`、需要厂商头文件的 `validation/pcan_header_abi.cpp`，
以及需要本报告环境未安装的外部 Noetic SDK 的 `validation/ros1-consumer`。ROS fixture
在 Noetic 工作流以 warnings-as-errors 编译并运行节点/nodelet；独立全局 style job 仍对
全部已跟踪 C++ 源码检查格式。构建产物目录与第三方依赖也不参与分析。
报告启用 `clang-analyzer-*` 和 `bugprone-*`，不替代现有 style 工作流中不同的规则集
或平台验证。

`validation/prepare-report-db.py` 合并编译数据库，并将项目的 `-isystem` 目录显式表示为
`-I`，供 Cppcheck 使用。工作流同时记录编译器的实际目标宏，并让 Cppcheck 预包含已安装的
多架构 Python 配置头，以解析 CPython 同名转发头；不排除验证模块源码或预处理错误。
`validation/cppcheck-report.sh` 让 Cppcheck 使用 `std` 标准库模型，
并通过 `validation/catch2-cppcheck.cfg` 为项目使用的 Catch2 宏建模。
包装脚本仅抑制 `missingIncludeSystem`：编译器标准库头文件不是 Cppcheck 的分析输入。
完整模式中的 clang-tidy 仍检查每个编译单元的缺失头文件，包括尖括号形式的项目头文件；
其他 Cppcheck 预处理和语法错误不会被抑制。

Catch2 模型处理的是 Cppcheck 2.13 的解析限制，不会从分析中移除测试体或断言操作数；
真实构建继续使用 Catch2。工作流在生成报告前运行 29 个静态哨兵用例和两个运行探针，
对比真实 Catch2 与模型的 fatal/nonfatal 断言行为。哨兵要求检出断言、section 和后续
测试用例、section 名称及其代码中的空指针和越界错误。注册参数检查要求测试名称和标签
使用字符串字面量，拒绝模型无法分析的注册表达式。按工作流构建三组分析输入并生成合并数据库后，可在
源码根目录执行相同的模型检查：

```bash
python3 validation/test-cppcheck-model.py --compile-commands build/report-compile-commands.json
```

CI 报告对应其实际分析的提交。

从工作流下载 `cppreportcard-ubuntu-24.04-arm` 附件，可查看 `report.json`、`report.md`
和 `badge.svg`，也可阅读 job summary。CI 将它们写入 `.github/cppreportcard/`。
中英文 README 展示仓库中的 badge 快照，来源提交和运行记录见
[快照说明](../../.github/cppreportcard/README.md)；更新时须在签名维护提交中同步修改 SVG 和说明。
生成的 JSON、Markdown 报告保留为附件。工作流只读仓库，采用 `fail_on: findings`。
`validation/check-quality-report.py` 还要求 A+、三个工具完整完成 full 模式分析且没有诊断。
对应的反例验证会拒绝分析不完整、缺少工具或仅声称高分的报告。
这是收紧 CI 门槛，没有改变分析规则或评分方式。分数不代表测试覆盖率或实机验收。
Cppcheck 来自 Ubuntu 包仓库，runner 更新可能改变其版本；比较报告时应核对记录的工具版本。

### 现有 Clang 检查

独立的 Linux ARM64 工作流使用 clang-format 18 检查所有已跟踪的 C++ 源文件和头文件；
clang-tidy 18 检查 compile database 中的项目、示例、测试翻译单元及下述两个单独文件，头文件
通过这些翻译单元间接检查。主 compile database 包含 `examples/shared_bus.cpp`，不包含独立
配置的 `examples/quickstart/main.cpp`，当前 style job 未对该 quickstart 执行 clang-tidy；
格式检查仍覆盖这两个已跟踪的示例源文件。维护者本地验收同样以 ARM64 为准；amd64 结果由 GitHub CI 提供，
不能用本地 x86 运行替代 ARM64 证据。Ubuntu 24.04 可安装以下工具：

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  cmake ninja-build git ca-certificates \
  clang-18 clang-format-18 clang-tidy-18 clang-tools-18 \
  libc++-18-dev libc++abi-18-dev
```

从源码根目录执行格式检查：

```bash
git ls-files -z -- '*.cpp' '*.hpp' '*.h' \
  | xargs -0 clang-format-18 --dry-run --Werror
```

生成包含测试和示例的 compile database，再按 CI 的范围执行 clang-tidy：

```bash
cmake -S . -B build-tidy -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-stdlib=libc++' \
  '-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-stdlib=libc++' \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON \
  -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON

run-clang-tidy-18 -p build-tidy -j 2 -quiet \
  -warnings-as-errors='*' \
  "^${PWD}/(src|examples)/.*[.]cpp$|^${PWD}/tests/(?!test_).*[.]cpp$"

run-clang-tidy-18 -p build-tidy -j 2 -quiet \
  -checks=-cert-err58-cpp -warnings-as-errors='*' \
  "^${PWD}/tests/test_.*[.]cpp$"

clang-tidy-18 tests/vendor_fixture_dependency.cpp \
  -warnings-as-errors='*' -- -std=c++17

clang-tidy-18 validation/installed-consumer/main.cpp \
  -warnings-as-errors='*' \
  -- -std=c++17 -stdlib=libc++ -Iinclude -Icompat
```

`.clang-tidy` 只对宏展开忽略 `cppcoreguidelines-avoid-do-while`。Catch2 的 `TEST_CASE`
会生成静态注册对象，而 `cert-err58-cpp` 没有宏忽略选项，因此仅 `tests/test_*.cpp` 在第二轮
排除该检查；项目源码、测试辅助翻译单元和其余已启用规则仍以告警即错误执行。
`tests/vendor_fixture_dependency.cpp` 仅由 Windows CMake 构建，不进入 Linux compile database，
所以在 Linux 中单独检查其可移植分支。`validation/pcan_header_abi.cpp` 需要未随仓库分发的
官方 PCAN 头，不属于通用 style job；执行或跳过该检查都应在验证记录中明确说明。

格式或静态检查通过不代表编译、CTest、sanitizer、实机或发布已经通过，必须分别保留结果。

## 源码接入和安装接入

源码接入时，调用者的 CMakeLists 应先定义自己的 `your-sdk` 目标：

```cmake
add_subdirectory(../linkerbot-transport-cpp linkerbot-transport-cpp)
target_link_libraries(your-sdk PRIVATE linkerbot::can)
```

本项目作为子目录时测试和示例默认关闭；若显式开启测试，顶层工程也需开启 CTest 发现。
按下面方式安装并验证不依赖源码树的消费者：

```bash
cmake --install build-release --prefix "$PWD/install/transport"
cmake -S install/transport/share/linkerbot-transport/validation/installed-consumer \
  -B build-consumer -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/install/transport"
cmake --build build-consumer --parallel 4
ctest --test-dir build-consumer --output-on-failure --no-tests=error --timeout 60
```

应用工程使用 `find_package(linkerbot-transport 0.3 CONFIG REQUIRED)`：

- `linkerbot::can`：实现、头文件、C++17 和线程链接依赖。
- `linkerbot::can-compat`：同时提供旧的扁平头文件路径，并链接实现。
- `linkerbot::headers`：头文件路径和 C++17，不链接实现；须链接所调用 API 对应的模块。
- `linkerbot::serial`：串口字节流（启用时）。
- `linkerbot::modbus`：RTU 事务及串口依赖（启用时）。

`SameMinorVersion` 的包版本规则要求相同 major/minor，且安装版本不得低于请求版本。
配套 hand SDK 的补丁版本要求仍须满足，不能只根据 `find_package(... 0.3 ...)` 成功就判断兼容。
跨架构、CRT、编译器或库版本混用时，先按 [升级顺序](operations.md) 配套重编译。

## 打包与来源

顶层 Linux 构建默认生成 TGZ：

```bash
cpack --config build-release/CPackConfig.cmake -B "$PWD/dist"
```

CPack 生成相邻 SHA256 文件；新 Linux 包名包含版本、系统、架构、构建发行版、编译器、
标准库和配置。不同配置仍使用独立构建目录，已发布资产保留原名。Windows ZIP 命名及
运行库另见专项说明。

`share/linkerbot-transport/build-info.json` 包含版本、`transport_sha`、`source_dirty`、系统、架构、
编译器、工具集、配置和运行库。来源信息在 CMake 配置时生成：更换源码提交后应重新配置，
不能用旧构建目录的 metadata 声称新提交已验证。无 Git 的导出源码可显式传入完整 SHA，
但它是调用者提供的来源说明，不是内容校验或签名。
`source_dirty` 包含 Git status 中的已跟踪变更和未忽略的未跟踪文件，不检查被忽略文件
或另行安装的厂商二进制。严格发行输入、扩展 metadata 与验证目标见 [CI 产物与兼容性](ci.md)。

发行前核对干净源码、版本、配置、架构和 SHA256，并重新执行安装后消费验证。
本库构建不自动创建 GitHub Release、签名标签或镜像。独立发行工作流仅在显式手动请求
且门禁通过后创建草稿；CI 成功和打包成功都不等于正式发布。

串口与 RTU 分别导出 `linkerbot::serial` 和 `linkerbot::modbus`，须显式链接；
`linkerbot::can` 不传递链接这两项。这些模块需要 0.3.3 或更新版本，已发布的 v0.3.2 包不包含它们。
参见[串口与 RTU](modbus-rtu.md)。

生命周期分配失败夹具替换全局分配函数，因此不在同样替换这些符号的 TSan 构建中运行。
该夹具仍参与 GCC 覆盖率、严格 libc++ 和 ASan/UBSan；常规并发测试继续由 TSan 验证。
Linux 系统调用包装夹具与常规 PTY 测试分别运行。

CI 使用原始 `line_covered`、`line_total` 行数，要求
`line_covered * 100 > line_total * 99`。gcovr 舍入后显示 99.0，不能替代实际比例超过门槛。

MSVC Debug 下，分配夹具不对迭代器代理大小的请求注入故障：STL 会在 `noexcept` 的空容器
构造函数中分配代理。按大小区分也会使夹具中的同大小有效载荷分配不被注入。Linux 覆盖率注入不受影响。
