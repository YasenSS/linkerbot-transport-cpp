# CI 产物与兼容性验证

[English](../en/ci.md) · [文档索引](../README.md)

CI 分别执行真实 SDK 构建、质量检查和跨发行版消费验证。当前发行目标为
`ci/package-targets.json` 中定义的 **12 份独立构建 SDK**，用于 0.3.2 及后续构建，不替换已经发布的
0.3.1 四份 SDK。工作流目标本身不等于硬件验收证据。

## 实际产物矩阵

- Ubuntu 20.04、22.04、24.04、26.04 各构建 x86_64、aarch64 Release TGZ，共八份。
  每个 job 使用对应发行版的默认 GCC、CMake、头文件和库，运行示例，并迁移消费自己的包；
  其他发行版包不是 Focal 包改名。Linux Debug 仍用于质量检查。
- Windows x64、ARM64 各构建 Debug、Release ZIP，共四份。x64 使用 windows-2022/VS2022，
  ARM64 使用原生 windows-11-vs2026-arm/VS2026，两者均选择 v143 14.44。原生 CMake
  3.31.8/x64 和 4.2.3/ARM64 下载均校验 SHA256。检查原生系统架构、全部 EXE/DLL 的 PE
  machine 和 SDK 全部 COFF 对象，执行回归、loader 测试及匹配架构的迁移消费者。

每份归档附单独 SHA256 文件，加上 `release-manifest.json` 和 `SHA256SUMS.txt`，完整发行
应有 **26 个上传附件**，不计 GitHub 自动生成的源码归档。仅包含明确列出的 64 位架构，
x86_64 不能代替 32 位 x86，ARM64 不能代替 ARM32。

Linux artifact 名为 `sdk-linux-ubuntu<版本>-<架构>`，Windows 为
`sdk-windows-<架构>-<配置>`，避免目标间覆盖。Linux 在相同架构 runner 上的对应 Ubuntu
容器中构建，容器共享宿主内核，结果验证用户态，不证明各 Ubuntu 原生内核兼容性。

## 构建之外保留的兼容性检查

原生产物流程默认构建八份 Linux 包，PR 也执行全部八个 producer。显式
`baseline_only=true` 仅供 Focal 定向调试；完整发行清单会拒绝这类缺项集合。

独立兼容矩阵有意下载同架构的 **Focal 包**。PR 在 Ubuntu 20.04、26.04 消费该包，定时及
完整手动运行加入 22.04、24.04，保留旧基线的跨发行版证据。这与各发行版自身产物验证并行，
不能替代其他发行版的真实构建。Noetic 节点/nodelet 同样消费 Focal 包。

Ubuntu 26.04 检查默认 CMake 4。CMake 3.16 从构建目录运行 CTest，完整 Catch2 回归仍要求
3.19+。Noetic 使用真实 catkin、节点/nodelet 加载和卸载及 FakeTransport；它已结束上游
支持，仅在 Focal 检查。核心库仍要求 C++17，不新增 ROS 依赖。

这些检查不宣称原生内核、SocketCAN 接口、厂商驱动或实物验收。Windows ARM64 SDK
不证明对应厂商 ARM64 驱动/DLL 可用。Ubuntu 18.04/Melodic、Debian、musl 和 32 位目标
仍不在矩阵中。

## 质量与效率

保留既有 Linux Debug/Release、coverage、libc++、sanitizer、style 和 report 检查。
Linux 构建/report 在 runner 临时目录使用按架构、编译器和模式隔离的 ccache，发行
producer 不恢复该缓存。最低 CMake 3.16.9 缓存已校验源码构建的工具，并每次核对版本。
Windows 取消同工作流/ref 的过期运行，不预先宣称受控实测提速。

原生 producer 只构建一次 SDK/示例，再以 `ci/ubuntu-smoke.sh --consume-only` 消费归档，
不重复编译源码；跨发行版 job 独立消费 Focal 包，不重复原生源码构建。容器记录并使用 digest，但 apt 输入
可变，因此不承诺字节级可复现。预制工具链镜像及进一步减少矩阵需另行测量。

## 包身份与校验

包名及 `share/linkerbot-transport/build-info.json` 区分源码 SHA、发行版、架构、编译器、实际
标准库/dual ABI、配置和 Windows CRT，并记录 PIC、插桩状态。探测缓存包含配置参数，
多配置 CPack 依据 `-C` 命名；输入变化后重新配置。

`LINKERBOT_TRANSPORT_RELEASE_BUILD=ON` 要求干净源码和完整 SHA，拒绝已识别的 coverage/
sanitizer 参数；`LINKERBOT_TRANSPORT_BUILD_IMAGE` 可记录镜像身份。开发构建仍可使用脏树或
未知来源。来源说明不是完整 ABI 证明，无 Git 导出源码的调用者 SHA 也不是内容签名。

