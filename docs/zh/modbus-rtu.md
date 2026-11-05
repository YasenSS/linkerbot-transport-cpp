# 通用串口与 Modbus RTU

新增可选模块提供 Linux/Windows 串口字节流与 Modbus RTU 客户端；不包含灵巧手
寄存器映射、型号发现或运动控制。现有 CAN/SLCAN 接口不变，SLCAN 不是 RTU。
完整接口和失败语义见[英文说明](../en/modbus-rtu.md)。

- 构建开关：`LINKERBOT_COMM_ENABLE_SERIAL`、`LINKERBOT_COMM_ENABLE_MODBUS`，默认开启；
  Modbus 依赖 serial。仅使用 CAN 可同时关闭两项。
- 独立导出目标：`linkerbot::serial`、`linkerbot::modbus`；原 `linkerbot::comm` 不链接新增库。
- 默认串口格式 115200 / 8E1，必须按设备协议显式配置。Linux 支持驱动提供的内核 RS485
  方向控制；Windows 要求自动方向适配器，不实现软件 RTS 时序。
- 一个物理串口共享一个 `RtuSession`，不同从站也只允许一个在途事务；队列有界。
- 支持功能码 03/04/06/10，拒绝广播；地址为 PDU 零基地址。长度使用 `size_t`，不截断 256 字节。
- 必须传入有限的绝对截止时间，覆盖排队、静默间隔、发送、排空与接收。没有自动重试。
- 已接纳事务的错误保留在 `TransactionResult::error`；写入须检查结果，不能只看函数返回。
  设备异常回复不等于写入成功；正常写回显也不代表机械到位。
- 部分发送或应答不确定时会话进入故障态。恢复会取消旧事务并更新共享代次，不重放命令。
  RTU 没有事务 ID；调用方必须在恢复前确认旧回复已不可能继续到达，不能认为重开串口就解决迟到回复。
- `ByteStream` 支持注入测试实现。Linux PTY 和离线测试不代表真实 RS485/USB 时序验收；
  Windows 故障驱动的异步取消清理可能超过截止时间，不保证硬实时。

hand SDK 及设备能力需在后续独立变更中适配，本次没有修改。
