# 公共 API、线程与所有权契约

[English](../en/api.md) · [文档索引](../README.md) · [Session](session.md) · [错误与诊断](troubleshooting.md) ·
[运行、升级与回滚](operations.md)

本文给出 `include/linkerbot/` 下 CAN 公共接口的选择和边界。项目使用 C++17；设备命令、身份核验、
CAN 请求/回复关联及业务幂等由上层 SDK 实现。串口和 RTU 见[模块指南](modbus-rtu.md)。
构造 CAN 对象或执行 CAN I/O 时链接
`linkerbot::can`；旧扁平 include 使用 `linkerbot::can-compat`。`linkerbot::headers`
只提供头文件搜索路径和 C++17 要求，不能替代实现库。

## 接口层次

| 层次 | 主要类型 | 适用场景与所有权 |
| --- | --- | --- |
| 帧和值 | `can::Message`、`TransportHealth`、错误类型 | 描述帧、状态和稳定错误分类，不拥有设备 |
| 物理后端 | `can::CANTransport` 及 SocketCAN/SLCAN/PCAN/MiniCANFD | 一个对象拥有一条物理连接；构造取得资源，`close()`/析构释放 |
| 接收与物理批次边界 | `can::CANDispatcher` | 单独接收线程、订阅广播，并串行化物理发送批次 |
| 可恢复共享总线 | `can::ManagedCANBus` | 保存 factory，显式 `start/stop/recover`，兼容旧发送队列 |
| 进程内连接池 | `can::SharedCANBusPool`、`CANBusLease` | 相同 key 共享总线；lease 明确持有和重试最后一次关闭 |
| 多设备共享运行时 | `transport::TransportRuntime`、`DeviceSession`、`DeviceOperation` | 独立设备注册与生命周期，共用物理 Session/发送器 |
| 直接 Session 与兼容共享 | `transport::Session`、`RouteInbox`、`SendReceipt` | 精确路由、单发送调度器、deadline、取消和 generation 恢复 |
| 离线测试 | `can::FakeTransport` | 内存收发、错误注入和并发门，不代表真实适配器 |

新多设备接入使用 `TransportRuntime`，每个逻辑设备独立持有 `DeviceSession`，共用现有 Session
发送器，见[设备会话](device-sessions.md)。直接使用 Session 的接口继续保留；实现新后端或兼容旧代码时可使用更低层接口。

## 帧与后端解释

`Message.dlc` 是载荷**字节数**，不是线上 0–15 DLC 编码。标准 ID 最大 `0x7ff`，扩展 ID
最大 `0x1fffffff`；扩展帧必须显式设置 `is_extended=true`。Classic 最多 8 字节，FD 最多
64 字节；`is_brs=true` 必须同时设置 `is_fd=true`。

| 后端 / 模式 | `format` 未指定且 `is_fd=false,is_brs=false` 的发送解释 |
| --- | --- |
| SocketCAN Classic、PCAN Classic、Lawicel Classic | Classic |
| SocketCAN FD、MiniCANFD | 兼容行为：FD、无 BRS |
| PCAN FD、CANable FD | Classic；FD 帧必须显式设置 `is_fd=true` |

`Message::format` 可显式选择经典/FD 编码，不改变上述默认值。可选 `timestamp` 和 `is_rx`
保存后端接收元数据，发送忽略这两项；时钟及后端限制见 [CAN 元数据说明](can-metadata.md)。
公共结构新增字段后须用匹配的头文件和库重新编译。

后端可能把 9、10、11 等 FD 字节长度向上补零到可表示的线长。上层解析必须使用返回的实际
长度并校验业务格式，不能假定后端接受等于 CAN 对端 ACK 或设备已执行。

## `CANTransport` 实现契约

自定义后端继承 `can::CANTransport` 时必须满足：

- `read()` 使用有限接收等待；正常等待超时返回 `std::nullopt`。空结果表示本次调用未返回应用帧；
  后端也可在过滤帧后提前返回，例如 SocketCAN 过滤 RTR 帧或错误帧。因此空结果不证明
  已等待完整接收超时预算，也不证明没有总线流量。链路故障、已关闭状态或非法接收数据
  可抛异常；操作系统或驱动调用可能延长实际耗时，不能将接收等待配置解释为硬性返回时限。
- dispatcher 线程可与发送线程并发调用 `read()` / `write()`；实现须保护相应资源。
- `write()` 在后端接受帧后返回，失败使用稳定的 `CANError` 子类，并尽量保留系统错误或原生状态。
- 默认 `write_batch()` 逐帧调用 `write()`；合并为一个原生调用的后端无法可靠指出部分成功帧。
- `prefers_write_batch()` 只表示无帧间 gap 时可使用原生批次；`initial_delay` 仍在批次前执行，
  且后端自身的数量限制继续生效。
- `close()` 拒绝新 I/O、协调在途操作，并在仍持有可释放资源时保留重试所有权。

驱动 Scan/Open/Init/Close/Exit 或同步 write 没有取消 API 时，本库不能提供硬性线程终止时限。

## Dispatcher、ManagedCANBus 与 Pool 默认值

