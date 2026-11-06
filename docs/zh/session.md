# Session、路由与生命周期

[English](../en/session.md) · [文档索引](../README.md) · [公共 API](api.md) · [错误与诊断](troubleshooting.md) · [运行与升级](operations.md)

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

如果最后一个 lease 在接收或故障回调中析构，Pool 会把关闭操作交给进程内的清理线程，
避免等待当前回调所属的 dispatcher。进程退出或销毁相关外部资源前，应由外部线程调用
`CANBusLease::drain_deferred_cleanup()`；若它抛 `FaultedError`，先排除后端关闭故障，再调用
`retry_deferred_cleanup()` 和 `drain_deferred_cleanup()`。`pending_deferred_cleanup()` 可用于
诊断待处理数量。失败任务会继续保留原 Pool 记录、物理 key 和重试所有权，不会为规避错误
而删除记录或改用新 key。以上 drain/retry 接口不能从接收或故障回调中调用。

`share()` / `lease()` / `borrow()` 复用的是同一个 Session，而不是独立逻辑连接。
任一共享使用者调用 `close()` 会让所有使用者的路由失效，并释放该 Session 持有的所有 lease；
`share()` 本身不停止外部 bus，但其他设备不能继续使用已关闭的 Session。
应由统一所有者关闭会话，单设备退出时只关闭自己的 inbox、取消自己的 endpoint。
注册表里仍存活的已关闭 Session 会拒绝再次 share/borrow；旧对象释放后才能创建新的会话。

factory 可以在同一线程重入不同的注册表 key（例如不同 `ManagedCANBus`）的 `share()`；同线程
重入同一 key，或注册表检测到初始化线程之间的等待环时，会抛 `ValidationError`。factory 不应
等待另一个线程对同一尚未完成 key 的 `share()`，因为这种发生在应用 factory 内的等待无法由
注册表观察和打断。

后端的 `close()` 及其析构函数不得调用 `Session::share()`、`lease()` 或 `borrow()`；Session
清理期间进入注册表会抛 `ValidationError`。这一限制同时适用于相同和不同 key，用于避免清理
线程等待自身 tombstone 或形成跨 key 的清理环。

最后一个 Session 引用消失后，注册表仍保留该代次的清理标记，直到订阅、发送线程、lease 和
自有总线完成同步或延迟清理；同一 key 的 `share()`、`lease()` 或 `borrow()` 会等待清理结束，
不会提前发布替代 Session。接收/发送回调不能等待自身触发的清理，此时会抛 `ValidationError`；
应从外部线程重试。

注册表节点、协调 mutex 和等待图在进程生命周期内保持可达，使静态 Session owner 在进程退出
阶段也能安全释放。清理完成后节点只保留弱引用和协调状态，不继续持有 Session、总线或驱动
资源；但不同 `ManagedCANBus` / dispatcher 对象身份产生的节点不会删除。长驻进程应
复用已管理的总线/dispatcher 对象，不能新建对象或换 Pool key 绕过关闭失败或配置冲突。
这些状态不写磁盘，不是持久队列或旧确认记录，进程退出后由操作系统回收。

## 接收路由

`subscribe_routes({{id, extended}, ...}, capacity=512)` 原子注册一组 key，共享一个有界 inbox。
同一 key 只能属于一个未关闭的 inbox；标准 ID 与扩展 ID 是不同 key。
`update_routes(inbox, keys)` 原子替换路由组，冲突时保留原有注册。

- `receive_until(steady_clock_deadline)` 等待一帧；超时返回 `std::nullopt`。
- `try_receive()` 不等待新帧到达，但会获取 inbox 互斥锁；有排队帧时取出一帧，否则返回空值。
  inbox 已关闭时抛 `ClosedError`。
- 队列满拒绝新帧，并累计 inbox 的 `overflow` 和会话的 `overflows`，不覆盖已有帧。
- `close()` 清空队列、释放 key 并唤醒等待者；关闭后的接收抛 `ClosedError`。
- 每帧附带 generation、sequence 和接收时间。FD/BRS、ID 和载荷合法性先由会话检查，
  具体型号的回复格式仍由上层校验。

## 批次、回执与取消

`MessageBatch` 包含 frames、帧间 `gap` 和首帧前 `initial_delay`。
`submit(batch, options)` 入队并立即返回 `transport::SendReceipt`；队列满抛 `QueueFullError`。
默认容量按等待中的批次计，不含正在执行的批次；下方 SDK 选项可改为按 FIFO 帧数计并独立限制 latest 槽位。
默认不限制单批次帧数和全局注册路由 key 数量。可配置 `SessionOptions::max_batch_frames`、
`max_route_keys` 和 `max_pending_rx` 增加相应限制，零保留原有不设上限的行为。
因此有界发送队列不等于总内存有界，应用还需限制提交速率和 inbox 容量。
`send(batch, options)` 提交后等待，使用相同调度路径。

