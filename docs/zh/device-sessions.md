# 逻辑设备会话

`Session::attach_device(identity, DeviceOptions)` 在既有 Session writer 上注册设备。必须在旧式发送和路由注册前接入；进入设备模式后，原始 Session 发送被拒绝。现有 C++ 构造入口、默认调度、回执等待和截止时间策略保持兼容。

设备句柄为提交绑定独立 endpoint 和物理 generation，调用方不能覆盖。关闭后重新注册同名设备会获得新 endpoint，旧句柄始终关闭。物理恢复使旧句柄失效；`channel_instance` 区分底层 generation 相同但已经重新创建的连接。

## 共享连接与恢复

`<linkerbot/transport/runtime.hpp>` 提供 `TransportRuntime`。同一链接库内的 Runtime 实例共享通道注册表和 `SharedCANBusPool`，每个通道持有一个物理租约和一个 Session，设备持有逻辑租约。

`open_device(ChannelSpec, DeviceSpec, factory)` 要求明确提供配置 fingerprint，覆盖 `CANBusKey` 之外的后端参数。共享连接必须使用一致的 fingerprint 和 Session 调度配置。池同时检查物理模式冲突。factory 在注册表锁外执行；并发首次打开合并初始化，循环等待被拒绝。

关闭一个设备保留其他设备通信；最后一个设备关闭 Session 并释放物理租约。显式关闭失败保留注册和连接所有者，允许重试。析构只尽力清理，底层失败资源沿用物理租约的保留机制。应用应显式关闭并处理异常。

`recover_channel(key, observed_generation)` 协调物理恢复：并发请求复用在途尝试，成功结果按旧 generation 复用，失败后允许再次尝试。旧句柄失效，未完成工作不重放。`ChannelResetPolicy::kRequireDetach` 阻止活跃设备期间的通道重置，适用于需要重建 SDK 状态的设备。

`rebind_device(key, previous, replacement, deadline)` 先保留连接租约，再关闭旧注册并接入替代设备，不重开后端。SDK 仍负责身份核对、缓存失效和初始化。它不替代 Python hand 既有的物理 recover 行为。

## 同一队列内的设备政策

`AdmissionDomain` 是不可变额度身份，不是额外队列。共享预算的设备使用同一个对象，并通过 `DeviceOptions::admission_domain` 绑定。FIFO 可按批次或帧计费，latest 按等待槽计费，claimed 工作不占等待额度。domain 与 Session 限制同时生效。latest 替换准入失败不影响旧任务。跨物理 Session 使用同一 domain 不合并额度。

`domain_round_robin` 为混合通道提供显式跨 domain 轮转，再执行域内排序。设备 FIFO 保持顺序。`max_batch_frames` 限制单个原子批次，零保留旧式不限制行为。部署必须明确给出总容量、批次上限和拓扑。

`SendOptions::pending_age_scope` 默认值为新增的 `PendingAgeScope::kDefault`：通过 DeviceSession 提交时解析为本设备 endpoint，通过旧 Session 入口提交时解析为整个 Session。显式 `kSession`、`kEndpoint` 和 `kAdmissionDomain` 按指定范围检查，domain 作用域要求提供额度域。检查包含相应范围的 queued、writing 和最终 cooldown。既有枚举数值与默认准入行为不变；字段默认枚举值改为 `kDefault`。设备调用方显式设置的 `kSession` 现在检查整个 Session，不再被改写为 endpoint。

设备 `send_period` 从每帧 dispatch attempt 入口计时，禁用原生批次合并，批次内部保持原子边界。最终帧 cooldown 释放 writer，使其他设备可发送；同一个 Job 仍由原生层保留，排空和队龄统计继续计入。设备周期不能与旧 Session 周期同时配置。

`max_consecutive_send_errors` 只使相应设备 fault。成功帧清除本设备错误连续计数，邻居发送成功不清除它。达到阈值时终结其等待任务并以 fault 原因关闭 inbox，不直接终止物理 Session。

已核对的 Python 源码基准为：hand 共享 FIFO 256 个请求批次、latest 64 槽、aging 50 ms；arm 每臂 FIFO 2000 帧、成功帧周期 300 μs、错误阈值 10。这些不是新的 C++ 默认值。部署需提供展开配置及来源，才能标记为原始业务配置。

## 时间、进度与关闭

新增 `DeadlinePolicy::kDispatchStart` 先记录 claim，再在 dispatch 入口检查一次截止时间。过期时零帧发送并保留 claim 元数据；开始后不再用该期限截断物理锁等待或发送后缀。取消和物理故障仍生效。旧 `kExecution`、`kStartOnly` 边界不变。调用方等待期限独立，等待超时不取消任务。

`sent_frames` 为已知后端接受前缀，`dispatch_completed_frames` 为完成节拍的前缀。最终 cooldown 中可能已经全部物理发送，但 dispatch 尚未完成。关闭唤醒最终等待时保留已知 dispatched 结果。

SDK 在关闭前用 `begin_operation()` 登记已有工作。可移动的 `DeviceOperation` 绑定创建线程，允许所属工作在公开准入关闭后发送恢复命令，不创建 worker 或载荷队列。其他线程不能借用它发送；持有操作的线程不能同步等待自身关闭。

`close_until(deadline)` 依次关闭公开准入、等待已登记操作、取消等待任务、排空 claimed 批次、关闭设备 inbox、释放逻辑租约。超时保留注册以便重试。底层关闭调用可能超过等待期限。transport 不自动发送运动、失能、标定或抱闸命令；不同型号的收尾规则仍由 SDK 执行。

## 路由、发现与诊断

`subscribe_routes()` 注册精确 ID 和标准/扩展 ID 标志，`subscribe_rules()` 注册掩码范围，支持 A7 Lite 扩展 ID。始终交付原始帧，逻辑 key 单独记录。按实际 wire 范围拒绝精确与掩码的重叠，整组注册在分配失败后回滚。掩码规则不能与旧全局 extractor 混用。

`observe_rules(rules, capacity)` 提供显式、有界、只读的发现观察。它不占用普通路由所有权或普通接收总容量，原始 owner 优先收到自己的帧；观察流满时仅丢弃其观察副本。发现请求发送仍使用设备的同一 writer。观察 inbox 的容量与丢弃情况通过自身 health 查看。

`max_route_keys` 限制注册规则总数，包含观察规则；`max_pending_rx` 限制普通路由总积压。零保留旧无限制行为。溢出丢弃新帧。`compatibility_sequence` 在匹配后、offer 前递增，因此溢出保留间隙；物理失效后重置。原 C++ sequence 不改变。相同报文的普通交付和观察副本共用原生事件顺序。

通道 health 包含路由数、普通接收积压与高水位、已路由帧、过期帧、额度拒绝和关闭丢弃。设备 health 包含准入、generation、instance、已登记操作、错误连续计数、等待 FIFO 帧/latest 槽、in-flight Job 和最老工作。设备故障原因由 inbox 保留，旧式 inbox 的关闭异常约定不变。

本次不交付 Python 绑定、Future 执行、协议事务、hand/arm SDK 接入或电机安全政策。原生接收时间戳和后端回显选项见 [CAN 元数据说明](can-metadata.md)；任意 Python 插件后端不自动获得原生支持。软件 fake 测试不代表实机、固件与驱动已验收；部署继续遵循迁移方案的型号、驱动和拓扑验收要求。
