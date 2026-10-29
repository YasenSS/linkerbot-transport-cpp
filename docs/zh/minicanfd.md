# MiniCANFD：平台 ABI、枚举与资源归属

MiniCANFD 通过运行时加载厂商库工作，核心 SDK 构建不依赖厂商开发包。
Linux 使用 `libcanbus.so`，Windows x64 使用 `HCanbus.dll`；库和驱动由用户另行安装。

## 原生 ABI 与统一调用

`VendorCanFdApi` 是可注入的、平台无关的调用表。生产 loader 使用独立的原生函数
指针类型绑定入口，再以 `std::function` 包装；不把 Windows 单索引函数转换为
Linux 双索引函数后调用。已有 Linux 形状的测试函数仍可赋给调用表。

| 操作 | Windows HCanbus.dll | Linux libcanbus.so |
| --- | --- | --- |
| 进程初始化 / 退出 | 不绑定、不调用 `LibCANbus_Init/Exit` | 同时存在时共享引用计数；只存在一个时拒绝；都不存在则由库内部管理 |
| 扫描 | `CAN_ScanDevice()` 返回原生通道数 | 同名函数返回物理设备数 |
| 打开 / 关闭 | `(devNum)` | `(device, channel)` |
| FD 初始化 | `(devNum, config*)` | `(device, channel, config*)` |
| 收发 | `(devNum, frames*, count, timeout_ms)` | `(device, channel, frames*, count, timeout_ms)` |
| 通道类型 | `CAN_GetDevType(devNum)`，兼容样本中的 `CAN_GeDevType` | 有 Init/Exit 时 `CAN_GetCHType(device, channel)`；无 Init/Exit 的设备级 API 可用 `CAN_GetDevType(device)` 核验通道 0 |

Windows 入口声明为 `__stdcall`，支持目标为 x64。
Windows 配置的 `device_index` 就是枚举返回的 devNum，`channel_index` 必须为 0。
配置错误在扫描、打开或其他厂商调用之前拒绝，不能用 `device * 2 + channel` 换算。
Linux 的显式通道编号继续保留，不通过物理设备数猜测通道数。

结构体使用自然 4 字节对齐。公开头文件加入了大小、字段偏移、标准布局和可复制性
断言：`CanFDConfig` 20 字节，`CanFDMsg` 80 字节，DLC 偏移 9、载荷偏移 16。

## 无 CAN 帧枚举

```cpp
#include <linkerbot/can/vendor_canfd_transport.hpp>
#include <iostream>

int main() {
    namespace can = linkerbot::can;
    const auto report = can::enumerate_vendor_canfd_channels(
        std::string(u8"C:\\MiniCANFD 驱动\\HCanbus.dll"));
    for (const auto &failure : report.failures)
        std::cerr << failure.stage << ": " << failure.message << '\n';
    for (const auto &channel : report.channels)
        std::cout << channel.device_index << ':' << channel.channel_index
                  << ' ' << channel.library_path << '\n';
}
```

Windows 默认查询所有扫描所得的原生通道。Linux 必须传已确认的布局，例如：

```cpp
const auto report = linkerbot::can::enumerate_vendor_canfd_channels(
    std::string("/opt/minicanfd/libcanbus.so"), {{0, 0, {}}, {0, 1, {}}});
```

示例中的双通道布局只用于说明调用方式，不能套用为所有设备的规格。未提供 Linux
布局时返回 `layout/channel_layout_required`；缺失类型接口返回 `type/type_unavailable`。
只有类型 1 才进入 FD 通道列表；Classic（0）、LIN（2）、未知值和查询失败逐项保留
诊断。一个通道失败不会丢弃其他已核验通道。结果同时保存原生索引、状态值和解析后
库路径；空列表必须结合 `failures` 判断。枚举不打开通道、不初始化速率、不收发 CAN 帧，
但会调用厂商扫描和类型查询，不能在任意实时控制循环内反复执行。

已有手 SDK 的默认 MiniCANFD Source 复用此枚举实现，保留标准诊断，并在扫描时
固定实际加载的库路径。候选的后续 factory 不会因环境变量变化选择另一份 DLL。

## 打开、共享与关闭

从枚举结果保留 `device_index`、`channel_index` 和 `library_path`，构建
`VendorCanFdTransport::Config`。Transport 构造立即打开通道；需要延迟 I/O 的上层
应传递 factory，由会话/设备的 `open()` 触发构造。

