# Session、路由与生命周期

[文档索引](../README.md) · [错误与诊断](troubleshooting.md) · [运行与升级](operations.md)

`include/linkerbot/transport/session.hpp` 是面向设备 SDK 的会话接口。
它在同一条物理连接上提供精确接收路由和一个发送调度线程。设备协议与请求/回复匹配仍由上层实现。

## 创建与资源归属

| 入口 | 行为 | `close()` 的资源处理 |
| --- | --- | --- |
| `Session::create(factory, tx_capacity=2000)` | 创建并启动 ManagedCANBus，可重新调用 factory 恢复 | 停止自有总线 |
| `Session::own(unique_ptr<CANTransport>)` | 接管已构造的 transport，通过自有 dispatcher 收发 | 停止自有 dispatcher；不支持物理 recover |
| `Session::share(shared_ptr<ManagedCANBus>)` | 启动或复用总线，同一总线返回同一 Session | 关闭会话，不停止外部总线 |
| `Session::lease(CANBusLease)` | 同一总线复用 Session，持有传入的租约 | 释放租约，最后一个租约关闭连接 |
| `Session::borrow(CANDispatcher&)` | 借用已运行的 dispatcher，同一 dispatcher 复用会话 | 取消订阅，不停止外部 dispatcher；不支持 recover |

`Session` 创建后即可收发，没有另一个 `open()` 步骤。`ManagedCANBus` 本身的构造只保存
factory，不打开设备；`start()` 才打开。需要多设备共享物理通道时，通过
`SharedCANBusPool::acquire(key, factory)` 获得 lease，再交给 `Session::lease()`。

Pool 对相同 key 共享连接，对同一物理通道的 FD/速率冲突配置报错。调用者须统一 endpoint
和库路径等身份字段；Pool 不会自行把不同路径别名识别为同一硬件。最后一个 lease 的
`close()` 失败时仍持有资源，须由该持有者重试关闭；此时新 acquire 抛 `FaultedError`。
`Session::close()` 也可重试失败的资源释放。会话关闭后不能重新启用。

`share()` / `lease()` / `borrow()` 复用的是同一个 Session，而不是独立逻辑连接。
任一共享使用者调用 `close()` 会让所有使用者的路由失效，并释放该 Session 持有的所有 lease；
`share()` 本身不停止外部 bus，但其他设备不能继续使用已关闭的 Session。
应由统一所有者关闭会话，单设备退出时只关闭自己的 inbox、取消自己的 endpoint。
注册表里仍存活的已关闭 Session 会拒绝再次 share/borrow；旧对象释放后才能创建新的会话。

## 接收路由

`subscribe_routes({{id, extended}, ...}, capacity=512)` 原子注册一组 key，共享一个有界 inbox。
同一 key 只能属于一个未关闭的 inbox；标准 ID 与扩展 ID 是不同 key。
`update_routes(inbox, keys)` 原子替换路由组，冲突时保留原有注册。

- `receive_until(steady_clock_deadline)` 等待一帧；超时返回 `std::nullopt`。
- `try_receive()` 立即取帧，没有消息时返回空值。
- 队列满拒绝新帧，并累计 inbox 的 `overflow` 和会话的 `overflows`，不覆盖已有帧。
- `close()` 清空队列、释放 key 并唤醒等待者；关闭后的接收抛 `ClosedError`。
- 每帧附带 generation、sequence 和接收时间。FD/BRS、ID 和载荷合法性先由会话检查，
  具体型号的回复格式仍由上层校验。

## 批次、回执与取消

`MessageBatch` 包含 frames、帧间 `gap` 和首帧前 `initial_delay`。
`submit(batch, options)` 入队并立即返回 `transport::SendReceipt`；队列满抛 `QueueFullError`。
容量按等待中的批次计，不含正在执行的批次。单个批次的帧数、所有路由组数量没有全局上限，
因此有界队列不等于总内存有界；应用还需限制单批次帧数、提交速率和总路由量。
`send(batch, options)` 提交后等待，使用相同调度路径。

