# 版本变更

## 0.3.0（未发布）

- 仓库更名为 `linkerbot-transport-cpp`。为保持接入兼容，CMake 包名仍为
  `linkerbot-comm`，链接目标仍为 `linkerbot::comm` 系列，库文件和安装目录名称保留。
- 提供 SocketCAN、MiniCANFD、PCAN Classic / FD 及原生串口 SLCAN 后端。
  SLCAN 支持 Lawicel Classic、显式 CANable FD 配置及 Linux / Windows 原生串口，仍属实验性。
- 完善 Session、共享总线、接收路由、有界发送、超时、取消、关闭与恢复契约，
  修复生命周期及并发问题，并增加相应离线回归。
- 增加 Windows x64 / MSVC 构建、原生测试 DLL、ZIP、构建来源与 SHA256、安装及
  中文空格路径解压接入验证。厂商 DLL、驱动及测试 DLL 不随发行包提供。
- 增加 GitHub Linux amd64/ARM64 Debug/Release 离线 CI、Session 恢复及 registry
  active-generation/initializer handoff 重复回归和汇总生产行覆盖率至少 95% 的门槛；
  增加 Clang 18 + libc++、ASan+UBSan 与 TSan 双架构质量矩阵。
- 整理双语入口与中文指南，补齐 Linux/SocketCAN、构建选项、诊断、升级回滚和
  安装文档索引。历史实机转述集中标记，不作为当前硬件验收结论。
- `0.3.x` 与 `linkerbot-hand-cpp 0.3.x` 配套，通信库版本不低于 hand 版本；
  SDK 更新后配套重编译应用，不保证跨构建的二进制 ABI 兼容。