默认仲裁/数据配置为 1 Mbit/s / 5 Mbit/s。每帧独立处理标准/扩展 ID、FD 和 BRS；
未标记帧在 FD Transport 上沿用已有的 FD、无 BRS 语义。O30 使用标准 FD、无 BRS，
L30 使用扩展帧；Transport 不根据手型号强行开启 BRS。
`Message.dlc` 为 0–64 字节，后端转换为厂商 0–15 的 DLC 编码；
不可直接表示的长度向上取整并清零填充。`Config::frame_type` 不覆盖发送标志。

同一进程内，相同库的扫描、打开、关闭与清理共用协调锁。规则如下：

- 首个连接扫描并保存数量快照；其他连接复用该快照，不刷新厂商全局索引。
- 每个原生通道只能由一个 Transport 占有；重复打开在厂商 Open 之前报错。
- 已打开、初始化后待回滚、物理关闭失败或 Exit 失败的资源都保留归属。
- 枚举遇到活连接返回 `busy`；遇到延迟清理失败返回 `cleanup`，不会继续扫描。
- 最后一个连接成功关闭并完成必要的 Exit 后才释放快照；插拔后先关闭全部旧连接再重扫。
- 初始化失败时尝试关闭，只关闭本次已成功打开的通道。关闭失败转存待清理记录，
  由后续 acquire/枚举或 `vendor_canfd_lifecycle::retry_deferred_cleanup(key)` 重试。
- Exit 失败只重试 Exit，不重复物理关闭或扣减引用。每次 loader 调用拥有自己的模块
  引用，在途连接和清理记录都存活期间不会卸载其使用的模块。

扫描编号不是永久物理身份。重连后仍须由 hand 层重新核验手身份；本进程协调不证明
其他进程没有占用同一适配器。

接收 timeout 为 0–10 ms，发送 timeout 为 0–100 ms，默认都为 10 ms。`close()`
先拒绝新 I/O，再等待在途读写结束，最后调用厂商 Close/Exit。测试覆盖并发读写/关闭
和重复关闭。等待有界依赖驱动遵守 timeout；原生 Scan/Open/Init/Close/Exit 没有取消
参数，SDK 不能强制终止卡死在厂商代码中的线程。真实驱动的时限、拔插恢复和负载表现需在对应硬件上验证。

## 库路径与依赖

两平台均按 `Config.library_path` → `LINKERBOT_CANFD_LIB` → 默认搜索选择库。
显式或环境路径失败不回退。Linux 默认依次尝试动态链接器搜索的 `libcanbus.so`、
`/usr/local/lib/libcanbus.so`、当前目录下的 `libcanbus.so`；库加载成功后若 ABI 绑定失败，
立即报错，不再尝试其他候选。

`Config.library_path` 和 `LINKERBOT_CANFD_LIB` 在 Windows 上按 UTF-8 解释。
环境变量通过 Wide API 读取；显式路径先转为绝对路径，再用 `LoadLibraryExW` 加载，
模块路径用可增长的 `GetModuleFileNameW` 缓冲区返回 UTF-8。

优先级为显式参数 → 环境变量 → 默认名称；一旦选择显式或环境路径，加载或绑定失败
立即报错，不回退到另一份同名 DLL。空的显式路径及含 NUL 的路径被拒绝。

显式路径采用 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`，
并在模块引用存活期间保留同目录的 `AddDllDirectory` cookie，支持依赖库从同目录解析。
默认名称使用 `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`，搜索应用目录、System32 和应用
显式添加的目录；不依赖当前目录或 PATH 搜索。依赖目录存在重名模块时仍遵守 Windows
加载器规则，应用应使用匹配的 x64 DLL 套件并保持目录配置稳定。依据
[Microsoft LoadLibraryExW 文档](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)。

加载/符号错误的 `TransportIOError` 保存 Windows 原始错误码到 `system_error()` 和
`native_status()`；消息包含请求路径，成功解析后则使用实际模块路径。缺少依赖通常
报告 126，错误 PE 架构报告 193，缺少导出报告 127；不能只凭 126 推断顶层 DLL 不存在。

## 验证

构建与测试命令见 [README](../../README.md) 和 [Windows 构建](windows-build.md)。
`vendor-native-loader` 使用独立声明 ABI 的动态库验证 loader、枚举、配置传参、
单帧/批量收发、DLC/FD/BRS、通道冲突、失败回滚与并发关闭；测试库不安装。
Windows 额外覆盖缺符号、错误架构、依赖库及中文/空格路径，Linux 覆盖 Init/Exit
不成对与设备级 API。离线 fixture 不能证明任意厂商 DLL 或实物兼容。
