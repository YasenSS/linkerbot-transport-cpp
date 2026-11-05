# 参与贡献与问题反馈

[English](CONTRIBUTING.md) · 简体中文

仓库启用 Issues 时通过 Issues 反馈通信层问题；未启用时可提交带最小复现的 Pull Request。
手部协议、角度和 `HandDevice` 接口问题应提交到配套 hand SDK 仓库。开始前请阅读
[README](README.zh-CN.md) 的验证范围和对应后端文档。

## 问题报告

先在指定版本复现，再检查当前 `main` 是否已经包含修复。报告至少包含：

- transport 与 hand 的完整提交、是否有本地修改，以及比较过的 `main` 提交；
- 操作系统、架构、编译器、CMake、Debug/Release 和 Windows CRT；
- backend、适配器、固件、驱动或 DLL、通道、UART/CAN 速率和 FD 时序；
- 触发条件、前置条件、所需权限、实际影响和不受影响的边界；
- 最小复现代码、精确命令、重复次数、预期结果、实际结果和完整错误；
- 问题属于可靠性、配置、构建、ABI、性能还是其他类型。

日志上传前移除 token、口令、验证码、内部服务器地址和个人目录信息；不要提交厂商 DLL、
驱动、专有头文件或无法公开的抓包。

## 修改原则

一次修改集中解决一个问题。先保存修复前的失败证据，再修复，最后用回归证明修复后通过；
同时覆盖正常路径与兼容行为。不能通过删除记录、更换幂等键、放宽校验或禁用检测来隐藏问题。
通信和生命周期修改应优先检查并发、进程退出、超时、取消、确认丢失、重试和旧上层记录兼容性。

性能修改须在相同环境和输入下提供前后基准，记录耗时、分配、波动范围和兼容性。新增依赖须说明
许可证、固定版本、下载方式和发行包是否包含它。未连接真实适配器时不要求执行实机测试，也不能
把 fixture、PTY 或内存 transport 的结果写成硬件通过。

## 本地 ARM64 验证

完整构建、覆盖率、sanitizer 和 clang-tidy 命令见[构建指南](docs/zh/build.md)。维护验收优先使用
Linux ARM64；本地 x86 结果不能替代 ARM64 证据，amd64 由 GitHub CI 独立执行。至少运行与改动
相关的格式、严格编译和 CTest：

```bash
git ls-files -z -- '*.cpp' '*.hpp' '*.h' \
  | xargs -0 clang-format-18 --dry-run --Werror

cmake -S . -B build-libcxx -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  '-DCMAKE_CXX_FLAGS=-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror' \
  '-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++' \
  '-DCMAKE_SHARED_LINKER_FLAGS=-stdlib=libc++' \
  -DLINKERBOT_TRANSPORT_BUILD_TESTS=ON \
  -DLINKERBOT_TRANSPORT_BUILD_EXAMPLES=ON
cmake --build build-libcxx --parallel 4
ctest --test-dir build-libcxx --output-on-failure --no-tests=error --timeout 90
./build-libcxx/can-example
```

生产代码的 Linux 汇总行覆盖率要求按原始行数计算超过 99%（不使用舍入后的百分比），范围是 `src/` 与 `include/`，不是每个文件、
分支覆盖率或实机验收。并发改动使用 TSan 等适用工具；如跳过某项，须说明原因。Go 的
race/vet、Redis 和数据库测试只在涉及对应上层项目时适用，本通信库没有这些依赖。

安装、导出或 CMake 包变化还须验证 `find_package` 的独立 consumer。文档修改须检查接口、
默认值、命令、链接和安装后的相对路径；历史资料必须明确标为历史，并链接当前维护指南。

## 提交与 Pull Request

从最新 `main` 创建独立分支，使用签名提交，并在推送前验证签名：

```bash
git commit -S
git verify-commit HEAD
```

PR 使用克制、准确的英文，说明复现、根因、修复原理、兼容性、配置、旧记录、升级顺序、
回滚限制及通过、失败和跳过的验证。创建 PR 后检查**最新 head** 对应的 CI、review 和未解决线程；
“已创建 PR”不等于 CI 通过，“已推送标签”也不等于发布完成。

全部通过并获授权后才能合并。合并后同步 `main`，确认 main 的新 CI，再清理本次远端、本地分支
和临时工作树；不要删除或覆盖用户原有文件。项目采用 [MIT 许可证](LICENSE)，提交前请确认
代码、文档和测试资料有权公开。
