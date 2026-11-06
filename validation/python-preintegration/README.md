# Python 接入前验证

本目录是独立验证程序，不导入或修改 Python transport、hand、arm SDK，也不交付正式绑定或 wheel。
现有 Python 版本继续作为已经验收的业务基准。[English details](README.en.md)

## 本轮证据范围

- **回执所有权**：发送前建立 Future、原生 receipt slot 和强引用登记；原生回执用无异常移动赋值关联。
  实际注入 CPython capsule 分配失败，验证零准入；准入后的 `bad_alloc` 和结果包装 `MemoryError`
  保留原 Future/receipt，重新发布结果不重发。原生分配失败测试另行逐点扫描 device/latest 准入，
  并验证准入后包装分配失败仍可读取既有回执。
- **GIL 与生命周期**：等待、关闭释放 GIL；一个设备的 Event 阻塞 callback 不阻塞另一设备完成。
  验证回调顺序、完成后登记的立即执行、重入、异常、包装析构及解释器退出。每个场景在独立子进程
  运行，由父进程施加超时，并启用 Python debug allocator。
- **共享运行时**：两个独立消费者探针导入同一个扩展；另建 Runtime 不能绕过已存在的设备注册。
  工厂与后端计数检查一个连接、相同 channel-instance 和最多一个后端写入。8 个回执额度限制完成
  元数据直到发布及 callback 结束；保留已完成 Future 不继续占用额度或连接所有权。Python 登记表只保存 Future/receipt，不保存帧，不增加发送调度队列。
- **部署证据门禁**：检查配置、后端、参数、时间戳/帧格式依赖、版本、拓扑和证据哈希。
  缺失项、未知后端/参数、fake 记录、历史版本或被修改的记录不能通过。该工具不执行设备 I/O，
  也不独立证明记录真实或硬件测试确实执行。

探针消费者不是实际 hand/arm SDK。在不接入的前提下，不能把这些结果写成实际 SDK 已共享 Runtime，
也不能声称协议业务已等价。本目录不发送任何运动、抱闸、标定或使能命令。

## 限制

验证入口要求带传统 GIL 的 CPython 3.10 及以上，拒绝子解释器导入和 free-threaded 构建。
分配器注入仅允许一个解释器、一个 Python 线程，并在 capsule 创建后立即恢复，仅用于测试。
跨设备 callback 测试使用有限执行或会释放 GIL 的回调；任意 C 扩展回调若一直持有 GIL，不能保证
其他 Python 线程前进。退出测试验证显式、协作式关闭，不证明可以强制终止无限回调或不可中断驱动。

`CandidateBridge` 是可执行验证夹具，保留分配失败和 callback 重入时的完成元数据所有权；它不是
正式兼容层、异常映射、时钟桥或设备 SDK 实现。实际运行环境以 CI/本地产出的证据记录为准。

## 运行

完整命令见英文说明。先安装原生库，再使用与解释器匹配的开发头文件构建本目录：

```sh
cmake -S validation/python-preintegration -B build/proof-python \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/absolute/native/install \
  -DPython3_EXECUTABLE=/path/to/python
cmake --build build/proof-python --config Release --parallel 4
ctest --test-dir build/proof-python -C Release --output-on-failure
```

`evidence.json` 保存逐场景结果、Python/平台信息、模块/验证源码哈希及可用的 CI revision。
实际分配器注入和特定故障点模拟分别说明，不声称覆盖永久 OOM 或进程终止。

## 实际部署范围

`baseline-inventory.json` 保存本次检查的源码修订号和用户确认的 Python 已验收状态。
检查的源码 SHA 不冒充实际部署 SHA；旧 C++ hand 报告只覆盖其各自 comm 版本和当时环境。

`backend-contracts.json` 是源码/API 清单，合法值仍以原生配置验证器为准。目前明确存在的差异包括：

- 原生 Message 不保存后端硬件时间戳。
- FD 链路上的未标记帧会解析为 FD，不能直接等价于 Python 显式 classic-on-FD。
- Python 的 `receive_own_messages` 没有对应的原生公开配置项。
- 任意 python-can 插件和额外参数不自动获得支持。
- vendor `frame_type` 是保留但不应用的旧字段，不能记为已生效的发送配置。

按已经验收的 Python 部署填写 `deployment.template.json`，不猜测缺失值。记录精确修订号、平台、
通道、适配器/驱动、型号/序列号/固件、wire identity、展开后的 Python/原生/外部参数和元数据依赖。`python_options` 的每个字段均需
`option_mapping`，直接映射必须保持 JSON 类型及值；未映射参数或未验证的转换不能通过。原生实机记录
需绑定完全一致的版本、配置摘要和身份/收发/共享生命周期/恢复/关闭结果。格式见英文说明。

```sh
python validation/python-preintegration/check_deployment.py deployment.json \
  --report deployment-verdict.json
```

提供的模板故意不能通过；单元测试中的合成记录只测试门禁。没有对应当前原生版本和配置的实机
证据时，第 4 项保持待验证，不把 Python 已验收、历史报告或 fake 通过转换成原生实机通过。
