# 版本变更

[English](CHANGELOG.en.md) · 简体中文

## 0.3.0（未发布）

- 接入 Linux ARM64 C++ Report Card 完整分析，上传 JSON、Markdown 和 SVG 报告；
  使用只读权限并保留既有严格质量门槛。

- 明确接收空结果与超时、过滤帧的区别，补充 Session 同步提交异常及 deadline 后完成语义；
  提供可复现的 sanitizer 命令并标明静态检查对独立 quickstart 的覆盖范围。

- 补齐中英文详细指南、贡献说明、版本变更和支持政策，统一超时、回执、确认、恢复与清理术语。

- 拆分中英文 README，补充可独立构建和运行的安装包最小示例；澄清 CMake 3.16
  接入与现代 CTest 命令的版本要求，完善 Session 回执、恢复和关闭的公共注释。

- 仓库更名为 `linkerbot-transport-cpp`。为保持接入兼容，CMake 包名仍为
  `linkerbot-comm`，链接目标仍为 `linkerbot::comm` 系列，库文件和安装目录名称保留。
- 提供 SocketCAN、MiniCANFD、PCAN Classic / FD 及原生串口 SLCAN 后端。
  SLCAN 支持 Lawicel Classic、显式 CANable FD 配置及 Linux / Windows 原生串口，仍属实验性。
- 完善 Session、共享总线、接收路由、有界发送、超时、取消、关闭与恢复契约，
  修复生命周期及并发问题，并增加相应离线回归。
- 增加 Windows x64 / MSVC 构建、原生测试 DLL、ZIP、构建来源与 SHA256、安装及
  中文空格路径解压接入验证。厂商 DLL、驱动及测试 DLL 不随发行包提供。
- 增加 GitHub Linux amd64/ARM64 Debug/Release 离线 CI、Session 恢复及 registry
  active-generation/completed-attempt/initializer handoff 重复回归和汇总生产行覆盖率至少 95% 的门槛；
  增加 Clang 18 + libc++、ASan+UBSan 与 TSan 双架构质量矩阵。
- 使用 Clang 18 统一格式，增加 Linux ARM64 clang-format/clang-tidy 门禁；libc++ 的
  `-Wall -Wextra -Wpedantic -Werror` 覆盖生产库、测试、fixture、示例和安装后消费者，
  GitHub Actions 依赖固定到完整提交 SHA，并使用 Node.js 24 action 版本。
- 修复静态 `Session::share()`、`Session::lease()` 或 `Session::borrow()` owner 在进程退出时
  受 registry 析构顺序影响的可靠性问题，并增加 shared/borrowed 独立进程退出回归。
- `SendReceipt::result()`、`wait_until()`、`completed()` 与 `RouteInbox::health()` 标记为
  `[[nodiscard]]`；启用 `-Werror` 的下游若有意忽略返回值，须显式转换为 `void`。
- 整理双语入口与中文指南，补齐 Linux/SocketCAN、构建选项、诊断、升级回滚和
  安装文档索引。历史实机转述集中标记，不作为当前硬件验收结论。
- `0.3.x` 与 `linkerbot-hand-cpp 0.3.x` 配套，通信库版本不低于 hand 版本；
  SDK 更新后配套重编译应用，不保证跨构建的二进制 ABI 兼容。