提交会在入队前校验批次；token 已取消、deadline 已过期或 Session 已关闭时同步抛异常，
不会返回回执。后端特有的校验仍可能在随后发送时失败，并通过已返回的回执报告。

默认 `SendPolicy::fifo`；priority 越大越先调度，等待时间参与优先级计算，相同排序保留先后顺序。
`SendPolicy::latest` 要求非空 endpoint 和 family，只替换同组尚未执行的 latest 批次，
旧回执变为 `superseded`，已经进入执行的批次不被替换。
Session 与旧 dispatcher/managed-bus 发送共用物理批次锁，批次的间隔内也不允许其他发送插入。
priority 只影响待执行任务，不能抢占在途批次；很长的 gap/initial_delay 或大批次会阻塞其他设备。
endpoint/family 是调用者定义的调度键，不自动等同于 CAN ID。使用明确的 endpoint；
`cancel_endpoint("")` 会匹配未命名任务，不能当作只取消一个设备的安全默认。
无帧间间隔时，支持 batch 的后端可合并发送。
MiniCANFD 的单次原生批次上限为 100 帧；`gap=0` 时超过该值会以失败回执结束，详见
[MiniCANFD 默认值与批次边界](minicanfd.md#打开共享与关闭)。其他后端也可有自己的驱动限制，
“Session 不设全局帧数上限”不等于任意后端可接受无限批次。

`SendOptions.deadline` 是单调时钟绝对时间，默认 execution 策略覆盖排队、发送锁等待及帧间等待。
`cancellation` 可取消未完成任务；`cancel_endpoint(name)` 同时取消该 endpoint 的排队与在途任务。
进入驱动后的当前写入可能继续完成；最后一帧已被后端接受后到达的取消不会覆盖 `dispatched`。
`CancellationSource` 和由它生成的 token 共享一个单向状态；`cancel()` 可重复调用但不能撤销，
也不能中止厂商驱动中没有取消接口的同步调用。
deadline 在进入后端调用前及调度等待期间检查，最后一次后端调用成功返回后不再检查。
因此该调用可能在 deadline 之后完成，而回执最终仍为 `dispatched`；仅等待到 deadline 的
调用者此时可能已经收到等待超时异常。

回执的 `result()` 不等待任务完成，提供 outcome、generation、各阶段时间、已接受帧数及异常。
`wait()` / `wait_until()` 在失败、过期、取消或被替换时抛出保存的异常。
回执等待超时不会取消任务，可继续查询同一回执。
`SendReceipt::result()`、`wait_until()`、`completed()` 和 `RouteInbox::health()` 标为
`[[nodiscard]]`；确实不使用结果时应显式写 `(void)`，避免在 `-Werror` 消费者中产生告警。
`sent_frames` 表示已知被后端接受的数量；`current_frame_uncertain=true` 表示报错的当前
帧或原生批次可能已部分送达，不能据此自动重放。
后端接受也不等于 CAN 对端 ACK 或设备业务执行成功；CANable SLCAN 甚至只确认本机串口驱动接收字节。

`can::SendReceipt` 是旧 `ManagedCANBus::queue_send()` 的回执，提供完成/异常、
`wait()` / `wait_for()`，与上述 `transport::SendReceipt` 是不同类型。

## 恢复、关闭与诊断

从 `session->health().generation` 保存观测值，传给 `recover(observed_generation)`。
使用同一旧 generation 的并发调用只进行一次成功重开；恢复失败可重试。
恢复关闭旧 inbox、取消旧的未完成批次，且不重放控制帧；已完成回执仍可读取。恢复后必须重新注册路由并重新核验设备身份。
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

接收路由或发送线程内部故障会使 Session 保持终止状态，即使物理后端仍连接。
健康刷新不会重新启用会话，`recover()` 也会拒绝。应关闭并销毁该 Session，再创建新会话；
仅恢复物理连接不能重启已经退出的发送线程。

## SDK 发送调度选项

原有构造入口、`submit()`、`send()`、`cancel_endpoint()` 和回执等待接口保留签名及默认行为。
以下选项供绑定层复用 Session 的唯一发送线程；本库不提供 Python 绑定，也未迁移 hand/arm SDK。
使用新增接口须以配套头文件和库重编译消费者，不因此获得新的二进制 ABI 保证。

在第一次成功准入发送前调用 `configure(SessionOptions)`。尚未接受过发送时可调整配置；
恢复不会清零已接受发送的计数，已有发送准入后仍不能重新配置。`options()` 读取当前设置。
共享 Session 的统一所有者应在
接受流量前确定配置，并确认各使用者一致。

默认仍是 `tx_capacity=2000` 个等待批次的统一队列、`endpoint_fifo=false`、
`aging_quantum=10 ms`，不设置发送周期或连续发送失败终止阈值。
开启 `separate_policy_capacity=true` 后，FIFO 使用 `fifo_capacity`，latest 使用
`latest_capacity`，不再使用合并的 `tx_capacity` 限制。`fifo_unit` 可选
`CapacityUnit::kBatches` 或 `CapacityUnit::kFrames`；latest 始终按等待中的
`(endpoint, family)` 槽位计。已领取的批次不占等待容量。FIFO 批次整组接受或整组拒绝，
替换已有 latest 槽位不额外占用槽位。应用仍须限制单批次帧数和路由总量。

hand 式协议请求可按一批请求占一个 FIFO 单位；arm 式关节目标使用 `CapacityUnit::kFrames`，
七帧目标组需要七个空闲 FIFO 单位。不能拆成七次 `submit()`，否则不再具备整组准入语义。

`endpoint_fifo=true` 只将每个 endpoint 的首个等待 FIFO 批次列为候选。候选依次比较
priority 加等待老化、较早 deadline、较久未服务的 endpoint 和队列顺序。latest 任务仍可
与同 endpoint 的 FIFO 队首竞争。匹配 Python 仲裁器的默认老化周期时显式设置
`aging_quantum=50 ms`；保留 10 ms 则沿用 C++ 默认值。调度不能抢占已领取批次。

`replace_latest_if_pending(batch, options)` 要求 latest 策略及非空 endpoint/family。
它与领取操作使用同一队列锁：仅替换已有未领取槽位，否则返回 `std::nullopt`，不创建新任务。
普通 `submit(..., latest)` 则可在上一个批次已领取后新建等待槽位。被替换回执变为
`superseded`。原 `wait()`/`wait_until()` 仍重新抛出保存的 `CancelledError`；新增
`wait_result_until(deadline)` 等待完成后返回任意终态的 `SendResult`，不重新抛出其中的任务
异常，只有等待本身超时仍抛异常。Future 适配层可据此把 `superseded` 转为正常结果，
按自身公开异常契约转换其他状态。

## 准入、截止时间与 endpoint 排空

不支持的 `SessionOptions::fifo_unit`、`SendOptions::policy` 和 `SendOptions::deadline_policy`
在配置赋值或发送准入前抛出 `ValidationError`。拒绝不修改现有配置或等待中的 latest 任务；
支持的枚举值及默认行为不变。

`SendOptions.expected_generation` 非空时在准入锁内校验；旧代次抛 `ClosedError`，不返回回执。
它防止旧设备 actor 向恢复后的新连接提交任务，但不代替设备身份核验。

`max_pending_age` 是可选的正时长。等待或已领取任务中最早接受的任务达到该年龄时，准入抛
`TimeoutError`。这与新任务自身的 deadline 不同。按需要用于运动目标，不应自动套用到所有
生命周期或配置写入。`health().oldest_pending_age` 包括已领取任务；FIFO/latest 等待量不包括它。
`PendingAgeScope::kDefault` 在这些入口解析为整个 Session，在 DeviceSession 解析为设备 endpoint。
显式作用域按指定范围执行；共享额度域行为见[设备会话](device-sessions.md)。

`deadline_policy=DeadlinePolicy::kExecution` 保留物理发送锁等待和帧间等待期间的检查。
`DeadlinePolicy::kStartOnly` 保留排队、锁等待和首帧延迟检查，在第一次进入后端写入后不再
检查发送 deadline，已准入的后缀可在 deadline 后完成。取消和生命周期故障仍生效。
调用者按原 deadline 等待可能先超时，任务随后仍成功；等待超时不会取消任务。
无论策略如何，`send()` 都只等到 `SendOptions.deadline`。需要观察最终结果时使用
`submit()` 返回的回执。

`stop_endpoint(name, deadline)` 先关闭该 endpoint 的准入，移除未领取任务，再等待其已领取
批次完成，不取消该批次。返回值是移除的任务数。等待期间释放 Session 状态锁，其他 endpoint
仍可继续执行。超时后准入门保持关闭，已领取任务仍由原 writer 持有；应重试排空，不能创建
替代 writer。`resume_endpoint(name)` 仅在 Session 打开且该 endpoint 不再有已领取批次时
重新开放准入。endpoint 名须稳定标识设备；空名称会匹配未命名任务。

原 `cancel_endpoint(name)` 仍立即返回，可能截断已领取批次尚未发送的后缀，且不会关闭准入。
这两种操作不可混用。`stop_endpoint()` 须从外部线程调用，不能从接收/后端回调调用。
单设备关闭应排空自己的 endpoint 并关闭自己的 inbox，由共享所有者关闭整个 Session。

`send_period` 是可选的成功帧最小发送周期，从原生物理写入开始计时，包含后端耗时和剩余等待。
它跨同一 dispatcher 的 Session 批次生效；非零时不合并为原生批量写入。
它与 `MessageBatch::gap` 分开，后者是额外的帧间等待。它不包含进入原生发送前 Python 侧的
编码、锁和其他前端耗时。最后一帧后的周期等待不能把已知的后端接受结果改为取消或过期。
旧的直接 `send()`/`send_batch()` 入口不具备这套共享周期契约。

`max_consecutive_send_errors` 非零时，连续达到该次数的失败发送会使 Session 进入终止状态；
每个已知成功接受的帧会清零计数，包括随后失败的批次中的成功前缀；原生批次未知的部分送达不能清零。
默认零关闭该策略。终止后需要关闭并重建 Session，不能 `recover()`。
后端写入抛出的超时也计入此阈值，同时保留原有 `expired` 结果和
`current_frame_uncertain=true`。队列、发送锁或节拍等待过期及取消不作为后端写失败计数。

## 路由扩展与绑定层通知

`set_route_key_extractor()` 接受将收到的 `can::Message` 映射为 `RouteKey` 的原生函数，
须在第一次成功注册路由或接受发送前配置。此前未匹配路由的主动 RX 不会冻结配置；恢复后
也不会解除冻结。空函数恢复精确 ID/标准或扩展标志路由。提取器在 Session 状态锁
内执行，必须快速、不阻塞、不重入 Session，不应调用 Python 或执行 I/O。异常或非法返回 key
累计 `route_key_errors`，只丢弃该帧。交付的 `message` 保留原 ID、标志和载荷，
`RoutedMessage::route_key` 记录提取后的 key。

`received_order` 与 `SendResult::claimed_order` 共用 Session 内的事件计数器，只能在同一
Session 内比较，并保留 generation 校验。RX 的 `sequence` 仍是独立的接收序号。
这些值无需依赖时钟精度即可排列软件领取和路由接收事件，但不证明设备已执行命令，
也不能让不带请求序号的协议区分所有晚到回复。接收时间记录原生路由时刻，不是绑定层后来
消费数据的时刻。

Session、inbox 和回执通过 `notification()` 暴露共享的 `ChangeSignal`，其中只有 revision 和
条件变量。RX 入队、溢出、路由关闭、任务领取、回执完成和代次失效可推动 revision；多次变化
可以合并，一个 inbox 也可能因其他 inbox 或回执变化而被唤醒。它不执行应用回调，不增加载荷
队列。外部运行时桥接应先读取 revision，再检查自身状态，没有就绪工作时用该 revision 等待：

```cpp
auto signal = session->notification();
const auto observed = signal->revision();
// 在此检查 inbox、回执快照和生命周期状态。
// 没有就绪工作时，等到变化或桥接器下一次定时任务。
const bool changed = signal->wait_until(observed, next_deadline);
(void)changed; // 等待返回后总是重新检查状态。
```

用 shared pointer 保持 signal 存活。`wait_until()` 自身等待超时返回 false；
`Deadline::max()` 表示无限等待。终态必须在再次等待前处理：Session 关闭后，保留的 signal
不会持续增长。Route health 另记录容量、已交付帧和关闭时丢弃的排队帧数。

迁移后的 SDK 应从绑定层直接提交 Session，不在外面再包 Python `TransmitArbiter`，也不将
Session 任务转入 `ManagedCANBus::queue_send()`，避免增加另一个排队和领取点。
协议 actor、ACK 匹配、晚到回复隔离、目标校验和设备缓存继续由上层 SDK 实现；这些不是第二套
物理发送调度器。原有旧接口继续供其他消费者使用，单队列接入应将它们排除在活动 Session
路径之外。离线回归验证软件契约，不代表适配器、固件、驱动时序或实机验收。

回执仍为 pending 时，`result().sent_frames` 可以随已知的逐帧接受而增加。它不推测原生批次失败时的部分送达数量；该不确定性仍通过终态结果报告。进度变化本身不递增 `ChangeSignal`，需要中间进度的桥接层应使用定时器读取。
