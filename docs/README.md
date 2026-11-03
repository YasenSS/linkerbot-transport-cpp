# Documentation / 文档索引

This index and the English overview in [README](../README.md#english) describe the
current implementation. Detailed guides below are maintained in Chinese.
Historical hardware statements are separated in the validation guide.

本索引对应当前源码。先完成 FakeTransport 的离线构建和接入，再选择真实后端。
文档中的真实设备示例需要按已确认的硬件参数调整，不会由离线 CI 自动执行。

## 维护状态

当前维护入口是根目录 README、本索引和 `docs/zh/`。验证文档中指向固定旧提交的内容是
档案证据，不会覆盖当前指南。将来如恢复旧译文，应放入 `docs/archive/`，在文件顶部标注历史状态
并链接本索引；历史译文不加入现行 API 导航。

## 入门与构建

- [构建、安装与 CMake 接入](zh/build.md)：依赖、选项、Debug/Release、覆盖率、安装消费。
- [Windows 构建与独立通信包](zh/windows-build.md)：MSVC、运行库、ZIP、来源和校验。
- [验证记录与支持边界](zh/validation.md)：固定提交证据、CI 范围、历史实机陈述。

## API 与后端

- [公共 API 与线程/所有权契约](zh/api.md)：帧、传输层、Dispatcher、共享总线、Pool、取消和测试替身。
- [Session、路由与生命周期](zh/session.md)：所有权、回执、共享、取消和恢复。
- [SocketCAN](zh/socketcan.md)：Linux 网络接口、Classic/FD、超时及权限。
- [MiniCANFD](zh/minicanfd.md)：平台 ABI、厂商库、枚举及资源重试。
- [PCAN](zh/pcan.md)：原生通道、时序、加载与故障诊断。
- [SLCAN](zh/slcan.md)：Lawicel/CANable 配置、串口、ACK 和吞吐边界。

## 运行与维护

- [错误、诊断与排障](zh/troubleshooting.md)：错误类别、状态指标、回执与常见问题。
- [运行、升级与回滚](zh/operations.md)：固定依赖、数据保留、重连及升级顺序。
- [贡献流程](../CONTRIBUTING.md)、[版本变更](../CHANGELOG.md)、[第三方组件](../NOTICE)。
- [支持范围与敏感报告](../SECURITY.md)：普通可靠性问题与敏感内容的不同提交路径。

安装后本索引位于 `<prefix>/share/linkerbot-comm/docs/README.md`。文档间使用相对链接，
源码树和完整安装包都可浏览；源码文件名作为定位线索，不代表安装包包含测试源文件或 CI 配置。
