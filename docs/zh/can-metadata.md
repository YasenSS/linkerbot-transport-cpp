# CAN 帧类型与接收元数据

新增字段保持现有源码调用和聚合初始化方式兼容。使用方须用匹配的头文件和库重新编译；`Message` 和配置结构大小增加，不保证已编译二进制的 ABI 兼容。本次不接入 hand/arm，也不修改 Python 生产绑定。

`Message::format` 默认 `FrameFormat::kUnspecified`，保留原有后端行为：SocketCAN/vendor 的 FD 链路将未标记帧视为 FD，PCAN/SLCAN 将其视为经典帧。显式 `kClassic` 可在 FD 通道发送经典帧，要求不超过 8 字节且无 FD/BRS 标志；`kFd` 在经典链路上会被拒绝。非法枚举、冲突标志和超长经典帧在 Session 入队或替换 latest 任务之前被拒绝。接收帧只保留既有 flags，新的发送覆盖字段保持未指定。接收路由 key 使用仲裁 ID 和标准/扩展标志，不使用这个发送端经典/FD 覆盖字段。

`Message::timestamp` 可为空，携带原生数值、单位、时钟、来源和计数器位宽。它与 Session 的本地单调时钟 `received_at` 分开保存；发送忽略接收元数据，不用主机读取时间填补缺失值。

- SocketCAN 的 `SocketCANOptions::timestamp_mode` 默认 `kDisabled`。`kSoftware` 请求内核系统时间戳；`kHardware` 请求设备原始硬件时间戳，不请求软件回退。驱动、接口硬件配置和时钟关联仍需部署侧确认。选项设置成功不代表硬件实际提供时间戳。缺失、截断或非法元数据仍为空。
- PCAN 保留驱动微秒值，经典模式合并毫秒回绕字段，FD 模式保留完整 64 位值。时钟和来源均标为 `kDriver`；API 没有逐帧硬件/软件来源标记，不推定 UTC 纪元或硬件精度。
- vendor 保留原始 32 位 `TimeStamp`，包含零值和回绕，单位为 `kRawTicks`，时钟为 `kVendorRaw`。不猜测单位或纪元，不展开回绕，与 Python ctypes 后端保留原始整数的方式一致。
- 当前 SLCAN 固件配置不提供接收时间戳，不生成替代值，也不更改启动命令。

SocketCAN 和 PCAN 的 `receive_own_messages` 默认 false，开启后分别设置内核或驱动选项。PCAN 需要驱动支持 `PCAN_ALLOW_ECHO_FRAMES`，动态 setter 对既有注入 API 和默认调用保持可选。缺失 setter 时在初始化前拒绝开启；设置失败时执行已有资源清理/重试协议。

`Message::is_rx` 可为空；SocketCAN 的 `MSG_CONFIRM` 和 PCAN 的 echo 标记用于区分自身发送帧。vendor/SLCAN 缺少可核实的等价接口，方向保持未知，开启 receive-own 会在初始化前被拒绝，不用软件回填模拟回显。发送和接收继续使用现有原生通道，没有新增发送调度队列。

共享 Runtime 的配置指纹须包含所选新参数。部署证据校验要求后端能力、显式配置及相应功能的实机记录一致。每套实际适配器、驱动和固件组合的时间戳来源/时钟、回显和混合经典/FD 总线收发仍标为**待实机验证**。完整接口说明和参考资料见[英文文档](../en/can-metadata.md)。
