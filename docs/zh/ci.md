# CI 产物与兼容性验证

[English](../en/ci.md) · [文档索引](../README.md)

CI 将质量构建、基线 SDK 打包和用户态兼容性检查分开。工作流中的矩阵表示验证目标，
不是平台或真实后端已经通过的证据；实际结果须查看对应源码提交的运行记录。

## 构建与验证目标

基线工作流使用 Ubuntu 20.04 默认 GCC 9 和 CMake，原生构建 x86_64、aarch64 Release
静态 SDK，关闭测试依赖下载、coverage 和 sanitizer。CPack 使用绝对输出路径，避免
3.16 在相对 `-B` 路径下静默生成空归档；解包回归要求拒绝校验值正确的空 SDK。
生产静态库开启 PIC，以支持共享库
和 ROS nodelet 消费。项目仍要求 C++17，核心库不新增 ROS 依赖。

PR 检查 Ubuntu 20.04、26.04 双架构用户态，以及 Ubuntu 20.04 的 Noetic 节点和 nodelet。
定时及完整手动检查加入 Ubuntu 22.04、24.04。每个 Ubuntu 环境均以默认 GCC/CMake
构建源码，再消费相同架构的 Focal 包，运行包内 quickstart、独立消费者和共享库消费者。
CMake 3.16 从测试目录运行 CTest，不使用其尚不支持的 `--test-dir`；完整 Catch2 回归
仍要求 CMake 3.19+。

Ubuntu 26.04 是新增验证目标，包含系统默认 CMake 4，不表示 ROS1 支持该系统。
Noetic 使用真实 catkin、ROS 节点、动态 nodelet 及卸载服务，但收发仅使用 FakeTransport。
Noetic 已结束上游支持。本矩阵不新增 Ubuntu 18.04/Melodic、Debian、musl 或 Windows ARM64。

容器共享 runner 内核，结果证明用户态构建和集成，不证明原生内核、SocketCAN 接口、
厂商驱动或设备验收。fixture 与 FakeTransport 成功不能写成真实 CAN 设备测试通过。

## 质量门禁与效率

现有 Linux Debug/Release、coverage、libc++、sanitizer、style、report-card 和 Windows
检查继续保留，不削减覆盖，也不预先宣称实测提速比例。Linux 构建与 report job 在 runner
临时目录使用 ccache，恢复前缀区分架构、编译器和模式；ccache 同时校验编译器内容与编译
输入。缓存不替代回归执行，基线发行包不恢复这份编译缓存。

CMake 3.16.9 检查按源码 SHA256、架构和构建策略缓存完整安装目录。未命中时仍下载、
校验并构建，每次检查恢复后的版本。Windows 会取消同一工作流和 ref 的过期运行。

工具链镜像当前按明确版本标签拉取，记录解析后的 digest，并以该 digest 执行。
Apt 软件仓库仍可能变化，因此这里提供构建来源，不承诺字节级可复现或永久固定的软件包
集合。预制工具链镜像及进一步压缩 PR/main 矩阵属于后续工作，应先收集冷/热缓存耗时和
命中率，再决定是否减少现有检查。

## 包身份与来源

新 Linux 包名包含系统、架构、构建发行版、编译器版本、标准库及配置。已发布的 0.3.0
资产不变。Windows 保留工具集、编译器、配置和 CRT 信息，目前只允许 x64；64 位指针
不能独立证明目标是 x64。

`share/linkerbot-comm/build-info.json` 记录源码 SHA、脏树状态（含未忽略的未跟踪文件）、
发行版、编译器、标准库、C++ 标准、PIC、插桩状态、配置、CRT 和可选镜像身份。
同时记录探测得到的 libstdc++ dual ABI；标准库探测包含配置专用编译参数，多配置 CPack
按 `-C` 选择包名，避免 Debug/Release 重名。信息在配置时生成，输入变化后须重新配置。自定义编译参数和工具链仍需消费验证；
metadata 不是完整二进制证明或任意 ABI 兼容保证。

`LINKERBOT_COMM_RELEASE_BUILD=ON` 要求干净源码和完整 SHA，拒绝已识别的 coverage/
sanitizer 参数；`LINKERBOT_COMM_BUILD_IMAGE` 可提供镜像身份。普通开发构建仍允许脏树或
未知来源。无 Git 的导出源码须显式提供 SHA，该值属于调用者声明。

`validation/package/verify-package.py` 检查 metadata、全部 ELF archive 对象及不应包含的
运行库和测试组件，`test-verify-package.py` 检查拒绝路径。消费测试记录最终 ELF 消费者的
符号版本需求；不能通过对静态 archive 执行 `ldd` 证明运行时兼容。符号列表是证据，
不是任意编译器和标准库都可混用的承诺。

## 运行与发行

手动运行 **Focal baseline SDK packages** 可构建产物，不发布。运行 **Ubuntu and ROS1
compatibility** 并设置 `full_matrix=true` 可验证四个 Ubuntu 用户态；PR 默认检查最旧和
最新端点。这些工作流只有仓库只读权限。

发行流程接受与 CMake 版本匹配、指向 main 历史的 `vMAJOR.MINOR.PATCH` 标签。
标签 push 只验证。流程构建 Linux 和 Windows 包，以相同 Linux artifact 验证完整
Ubuntu/Noetic 矩阵，并要求相同提交已有成功的 Linux、Windows、style 和 report-card
可信 push/手动运行。若质量检查尚未完成，完成后重跑失败的发行门禁。

需要草稿时，在该版本标签上手动运行 **Validate release and optionally create draft**，
显式设置 `create_draft=true`。流程检查本次运行的两份 Linux Release、Windows x64
Debug/Release 包，核对校验值、来源、版本、基线、配置和 CRT，生成发行清单，重新核对
远端标签，拒绝覆盖已有 Release，再上传已验证的相同字节，并下载复核。

只有草稿 job 获得仓库写权限。流程不会自动正式发布或移动标签。维护者应按正常流程核对
草稿和证据后发布。上传失败可能留下不完整草稿，重试前须检查；流程有意拒绝覆盖。
已发布内容需要修正时发布新版本，不替换旧包。本次 CI 变更的评审不需要创建标签或 Release。
