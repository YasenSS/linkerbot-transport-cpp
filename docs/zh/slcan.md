# SLCAN：Windows / Linux 原生串口

[文档索引](../README.md) · [English](../en/slcan.md) · [诊断](troubleshooting.md) · [运行与升级](operations.md) · [验证边界](validation.md)

SDK 提供两个显式固件配置：Lawicel Classic 和 CANable 2.0 FD 扩展。
原生串口实现使用系统驱动，不依赖额外厂商 DLL。
固件协议需要由用户确认，端口名、CANable 产品名不能证明协议兼容。

## 固件配置

| 项目 | `kLawicelClassic`（默认） | `kCanableFd` |
| --- | --- | --- |
| 初始化 | `C\r`、`S0..S8\r`、`O\r`；逐条等待 CR | `C\r`、`S0..S9\r`、FD 模式下 `Y2\r` 或 `Y5\r`、`O\r`；无串口 ACK |
| CAN 仲裁速率 | 10k、20k、50k、100k、125k、250k、500k、800k、1M | 10k、20k、50k、100k、125k、250k、500k、750k、1M、83.3k（依次 S0..S9） |
| FD 数据速率 | 不支持 | 2M / 5M，对应 Y2 / Y5 |
| Classic 帧 | `t` 标准 ID / `T` 扩展 ID，0..8 字节 | 同左，FD 连接也可发送 Classic 帧 |
| FD 帧 | 不支持，打开前拒绝 FD/BRS | `d` / `D` 无 BRS，`b` / `B` 有 BRS，0..64 字节 |
| 发送完成 | CR 或 `z`/`Z` + CR 表示适配器接收 | 仅表示全部字节被本机串口驱动接收 |

两种配置都不以串口发送完成表示 CAN 对端 ACK 或手部业务执行成功。
CANable 固件不返回 ACK/NACK，无法据此验证初始化、CAN 速率或设备连通；必须检查手的身份和业务回复。
选择 CANable 配置但设 `fd=false` 时，只发送 Classic 帧，不配置 Y 命令。

FD 使用 CAN DLC 映射 `0..8,12,16,20,24,32,48,64`。`Message.dlc` 是字节数；
发送 9、10、11 等长度时向上补零到合法线长，接收保留线上完整长度。
每条帧的 `Message.is_extended/is_fd/is_brs` 决定帧格式；`config.fd=true`
允许 FD，不强制将 Classic 帧转换成 FD，也不强制 BRS。`config.brs` 是配置校验项，
不能替代消息的 `is_brs`。标准/扩展 ID 和 BRS 必须由上层设备协议决定。

