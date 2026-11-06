# Documentation / 文档索引

This index and the [English](../README.md) and [Chinese](../README.zh-CN.md) READMEs describe the
current implementation. Each guide has an English and a Chinese edition.
Historical hardware statements are separated in the validation guide. Source guides
include the Changelog's Unreleased APIs; published SDKs must be checked against their
matching tag, headers and bundled documentation, rather than the version number alone.

## English guides

Start with the offline quickstart, then choose a physical backend. Device examples
require parameters confirmed for the actual hardware; offline CI does not run them.

- [Terminology and observable behavior](en/terminology.md)
- [Package names and compatibility](en/naming.md)
- [Build, install and CMake integration](en/build.md)
- [Release downloads, checksums and documentation](en/releases.md)
- [CI packages and compatibility validation](en/ci.md)
- [Windows builds and standalone packages](en/windows-build.md)
- [Public API, threading and ownership](en/api.md)
- [Session, routing and lifecycle](en/session.md)
- [Per-device sessions and shared Runtime](en/device-sessions.md)
- [CAN frame selection, timestamps and receive-own messages](en/can-metadata.md)
- [Serial byte streams and Modbus RTU](en/modbus-rtu.md)
- Backends: [SocketCAN](en/socketcan.md), [MiniCANFD](en/minicanfd.md),
  [PCAN](en/pcan.md), [SLCAN](en/slcan.md)
- [Errors, diagnostics and troubleshooting](en/troubleshooting.md)
- [Operations, upgrades and rollback](en/operations.md)
- [Performance measurement and ARM64 results](en/performance.md)
- [Validation evidence and limits](en/validation.md)
- [Contributing](../CONTRIBUTING.md), [Changelog](../CHANGELOG.en.md),
  [support and sensitive reports](../SECURITY.md)

Both editions describe the same implementation. Changes to API behavior, defaults,
commands or validation scope must update both editions together. Keep identifiers
and fixed-commit evidence unchanged when translating. Historical translations belong
under `docs/archive/`, with a historical-status notice and a link to this index.

本索引对应当前源码，包含 Changelog 的 Unreleased 接口；已发布 SDK 应核对对应标签、头文件
和随包文档，不能只按版本号推定包含所有新能力。先完成 FakeTransport 的离线构建和接入，再选择真实后端。
文档中的真实设备示例需要按已确认的硬件参数调整，不会由离线 CI 自动执行。

## 维护状态

当前维护入口是根目录中英文 README、本索引、`docs/en/` 和 `docs/zh/`。涉及行为、默认值、命令
或验证范围的修改须同步两种语言。验证文档中指向固定旧提交的内容是
档案证据，不会覆盖当前指南。将来如恢复旧译文，应放入 `docs/archive/`，在文件顶部标注历史状态
并链接本索引；历史译文不加入现行 API 导航。

## 入门与构建

- [术语与可观测行为](zh/terminology.md)：后端接受、CAN 确认、业务确认、超时和恢复的精确定义。
- [包名、目标及兼容迁移](zh/naming.md)
- [构建、安装与 CMake 接入](zh/build.md)：依赖、选项、Debug/Release、覆盖率、安装消费。
- [发行版下载、校验与文档](zh/releases.md)：稳定版入口、源码标签、SDK 包和文档勘误。
- [CI 产物与兼容性验证](zh/ci.md)：Ubuntu 20.04 至 26.04、Noetic、缓存和草稿发行。
- [Windows 构建与独立通信包](zh/windows-build.md)：MSVC、运行库、ZIP、来源和校验。
- [性能测量与 ARM64 结果](zh/performance.md)：计时、分配、空闲唤醒与优化取舍。
- [验证记录与支持边界](zh/validation.md)：固定提交证据、CI 范围、历史实机陈述。

## API 与后端

- [公共 API 与线程/所有权契约](zh/api.md)：帧、传输层、Dispatcher、共享总线、Pool、取消和测试替身。
- [Session、路由与生命周期](zh/session.md)：所有权、回执、共享、取消和恢复。
- [每设备会话与共享 Runtime](zh/device-sessions.md)：独立生命周期与单发送器。
- [CAN 帧格式与接收元数据](zh/can-metadata.md)：显式经典/FD、时间戳及回显限制。
- [通用串口与 Modbus RTU](zh/modbus-rtu.md)：独立模块、寄存器事务、方向控制及恢复边界。
- [SocketCAN](zh/socketcan.md)：Linux 网络接口、Classic/FD、超时及权限。
- [MiniCANFD](zh/minicanfd.md)：平台 ABI、厂商库、枚举及资源重试。
- [PCAN](zh/pcan.md)：原生通道、时序、加载与故障诊断。
- [SLCAN](zh/slcan.md)：Lawicel/CANable 配置、串口确认、CAN 确认和吞吐边界。

## 运行与维护

- [错误、诊断与排障](zh/troubleshooting.md)：错误类别、状态指标、回执与常见问题。
- [运行、升级与回滚](zh/operations.md)：固定依赖、数据保留、重连及升级顺序。
- [贡献流程](../CONTRIBUTING.zh-CN.md)、[版本变更](../CHANGELOG.md)、[第三方组件](../NOTICE)。
- [支持范围与敏感报告](../SECURITY.zh-CN.md)：普通可靠性问题与敏感内容的不同提交路径。

安装后本索引位于 `<prefix>/share/linkerbot-transport/docs/README.md`。文档间使用相对链接，
源码树和完整安装包都可浏览；源码文件名作为定位线索，不代表安装包包含测试源文件或 CI 配置。