默认 `SendPolicy::fifo`；priority 越大越先调度，等待时间参与优先级计算，相同排序保留先后顺序。
`SendPolicy::latest` 要求非空 endpoint 和 family，只替换同组尚未执行的 latest 批次，
旧回执变为 `superseded`，已经进入执行的批次不被替换。
Session 与旧 dispatcher/managed-bus 发送共用物理批次锁，批次的间隔内也不允许其他发送插入。
priority 只影响待执行任务，不能抢占在途批次；很长的 gap/initial_delay 或大批次会阻塞其他设备。
endpoint/family 是调用者定义的调度键，不自动等同于 CAN ID。使用明确的 endpoint；
`cancel_endpoint("")` 会匹配未命名任务，不能当作只取消一个设备的安全默认。
无帧间间隔时，支持 batch 的后端可合并发送。

`SendOptions.deadline` 是单调时钟绝对时间，覆盖排队、发送锁等待及帧间等待。
`cancellation` 可取消未完成任务；`cancel_endpoint(name)` 同时取消该 endpoint 的排队与在途任务。
进入驱动后的当前写入可能继续完成；最后一帧已被后端接受后到达的取消不会覆盖 `dispatched`。

回执的 `result()` 不等待，提供 outcome、generation、各阶段时间、已接受帧数及异常。
`wait()` / `wait_until()` 在失败、过期、取消或被替换时抛出保存的异常。
只等待超时不会取消任务，可继续查询同一回执。
`sent_frames` 表示已知被后端接受的数量；`current_frame_uncertain=true` 表示报错的当前
帧或原生批次可能已部分送达，不能据此自动重放。
后端接受也不等于 CAN 对端 ACK 或设备业务执行成功；CANable SLCAN 甚至只确认本机串口驱动接收字节。

`can::SendReceipt` 是旧 `ManagedCANBus::queue_send()` 的回执，提供完成/异常、
`wait()` / `wait_for()`，与上述 `transport::SendReceipt` 是不同类型。

## 恢复、关闭与诊断

从 `session->health().generation` 保存观测值，传给 `recover(observed_generation)`。
使用同一旧 generation 的并发调用只进行一次成功重开；恢复失败可重试。
恢复关闭旧 inbox、取消旧批次，且不重放控制帧。恢复后必须重新注册路由并重新核验设备身份。
`own()` / `borrow()` 没有重建连接的 factory，不能执行物理恢复。

`health()` 提供队列、帧/字节计数、无路由帧、非法帧、溢出、恢复次数及最后错误。
`transport_health()` 补充底层传输状态。两者读取缓存，不探测硬件。
关闭会话会唤醒接收者并等待发送线程退出；耗时仍受在途驱动调用约束。

同步 `close()`、`recover()` 必须由外部线程调用；从接收或发送回调调用会抛 `ValidationError`。
在回调内释放最后一个 Session 引用会交给清理线程处理。借用的 dispatcher 必须保持存活，
并在销毁前由外部线程调用 `Session::drain_deferred_cleanup()` 等待延迟清理结束。

## 恢复调用示例

下面是使用已有可恢复 `session` 的片段，不是独立程序。factory 每次必须创建新的可用后端；
不能重复返回已关闭的同一适配器。

```cpp
const auto observed = session->health().generation;
const auto generation = session->recover(observed);
auto inbox = session->subscribe_routes({{0x28, false}});
// 此处由上层重新核验设备身份/状态；不自动重发恢复前的控制命令。
(void)generation;
```

成功重开后，同一旧 observed_generation 的其他调用者取得当前代次而不再重开。
失败恢复不会记为成功 recoveries；后续可再次尝试。显式 close 是最终关闭，不允许 recover。
底层 write/write_batch 接口没有 deadline 参数，进入驱动后的调用不能由 Session 强制打断；
`recover()` 和 `close()` 可能等待它返回，deadline 不是整个生命周期操作的硬超时保证。

`result()` 可在异常后查看 outcome 和已接受帧数。回执的 `accepted_at`、`claimed_at`、
`completed_at`、接收的 `received_at` 均基于单调时钟，不能直接当 Unix 时间写入报表。
空的默认构造 SendReceipt 不可等待/读取，会抛 ValidationError。