通用校验器检查预期发行版/系统/配置、metadata、全部 ELF 或 COFF archive 对象及泄漏的
运行库/测试组件。回归覆盖错误或混合 machine、缺失/重复发行身份。最终 ELF 消费者提供
符号版本证据，不能对静态 archive 执行 `ldd` 来证明运行时兼容。

## 发行工作流

手动运行 **Native Ubuntu SDK packages** 可构建而不发布；**Ubuntu and ROS1
compatibility** 的 `full_matrix=true` 执行完整基线消费矩阵。普通 job 只有仓库只读权限。

标签须匹配 CMake 版本且属于 main 历史。标签 push 仅验证。发行流程要求相同源码的可信
Linux/Windows/style/report 成功，在同次运行构建全部 12 份包并等待完整兼容检查。
Windows 质量门禁要求四个成功 job；Focal Session 要求相同源码提交的两个原生 GCC 9 job
均成功。未完成、缺失或失败的检查阻止发行准备。

标签 push 的最后一个 job 将原始标签对象、源码 SHA、运行 attempt、成功 job 身份、
十二个 artifact 的 ID/摘要和完整包清单保存为只读验证记录；记录不是二进制签名。
在相同版本标签上手动设置 `create_draft=true` 时，还须显式提供该成功标签运行的
`validated_run_id`。草稿步骤重新核对质量门禁，复用原始产物，不重复构建。拒绝 fork/PR、
不同标签或源码、不完整矩阵、过期或变化的产物、标签对象变化、并发重跑及没有验证记录的
旧运行；不隐式选择最新运行，仍重新生成完整清单核对。SDK 与兼容性证据的附件名包含
attempt，完整重跑可保留旧附件但不会复用它们。须重跑全部 job；仅重跑失败 job 产生的
混合 attempt 结果会被拒绝。过期后应重新完成验证，不能放宽检查。

对包含新工作流的未来版本标签，检查标签 push 成功后，显式运行（替换两个占位值）：

```bash
gh workflow run release.yml --repo linker-bot/linkerbot-transport-cpp \
  --ref VERSION_TAG -f create_draft=true -f validated_run_id=SUCCESSFUL_TAG_PUSH_RUN_ID
```

命令仅创建新草稿。已有 Release 或没有验证记录的旧标签不适用；不能为采用新流程而移动
或重建旧标签。清单必须恰好
包含八个 Linux 发行版/架构身份和四个 Windows 架构/配置身份；生成清单和总校验值前
检查摘要、大小、metadata 及所有库对象 machine，复用 Focal 包不能满足另一个发行版身份。

草稿 job 重新检查远端标签，拒绝已有 Release，上传已验证的相同字节并下载复核；仅该
job 有仓库写权限，不自动正式发布或移动标签。不完整草稿须先检查再重试，正式产物修订
需新版本。修改工作流本身不会创建标签或 Release。

## Python 接入前验证

独立的 `python-preintegration.yml` 使用已安装原生库构建 CPython 验证模块，覆盖 Linux
x86_64/ARM64 的常规 GIL CPython 3.10–3.13、Windows x64 的 3.12 及 Windows ARM64 的 3.13。
源码入口为 `validation/python-preintegration`；它不是生产绑定或 wheel，拒绝子解释器和
free-threaded 构建。分配失败、回执、回调、生命周期和部署证据校验不证明实际 hand/arm 已共用
Runtime，也不代表实机兼容，后端能力与实机边界见 [CAN 元数据说明](can-metadata.md)。

## CI 耗时对照

[实测结果与限制（英文）](../en/ci-performance.md)记录全部基线/优化运行链接、三轮中位数、
Windows 缓存开销和仍待执行的新标签端到端验证。

Style 和完整报告的手动运行支持 `analysis_jobs=2`（基线）或 `4`（优化），实际并发不超过
runner 可用 CPU affinity。规则、翻译单元和完整报告门禁保持不变；证据记录实际并发、
源码 SHA 和配置摘要。比较相同源码、工具版本下分析步骤的中位耗时，不把调度与准备耗时
全部算作分析时间。Style 的 GNU time 最大 RSS 不等于所有并发进程的总峰值。

Windows 手动运行支持 `use_tool_cache=false` 强制下载，或 `true` 复用固定 CMake。
缓存键包含版本、下载架构和新获取的官方 SHA256；命中仍校验 ZIP 与全部安装文件，再
确认实际执行路径及精确版本。PR 不写入该缓存。现有 concurrency 会取消被替代的运行，
因此冷暖对照须串行，收益计算须包含校验值下载、缓存恢复/保存和完整性检查开销。
