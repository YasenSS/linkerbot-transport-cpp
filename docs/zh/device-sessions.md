# 逻辑设备会话

`Session::attach_device(identity)` 在现有 Session writer 上创建设备句柄。须在旧发送或路由注册前创建首个设备。进入设备模式后，直接调用 Session::submit 会被拒绝，应使用设备句柄。父 Session 仍是显式的物理连接所有者。

设备发送会覆盖 endpoint 和 expected_generation，调用方不能通过 SendOptions 改写。设备拥有自己的 inbox。close_until 拒绝新提交、移除未领取任务并排空已领取批次，不截断该批次；超时后保留注册，可以重试关闭，其他设备保持运行。成功关闭后可重新注册同一 identity，新注册使用不同 endpoint，旧句柄保持关闭。物理恢复使旧设备句柄失效，应关闭后重新注册。

AdmissionDomain 是不可变的计费身份，不增加队列。需要共享额度的设备使用同一个 domain 对象，并在 SendOptions::admission_domain 指定。FIFO 可按批次或帧计费，latest 按等待槽位计费；claimed 工作不占等待额度。Session 总额度与 domain 额度同时生效；准入失败不替换旧任务。同一 domain 在不同 Session 上分别计费。

PendingAgeScope::kEndpoint 只检查自身 endpoint 的排队及 claimed 工作；默认 kSession 保留原全会话行为。设备句柄选择 endpoint 局部检查。

本次仅实现所有权与准入基础。完整设计中的通道配置管理、设备独立节拍与错误政策、内部收尾 permit、可组合 mask 路由、完整诊断及 Python 绑定仍需后续实现。SDK 的运动参数恢复须先完成，再关闭设备句柄。设备关闭不发送失能、抱闸、校准或运动命令，也不关闭或恢复父 Session。
