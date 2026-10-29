# PCAN Classic CAN / CAN FD

`PcanTransport` 提供真实通道枚举和运行时加载，核心 SDK 构建不需要厂商开发包。
硬件支持目标为 Windows x64 / MSVC。Linux 测试动态库用于离线 ABI 验证。

## 厂商组件与 ABI

用户自行安装对应架构的 PEAK 驱动、PCAN-Basic DLL 及其依赖。SDK 发行包不带厂商
头文件、DLL、驱动或测试 DLL。自动枚举需要支持 `PCAN_ATTACHED_CHANNELS_COUNT` /
`PCAN_ATTACHED_CHANNELS` 的 PCAN-Basic；FD 还要求三个 FD 入口及确认的 FD 通道。
参考 [PCAN-Basic 官方页面](https://www.peak-system.com/products/software/development-packages/pcan-basic/?L=1)。

ABI 参考为 [官方 PCANBasic.h](https://www.peak-system.com/produktcd/Develop/PC%20interfaces/Windows/PCAN-Basic%20API/Include/PCANBasic.h)。
代码仅声明自有的 Classic/FD ABI 子集；Windows 入口使用 `__stdcall`，不调用 XL API。

公开头有结构布局断言：Classic 消息 16 字节、FD 消息 72 字节、Classic 时间戳 8 字节、
FD 时间戳 8 字节、通道信息 52 字节；消息 data 偏移为 6，通道 features/name/id/condition
偏移为 4/8/44/48。`validation/pcan_header_abi.cpp` 提供与官方头的布局对照检查。

## 加载、枚举与配置

加载优先级：显式 UTF-8 `PcanConfig::library_path` → `LINKERBOT_PCAN_LIB` → 系统
`PCANBasic.dll`。显式或环境路径失败不换另一份 DLL。Windows 使用宽字符 loader；
显式库转为绝对路径，依赖从库目录和安全默认目录搜索，`AddDllDirectory` cookie 跟随
模块生命周期。失败保留原始系统错误码，API 保存实际解析模块路径。

`enumerate_pcan_channels()` 返回厂商实际连接通道，保留原生 handle、硬件名、device ID、
controller、FD 能力、驱动占用条件和本进程占用标记。`PCAN_USBBUS1` 等只是对真实
handle 的格式化，不代表猜测可用通道。能力或可用性查询失败保留诊断，未知 FD 能力拒绝 FD。

```cpp
#include <linkerbot/can/pcan_transport.hpp>
using namespace linkerbot::can;
auto api = load_pcan_api();
auto attached = enumerate_pcan_channels(api);
// 从 attached.channels 选择真实通道；不要假设 USBBUS1 一定存在。
PcanConfig config;
config.channel = pcan_channel_handle("PCAN_USBBUS1");
config.mode = PcanMode::kClassic;
config.nominal_bitrate = 1'000'000;
PcanTransport transport(config, api); // 立即初始化；hand 的 lazy factory 则在 open 时执行
transport.close();
```

Classic 接受 1M、800k、500k、250k、125k、100k、50k、20k、10k、5k bit/s。
FD 使用 `CAN_InitializeFD` / `CAN_ReadFD` / `CAN_WriteFD`，独立于 Classic 速率常量。
`PcanFdTiming{}` 默认工程时序为：80 MHz，nominal BRP/TSEG1/TSEG2/SJW = 1/63/16/16，
data = 1/11/4/4，精确产生 1M/5M bit/s。此时序尚未在指定硬件验收。
更改速率时须同时设置分频、段长和 SJW；配置会校验范围、SJW ≤ TSEG2 及精确速率。

PCAN 按显式帧标志收发：`is_fd=false,is_brs=false` 在 FD 通道上仍是 Classic；
FD 需 `is_fd=true`，BRS 需额外 `is_brs=true`。L30 编码器使用扩展 FD，O30 使用标准 FD、
无 BRS。DLC 0–15 转换为 0–64 字节；发送不能直接表示的长度向上取合法 DLC 并零填充。
Classic 拒绝大于 8 字节、FD/BRS 或非法 ID，不截断。

## 生命周期与错误

同一实际模块、同一原生通道只能由一个 Transport 占用（包括模式/速率/时序冲突）。
能力检查先于初始化，成功取得资源才按本通道 `CAN_Uninitialize(handle)` 清理，
从不调用全局 handle 0 清理。初始化返回 CAUTION 表示取得资源但配置未严格满足，
会拒绝并回滚；失败回滚保留资源与模块供重试。普通初始化失败不误清理其他连接。

`close()` 唤醒接收轮询，串行等待当前 I/O 后清理，重复调用安全；失败后保留占用，
可再次 `close()`。析构/构造回滚失败转为 deferred，后续枚举/打开尝试重试；一个通道
清理失败会保留诊断而不阻止其他通道。也可显式 `retry_pcan_cleanup(api)`。

接收使用非阻塞厂商读取加 1 ms 可中断轮询，默认等待 10 ms，配置允许 0–100 ms。
仅纯 `QRCVEMPTY` 返回空值，组合错误不吞掉；bus-off、adapter unavailable/uninitialized、
TX queue full、bus error 分别由 `PcanTransportError::reason()` 表达，原始位码保留在
`native_status()`。RTR/echo 过滤，status/error 消息报错，不作为业务数据交付。
同步驱动调用没有取消 API，卡住的驱动不能强行中断；Windows 驱动调用时限和负载测试待验收。

## 验证

配套 hand SDK 已记录 Windows Debug / PCAN Classic 的 L6、O6、L20 Lite、L20
发现、读取、单关节命令及恢复通过；实测使用 PCAN-Basic `5.0.0.1115`。
PCAN FD、Release 实机、USB 拔插及长期负载没有本轮通过记录。

构建与测试命令见 [README](../../README.md) 和 [Windows 构建](windows-build.md)。
`pcan-native-loader` 动态加载独立声明原生 ABI 的 `pcan-fixture`，覆盖 Classic/FD、
Classic-only、缺符号库、枚举、帧转换与资源清理。fixture 不安装。
真实 DLL、驱动与适配器的双向收发、USB 断开恢复、模式切换和持续负载需硬件验证。
