# 包名与链接目标

从 0.3.3 起，CMake 工程、安装包和生成的 SDK 归档统一命名为 `linkerbot-transport`。
已发布至 v0.3.2 的包保留原来的 `linkerbot-comm` 名称和内容。

```cmake
find_package(linkerbot-transport 0.3 CONFIG REQUIRED COMPONENTS can)
target_link_libraries(app PRIVATE linkerbot::can)
```

按所需模块选择链接目标：

- `linkerbot::can`：CAN/CAN FD 后端、SLCAN、共享总线、帧路由和 CAN Session。
- `linkerbot::serial`：启用时提供原生串口字节流。
- `linkerbot::modbus`：启用时提供 Modbus RTU 事务及其串口依赖。
- `linkerbot::headers`：头文件路径和 C++17 要求，不链接已编译的库。
- `linkerbot::can-compat`：CAN 链接依赖以及旧的扁平头文件路径。

CAN 不传递链接 serial 或 Modbus，应用须显式链接使用的模块。包组件使用上述小写名称，
缺少必需组件时查找失败。现有 C++ 命名空间及头文件路径不变，包括 CAN 使用的
`linkerbot::transport::Session`。

## 兼容入口

旧的 `find_package(linkerbot-comm 0.3 CONFIG REQUIRED)` 继续可用。
`linkerbot::comm`、`linkerbot::comm-values`、`linkerbot::comm-compat` 分别转发到
`can`、`headers`、`can-compat`，保留原有 CAN 依赖范围。源码子目录构建也提供旧的
非命名空间库别名。别名是链接目标，不是独立的命令行构建目标；构建 CAN 库使用
`cmake --build build --target linkerbot-can`。

CAN 库文件改为 Unix 下的 `liblinkerbot-can.a` 或 Windows 下的 `linkerbot-can.lib`。
同时安装字节完全相同的 `linkerbot-comm` 库副本，兼容按文件名链接的应用。
不要将两个副本同时链接到应用，也不要将它们视为不同实现。新 SDK 文件名以
`linkerbot-transport-` 开头，已发布包的下载地址不变。

文档、示例及 `build-info.json` 安装到 `share/linkerbot-transport`；
`share/linkerbot-comm` 保留相同内容。两种 CMake 包配置目录均安装并支持重定位。

新的缓存选项及导出来源属性使用 `LINKERBOT_TRANSPORT_` 前缀，原 `LINKERBOT_COMM_`
名称继续支持。已有缓存值用于初始化新名称；在已经配置的构建目录中修改任一拼写时，
另一项随之更新。首次配置传入冲突值，或同时将两项改为不同的新值，会配置失败。
`LINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS`、`LINKERBOT_PERF_COUNT_ALLOCATIONS` 保留原名。

元数据仍为 schema version 1，以新增字段扩展。新包的 `package` 为 `linkerbot-transport`，
`components` 包含 `can`，`transport_sha` 记录来源；保留的 `comm_sha` 值与其相同。
`compatibility_libraries: ["comm"]` 声明兼容库副本。校验器检查副本字节一致，并继续验证历史 comm 包。
`LINKERBOT_COMM_SLCAN_FD` 功能宏与新的 `LINKERBOT_TRANSPORT_SLCAN_FD` 均可使用。

## 升级与回滚

迁移时使用独立的构建和安装目录。先构建 SDK，运行新旧消费者检查，再重编依赖应用。
旧包名可同时用于新旧 SDK；采用新包名的应用需要新 SDK，单独回滚 SDK 无法让新的
CMake 查找调用在旧包中生效。回滚时保留配套的应用构建配置。按归档文件名下载的脚本，
须为后续发行采用新前缀。

本次迁移不修改 hand SDK，不发布包或替换发行资产。C++ ABI、CAN、串口和 RTU 行为保持不变；
离线包和消费者验证不能替代实机验收。
