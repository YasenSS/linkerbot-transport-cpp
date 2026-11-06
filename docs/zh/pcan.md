# PCAN Classic CAN / CAN FD

[文档索引](../README.md) · [English](../en/pcan.md) · [诊断](troubleshooting.md) · [运行与升级](operations.md) · [验证边界](validation.md)

`PcanTransport` 提供真实通道枚举和运行时加载，核心 SDK 构建不需要厂商开发包。
硬件支持目标为 Windows x64 / MSVC。Linux 测试动态库用于离线 ABI 验证。

## 厂商组件与 ABI

用户自行安装对应架构的 PEAK 驱动、PCAN-Basic DLL 及其依赖。SDK 发行包不带厂商
头文件、DLL、驱动或测试 DLL。自动枚举需要支持 `PCAN_ATTACHED_CHANNELS_COUNT` /
`PCAN_ATTACHED_CHANNELS` 的 PCAN-Basic；FD 还要求三个 FD 入口及确认的 FD 通道。
参考 [PCAN-Basic 官方页面](https://www.peak-system.com/products/software/development-packages/pcan-basic/?L=1)。

ABI 参考为 [官方 PCANBasic.h](https://www.peak-system.com/produktcd/Develop/PC%20interfaces/Windows/PCAN-Basic%20API/Include/PCANBasic.h)。
代码仅声明自有的 Classic/FD ABI 子集；Windows 入口使用 `__stdcall`，不调用 XL API。

该官方头链接是可变地址，不是版本固定的归档。2026-11-03 获取的 PCAN-Basic
5.1.0.1194 头文件标注 `Last change: 2025-10-24`，大小 40,098 字节，SHA256 为
`1ff0c5723b7733e145bebdfca12da0ed7bd174ab46a845be2bf3c647087f14d2`。更新厂商组件时须
重新获取并记录摘要，不能把本次值沿用为未来版本证明；厂商头不进入本仓库或发行包。

公开头有结构布局断言：Classic 消息 16 字节、FD 消息 72 字节、Classic 时间戳 8 字节、
FD 时间戳 8 字节、通道信息 52 字节；消息 data 偏移为 6，通道 features/name/id/condition
偏移为 4/8/44/48。`validation/pcan_header_abi.cpp` 提供与官方头的布局对照检查。

## 加载、枚举与配置

加载优先级：显式 UTF-8 `PcanConfig::library_path` → `LINKERBOT_PCAN_LIB` → 系统
`PCANBasic.dll`。显式或环境路径失败不换另一份 DLL。Windows 使用宽字符 loader；
显式库转为绝对路径，依赖从库目录和 `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` 指定的目录搜索，`AddDllDirectory` cookie 跟随
模块生命周期。失败保留原始系统错误码，API 保存实际解析模块路径。

`enumerate_pcan_channels()` 返回厂商实际连接通道，保留原生 handle、硬件名、device ID、
controller、FD 能力、驱动占用条件和本进程占用标记。`PCAN_USBBUS1` 等只是对真实
handle 的格式化，不代表猜测可用通道。能力或可用性查询失败保留诊断，未知 FD 能力拒绝 FD。

```cpp
#include <linkerbot/can/pcan_transport.hpp>
#include <algorithm>

int main() {
    namespace can = linkerbot::can;
    auto api = can::load_pcan_api();
    const auto attached = can::enumerate_pcan_channels(api);
    const auto selected = std::find_if(attached.channels.begin(), attached.channels.end(),
        [](const can::PcanChannel &channel) {
            return (channel.condition & can::pcan_abi::available) && !channel.owned_by_process;
        });
    if (selected == attached.channels.end()) return 1;
    can::PcanConfig config;
    config.channel = selected->handle;
    config.mode = can::PcanMode::kClassic;
    config.nominal_bitrate = 1'000'000; // 必须与目标总线一致。
    can::PcanTransport transport(config, api);
    transport.close();
}
```

此例仅演示选择可用通道，不核验所连设备身份；实际应用还应按已确认的适配器和通道选择，
不能把列表第一个可用通道视为特定设备。不要用 `PCAN_USBBUS1` 的名称猜测它已经连接。

Classic 接受 1M、800k、500k、250k、125k、100k、50k、20k、10k、5k bit/s。
FD 使用 `CAN_InitializeFD` / `CAN_ReadFD` / `CAN_WriteFD`，独立于 Classic 速率常量。
`PcanFdTiming{}` 默认工程时序为：80 MHz，nominal BRP/TSEG1/TSEG2/SJW = 1/63/16/16，
data = 1/11/4/4，精确产生 1M/5M bit/s。此时序尚未在指定硬件验收。
更改速率时须同时设置分频、段长和 SJW；配置会校验范围、SJW ≤ TSEG2 及精确速率。

PCAN 按显式帧标志收发：`is_fd=false,is_brs=false` 在 FD 通道上仍是 Classic；
上述解释适用于 `format` 未指定时；`FrameFormat::kClassic` / `kFd` 可显式选择帧类型，
BRS 仍要求同时设置 `is_fd=true` 和 `is_brs=true`。设备协议决定标准/扩展 ID 和 BRS，
本库不提供手型号编码器。DLC 0–15 转换为 0–64 字节；发送不能直接表示的长度向上取合法 DLC 并零填充。
Classic 拒绝大于 8 字节、FD/BRS 或非法 ID，不截断。

## 生命周期与错误

同一实际模块、同一原生通道只能由一个 Transport 占用（包括模式/速率/时序冲突）。
能力检查先于初始化，成功取得资源才按本通道 `CAN_Uninitialize(handle)` 清理，
从不调用全局 handle 0 清理。初始化返回 CAUTION 表示取得资源但配置未严格满足，
会拒绝并调用 `CAN_Uninitialize(handle)` 回滚。回滚成功时立即删除本进程 claim；只有回滚
自身失败时才保留 claim、模块引用和 deferred cleanup 所有权。其他普通初始化错误不会标记为
initialized，也不会误清理其他连接。

`close()` 唤醒接收轮询，串行等待当前 I/O 后清理，重复调用安全；失败后保留占用，
可再次 `close()`。析构清理失败，或构造期间已经取得通道但回滚失败时转为 deferred；
后续枚举/打开尝试重试。一个通道清理失败会保留诊断而不阻止其他通道。也可显式
`retry_pcan_cleanup(api)`。

接收使用非阻塞厂商读取加 1 ms 可中断轮询，默认等待 10 ms，配置允许 0–100 ms。
仅纯 `QRCVEMPTY` 按接收队列为空处理，组合错误不吞掉；过滤非数据帧直到截止时间或完成最多 256 次厂商读取尝试后，
`read()` 也会返回空值，因此空值不表示已经等待完整接收时限。bus-off、adapter unavailable/uninitialized、
TX queue full、bus error 分别由 `PcanTransportError::reason()` 表达，原始位码保留在
`native_status()`。RTR 帧被过滤；echo 默认过滤，显式开启
`PcanConfig::receive_own_messages=true` 后由原生 setter 配置，并以 `Message::is_rx=false`
交付。缺少 setter 时在初始化前拒绝；驱动拒绝时执行通道清理/重试协议。status/error 消息仍报错。
驱动时间戳保留微秒值及驱动时钟标识，不作纪元转换，见 [CAN 元数据说明](can-metadata.md)。
同步驱动调用没有取消 API，卡住的驱动不能强行中断；Windows 驱动调用时限和负载测试待验收。

## 验证

当前通信库的 Windows Debug/Release CI 与 Linux fixture 验证可在
[验证记录](validation.md) 核对。原导入文档对 hand SDK / PCAN Classic 的实机陈述作为
历史转述集中保留于该文档，不能代表当前源码、PCAN FD 或新驱动已经验收。

构建与测试命令见 [README](../../README.md) 和 [Windows 构建](windows-build.md)。
`pcan-native-loader` 动态加载独立声明原生 ABI 的 `pcan-fixture`，覆盖 Classic/FD、
Classic-only、缺符号库、枚举、帧转换与资源清理。fixture 不安装。
真实 DLL、驱动与适配器的双向收发、USB 断开恢复、模式切换和持续负载需硬件验证。

取得单独的官方头后，可做可选的 compile-only 对照；下面的输出文件在临时目录，不会复制
厂商头。ARM64/Linux 分支只用固定宽度别名检查布局和函数类型，不验证 Windows `__stdcall`：

```bash
clang++-18 -std=c++17 -Iinclude \
  -DPCAN_REFERENCE_HEADER='"/absolute/path/PCANBasic.h"' \
  -c validation/pcan_header_abi.cpp -o /tmp/pcan_header_abi.o
```

显式库路径覆盖 `LINKERBOT_PCAN_LIB`；空环境变量按未设置处理，空显式路径拒绝。
Linux loader 默认尝试 `libpcanbasic.so`，这一实现细节不改变 Windows x64 的硬件支持目标。
接收等待默认 10 ms，可配置 0..100 ms；通道默认值为 0，会在打开前被拒绝，必须显式选择。

```powershell
$env:LINKERBOT_PCAN_LIB = 'C:\PEAK\PCANBasic.dll'
```

环境变量只影响后续加载，不能替换现有连接使用的 DLL；具体依赖和资源重试见上文。
