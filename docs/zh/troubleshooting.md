# 错误、诊断与排障

[文档索引](../README.md) · [Session](session.md) · [运行与升级](operations.md)

先区分配置错误、连接/驱动故障、队列压力和设备业务失败。离线回归通过不能排除实物驱动问题；
接收没有消息也不直接说明链路断开。不要根据异常文本的某个单词就归类为认证绕过或安全漏洞。

## 稳定错误接口

`CANError` 继承 `std::runtime_error`，提供 `code()`、`system_error()` 和 `native_status()`。
系统错误或厂商状态并非每次都有值，应同时保存异常类型、错误分类和消息。
`error_code_name(code)` 返回稳定的小写名称，适合日志字段，不必解析 `what()` 文本。

| 分类 / 名称 | 常见触发条件 | 处理边界 |
| --- | --- | --- |
| `ValidationError` / `validation` | ID、长度、FD/BRS、速率、路径、路由冲突或回调内生命周期调用 | 修正配置/调用；反复 recover 不会修复参数 |
| `ClosedError` / `closed` | 会话关闭、旧 generation、已失效 inbox | 关闭后重建；恢复后的旧 inbox 不能继续用 |
| `TimeoutError` / `timeout` | 排队、pacing、回执等待或后端预算超时 | 判断超时发生阶段，不能自动假定未发送 |
| `QueueFullError` / `queue_full` | 有界发送队列达到容量 | 降低提交速率、合并允许覆盖的任务或增加有预算的容量 |
| `TransportIOError` / `transport_io` | 系统调用、动态库、驱动/厂商返回错误 | 保留系统错误、原始状态和后端参数，按后端处理 |
| `FaultedError` / `faulted` | 连续 I/O 故障、通道冲突、关闭失败仍被占用 | 先处理故障及持有者清理，再恢复/重新申请 |
| `InternalError` / `internal` | 未分类的非标准异常等 | 保存最小复现和源码 SHA，不掩盖为成功 |

`transport::CancelledError` 是传输取消异常，不属于 `can::CANError` 分类；在等待回执时
也可能抛出。业务设备的错误码由上层协议解析，不会被本库自动转换为上述类别。
PCAN 还可用 `PcanTransportError::reason()` 区分 bus-off、适配器不可用、队列满、总线错误等。

## 回执与确认丢失

先检查 `SendResult.outcome`、`sent_frames`、`current_frame_uncertain` 和 `error`：

- `pending`：尚未完成，可能已经由 writer 接管。
- `dispatched`：所有帧已被后端接受，仍不证明 CAN ACK 或设备执行。
- `superseded`：同 endpoint/family 的排队 latest 被更新任务替代，在途批次不被替代。
- `cancelled`、`expired`、`failed`：结合已知接受帧数与当前帧不确定性判断实际进展。

`result()` 不重抛保存的异常，适合在等待失败后读取细节；`wait()` / `wait_until()` 会重抛。
等待本身超时不取消任务。如果业务回复丢失，先读取设备实际状态、核验身份及业务序号，
由上层决定是否安全重试；不能只更换请求标识、删除本地记录或盲目重放控制帧。

## Health 字段与观测

`health()` / `get_health()` 读取软件状态，不向硬件发送探测帧。`connected()` 只表示缓存
状态为 open；上层仍需用设备身份/业务回复判断真实可用性。

| 观测对象 | 字段 | 含义 |
| --- | --- | --- |
| `TransportHealth` | state、generation、consecutive_errors、last_error_code/last_error | 生命周期、连接代次、连续接收错误与最近错误 |
| `TransportHealth` | last_successful_tx/rx | 最近软件收发成功的单调时钟时刻；不是 UTC/Unix 时间 |
| `TransportHealth` | subscriber_count、disabled_subscriber_count、last_subscriber_error | 回调数、被禁用回调数及其错误 |
| `SessionHealth` | rx_frames/rx_bytes、tx_frames/tx_bytes | 合法接收与已知后端接受的帧/载荷字节累计数，不是线缆流量 |
| `SessionHealth` | route_misses、invalid_frames、overflows | 无有效路由、非法帧、inbox 拒收新帧的累计数 |
| `SessionHealth` | pending_tx、queue_high_water、recoveries | 当前等待+在途任务数、等待队列峰值、成功恢复次数 |
| `RouteHealth` | pending、high_water、overflow、closed、generation | 单个 inbox 的缓存、峰值、溢出与有效代次 |

Session 的队列容量按等待批次计，不含在途；`pending_tx` 却包含在途，因此两者不能直接
用同一个上限判断超限。ManagedCANBus 的 pending_tx 是其旧发送队列；Session 的
`transport_health()` 投影还会加上 Session 待发量。

Dispatcher 默认连续 10 次计入故障的接收错误后进入 faulted，默认退避基准 5 ms、上限 50 ms；
超时与校验错误不计入该故障阈值。回调连续抛错 3 次会被禁用；必须检查诊断，不能认为广播仍完整。
这些默认值来自 `CANDispatcher::Options`，不是设备心跳阈值。

本库没有内置 HTTP 端点、Prometheus exporter、PromQL 或告警规则。上层若导出指标，需自行
定义命名、label 基数、重启归零和单调时钟转换规则，并保留最近业务回复时间。
不要把示例中的字段名直接当作已经存在的 Prometheus 指标，也不要把 last_error 单独作为实时告警条件。

## 常见故障定位

| 现象 | 首先核对 |
| --- | --- |
| CMake 找不到 Catch2 | Catch2 必须为 3；离线时显式安装或提供 Catch2_DIR，检查下载开关 |
| `find_package` 找不到包 | 包名为 linkerbot-comm；CMAKE_PREFIX_PATH 指向安装前缀而非源码目录 |
| 符号未定义 / coverage 链接错误 | 是否链接 comm 实现、架构/CRT 是否一致；coverage 静态库消费需加 --coverage |
| 动态库加载失败 | 显式路径覆盖环境变量；检查同架构库、依赖库及导出符号，见后端指南 |
| Windows 错误 126/193/127 | 可能分别涉及缺依赖、架构不匹配、缺导出；126 不证明顶层文件不存在 |
| Session 路由冲突 | 精确 `(id, extended)` 已属于另一未关闭 inbox；共享 Session 应协调路由 |
| 恢复后仍 `ClosedError` | 旧 inbox 已失效，需重新订阅；显式 close 后 Session 不能 recover |
| 明明发出了却无回复 | 检查帧类型兼容默认、ID/扩展标志、inbox 溢出、设备身份及业务解析 |
| 关闭/退出耗时异常 | 可能仍在驱动写入、回调或等待批次；保存线程栈，避免在回调内同步关闭 |
| SLCAN 初始化/ACK 超时 | 固件 profile、UART/CAN 速率、回显/时间戳、端口独占；CANable 不等待 ACK |

库路径排查不要运行 `env` 或输出完整进程环境，以免同时泄露密码、令牌或验证码。
需要检查哪份库时，记录成功解析的 `library_path`，并只核对有关的配置字段。

## 可提交的问题证据

按 [贡献指南](../../CONTRIBUTING.md) 提供固定 SHA、平台/架构/编译器、backend、可公开的
适配器与驱动信息、最小命令/程序、异常结构、回执详情、generation 及时间顺序。
涉及并发和退出时，补充线程角色、持有的 Session/lease、关闭是否重试以及业务确认是否丢失。
先复现再修复，补充修复前失败、修复后通过的回归证据；无法复现时明确限制，不扩大影响结论。