协议依据：[CANable 固件说明](https://github.com/normaldotcom/canable2-fw/blob/b158aa7ab106bc018715cd4d5f02f544f9de1830/README.md)、
[固件解析代码](https://github.com/normaldotcom/canable2-fw/blob/b158aa7ab106bc018715cd4d5f02f544f9de1830/src/slcan.c)、
[python-can 4.6.1 SLCAN](https://python-can.readthedocs.io/en/v4.6.1/_modules/can/interfaces/slcan.html)。
这是指定扩展协议的实现，不承诺所有厂商的 SLCAN FD 固件互通。

## 连接示例

下面是程序片段：头文件放在源文件顶部，其余语句放在应用函数内。

```cpp
#include <linkerbot/can/slcan_transport.hpp>
#include <chrono>
#include <iostream>
linkerbot::can::SlcanConfig config;
config.port = "/dev/ttyACM0";           // Windows 改为 COM10；必须为已确认的适配器
config.serial_baud = 115200;           // UART，与 CAN 速率独立，按固件设置
config.nominal_bitrate = 1'000'000;
config.protocol = linkerbot::can::SlcanProtocol::kCanableFd;
config.fd = true;
config.data_bitrate = 5'000'000;        // 仅 2M / 5M
config.startup_delay = std::chrono::milliseconds{0}; // 固件开口复位时按需设置，最多 5 秒
linkerbot::can::SlcanTransport transport(config);    // 独占打开、配置、初始化
const auto frame = transport.read();   // 没有完整应用帧时可能返回 nullopt
if (frame) std::cout << frame->id << '\n';
transport.close();
```

默认参数为 Lawicel Classic、115200 UART、1 Mbit/s CAN、receive_timeout 10 ms、
command_timeout 500 ms、startup_delay 0 ms，`fd=false`、`brs=false`；data_bitrate 默认
5 Mbit/s 仅在 FD 时使用。Classic Lawicel 保留默认 protocol、`fd=false`，不设置 FD 参数。
固件须预先关闭回显和时间戳；SDK 不发送猜测性探测或刷写命令。
Lawicel 的首条 C 须能在已关闭状态返回成功 CR。
本库只在当前连接发送 `C/S/Y/O`，自身不持久化这些命令；适配器是否跨复位保留状态由具体固件
决定。本库也不保存跨进程回执或固件状态，设备或服务重启后应重新核对 profile、速率和目标身份。

`slcan_port_name` / `validate_slcan_config` 不做 I/O。`enumerate_slcan_ports` 仅查询
显式允许列表的元数据，不打开端口；空列表返回空，不扫描全机串口。
Windows 接受 COM1..COM65535（包括 COM10+）；Linux 接受绝对 `/dev/...` 路径，
支持 `/dev/serial/by-id/...`，拒绝含 `..` 的路径。
元数据存在不证明端口空闲、固件协议或 FD 能力。

Linux 直接打开串口，不需要 `slcand`，也不会创建 `can0`。不要同时让 slcand、
ModemManager 或其他 SDK 占用同一串口。使用发行版的串口设备组/udev 规则授予当前
用户读写权限；Ubuntu 通常是 dialout 组，加入组后需重新登录。USB CDC 固件可能
忽略 UART 波特率，仍应按固件要求配置。

## I/O、超时和关闭

UART 范围 1200..3000000；实际驱动支持在打开时核实。接收等待 1..100 ms，
串口写入 / Lawicel ACK 等待预算 1..5000 ms，startup_delay 为 0..5000 ms。
初始化每条命令分别计时。Lawicel 的收发共用解析器，等待 ACK 时收到的帧进入接收队列；
CANable 的接收和发送独立，不等待不存在的串口 ACK。

`read()` 在帧队列为空时执行一次串口输入处理。部分输入或协议确认可能尚未形成完整应用帧，
因此可能在配置的接收超时之前返回空值；调用者可再次调用 `read()` 继续解析输入。

解析器支持拆包/粘包，Classic 未完成行上限 26 字符，FD 上限 138 字符，
最多缓存 256 帧，单次串口读取最多 256 字节。非法 ASCII、BEL、超长行、队列溢出、
发送失败和 ACK 超时进入故障态，必须关闭重建，避免使用迟到 ACK。
不支持 RTR、时间戳后缀和命令回显。`SlcanConfig::receive_own_messages` 默认 false；
true 会在串口初始化前被拒绝，不生成接收时间戳或方向替代值。`Message::format` 可在已配置
固件及链路能力范围内显式选择经典/FD，见 [CAN 元数据说明](can-metadata.md)。

Windows 使用独占 `CreateFileW`、8N1、无软硬件流控、DTR/RTS 禁用，
采用 overlapped I/O / CancelIoEx。取消后等待 OS 完成请求才释放缓冲区；
异常驱动可能超出预算。释放失败仍保留资源归属，支持 close 重试及
`pending_slcan_cleanup()` / `retry_slcan_cleanup()`。

Linux 使用非阻塞文件描述符、termios2 原始 8N1 / BOTHER 波特率、flock / TIOCEXCL
独占、poll 超时和 eventfd 取消；处理部分写入、EINTR、USB 拔除和本地资源释放。
关闭先取消读写，再尽力发送 C，不等待其 ACK，始终尝试本地释放。Linux close
即使报错也不重复关闭数值相同、可能已被其他线程复用的 fd。
串口打开/配置/关闭仍受系统驱动约束，无法承诺任意驱动下的硬性耗时上界。

## 验证和吞吐

`can-tests "[slcan]"` 包含字节流测试桩与 Linux 伪终端测试：两个配置的启动、
ACK 差异、全部 FD 长度、标准/扩展 ID、BRS、拆粘包、无效输入、缓存边界、取消、
部分写入、断连、独占和重开。Linux 仅访问用例自行创建的 PTY，不访问已有串口。
构建与执行方式见 [README](../../README.md) 和 [Windows 构建](windows-build.md)。

115200 baud / 8N1 理想单方向吞吐为 11520 字节/秒：8 字节扩展 Classic ASCII
含 CR 为 27 字节，64 字节扩展 FD 含 CR 为 139 字节，理想上限分别约 426 / 82 帧每秒。
实际还受 ACK、USB、固件和双向业务影响，不能套用 PCAN / MiniCANFD 性能阈值。
真实验收应记录固件、驱动、适配器、UART/CAN 速率及负载和延迟结果。
原导入文档对 Windows Debug / CANable 六型号的陈述作为历史转述保留于
[验证记录](validation.md#历史实机陈述保留出处不作本仓库验收)。本库没有随附的实机报告，
不能从端口名、USB 产品名或外壳型号推断为已验证组合。Lawicel、Linux 原生串口实物、
其他固件/驱动、Release 实机和长期吞吐仍需独立验收。SLCAN 继续标为实验性。