`CANDispatcher` 使用一个接收线程。默认连续 10 次计入故障的接收错误后进入 faulted，错误退避
基准 5 ms、上限 50 ms；超时和校验错误不计入该阈值。订阅回调连续抛出 3 次异常后被禁用，
可从 health 中读取禁用数量和最近回调错误。外部线程调用 unsubscribe/stop 会等待已进入的回调
结束；回调线程自身不能同步等待需要该线程退出的生命周期操作。

`ManagedCANBus` 构造只保存 factory，不打开设备。当前 `Options` 默认值为：

- `send_queue_size=2000`
- `send_batch_max=32`
- `send_interval=0 ms`
- dispatcher 使用上述 10 次错误及 5/50 ms 退避默认值

`start()`、`stop()`、`recover()` 串行执行。`queue_send()` 和 `can::SendReceipt` 是旧发送接口；
新设备代码优先使用 Session 的批次和 `transport::SendReceipt`，两种回执不能混用。

`CANBusKey` 的 backend、endpoint、device/channel、FD 模式及 nominal/data bitrate 共同定义逻辑
配置；物理冲突判断不含速率和 FD 字段，因此同一物理通道不能同时用另一套配置重新申请。
`CANBusLease::close()` 失败时 lease 保留总线、key 和重试所有权。接收/故障回调中的最后一次释放
使用进程内 deferred cleanup；由外部线程执行 retry/drain，不能删除 Pool 记录或换 key 伪装成功。

## Session、deadline 与取消

Session 默认发送队列容量为 2000 个**等待批次**，路由 inbox 默认容量为 512 帧；正在执行的
批次不占等待队列容量，但计入 `pending_tx`。默认策略为 FIFO、priority 0、deadline 无上限。
完整的共享、路由、回执、恢复和退出契约见 [Session 指南](session.md)。

`transport::Clock` 是 `std::chrono::steady_clock`，Session deadline、回执时间和
`RoutedMessage::received_at` 不是 UTC/Unix 时间。`Message::timestamp` 是独立的后端元数据，
须按其时钟和单位解释。
`CancellationSource` 与其 token 共享一个原子、单向状态；取消可重复调用但不可撤销。取消、deadline
或等待超时都不能证明已经进入驱动的帧未送达，业务层仍须核对 `sent_frames`、
`current_frame_uncertain` 和设备状态。

## SDK 兼容接口

`SessionOptions` 可选地配置 FIFO/latest 容量计量、同 endpoint FIFO、等待老化、原生最小
发送周期和连续发送失败终止阈值，须在接受任何发送前配置；默认仍为 2000 个等待批次。
`SendOptions` 新增准入锁内的预期 generation 校验、等待年龄准入和
`DeadlinePolicy::kStartOnly`。`replace_latest_if_pending()` 在旧槽位已领取时不会新建任务。
`stop_endpoint()` 关闭准入并排空已领取任务，与原有会取消任务的 `cancel_endpoint()` 不同。
单位、排序、超时和共享所有权见 [Session SDK 选项](session.md#sdk-发送调度选项)。

`SendReceipt::wait_result_until()` 将保存的终态异常放在 `SendResult` 返回，不重新抛出；
原等待接口保持异常行为。`ChangeSignal` 提供共享、可合并的 revision 通知，供外部桥接等待，
不执行应用回调，不增加载荷队列。路由提取器保留原始消息并交付逻辑路由 key；原生领取和
路由接收事件序号属于同一个 Session 事件域。这些接口为后续绑定接入唯一发送队列提供基础，
不包含 Python 绑定，也不代表实机验收。

## 错误与健康状态

`CANError` 提供稳定的 `code()`、可选 `system_error()` 和 `native_status()`；不要解析 `what()`
字符串作为程序分支。`transport::CancelledError` 不继承 `CANError`。错误类别、回执 outcome 和
health 字段见 [错误与诊断](troubleshooting.md)。

`health()` / `get_health()` 返回当前对象的软件快照，不主动探测硬件；`connected()` 只表示状态为
open。`pending_tx`、订阅数和连续错误数等 gauge 会随状态变化；Session 的累计计数只覆盖该
Session 对象生命周期，重建后重置且不持久化。单调时钟字段需要由上层转换后才能用于带墙钟时间的报表。

## FakeTransport 测试边界

`FakeTransport` 可注入接收帧、记录发送历史、模拟 read/write/batch/close 失败并控制写入门，适合
单元测试和无硬件 consumer 验证。`on_request()` handler 在 FakeTransport 内部锁持有期间同步执行，
不得从 handler 回调同一 transport。它不模拟驱动调度、USB、CAN 仲裁、对端 ACK、bus-off 时序
或真实吞吐，不能代替后端 fixture 和实机验收。

## 兼容、持久化与升级

`0.3.x` 不承诺任意编译器、CRT 或构建选项之间的二进制 ABI；应用应使用配套头文件和静态库重新
编译。新增 `[[nodiscard]]` 会让启用 `-Werror` 且忽略结果的源码需要显式 `(void)`，但不改变
符号 ABI。旧扁平头只用于迁移，新增代码使用 `<linkerbot/...>` include 路径。

本库没有数据库、Redis、磁盘队列或跨重启确认记录，也不要求自身持久卷。进程内 registry、
扫描快照、inbox、回执和 deferred cleanup 都不是持久状态。上层的日志、校准、设备身份和业务
确认须自行保留；升级顺序、人工核对、旧记录保留及回滚限制见 [运行、升级与回滚](operations.md)。
