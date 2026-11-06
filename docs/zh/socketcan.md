# SocketCAN：Linux Classic CAN / CAN FD

[文档索引](../README.md) · [English](../en/socketcan.md) · [Session](session.md) · [运行与升级](operations.md) · [验证边界](validation.md)

`SocketCANTransport(interface_name, fd_mode=false)` 使用 Linux CAN 网络接口。
构造时创建 raw CAN socket、选择接口并绑定；它不配置接口速率、不启动接口，也不安装驱动。
下面的命令会更改接口状态，执行前先停止占用该通道的控制程序，并核对适配器参数。

## 配置已有接口

先确认系统已识别真实接口，用 `ip -details -statistics link show can0` 检查名称和状态。
以下速率只是示例，必须与总线上其他节点和硬件能力一致：

```bash
# Classic：1 Mbit/s
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 1000000
sudo ip link set can0 up
ip -details -statistics link show can0
```

支持 FD 的适配器、内核驱动和总线节点才能使用 FD 配置：

```bash
# FD：仲裁 1 Mbit/s，数据 5 Mbit/s
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 1000000 dbitrate 5000000 fd on
sudo ip link set can0 up
ip -details -statistics link show can0
```

接口配置通常需要网络管理权限。应用只做配置后的收发，应按所在系统的 socket/设备权限运行；
不要为了运行整个控制程序就默认授予所有特权。容器还需能看到对应网络命名空间中的 CAN 接口，
挂载 `/dev/ttyACM0` 不能替代 SocketCAN 网络接口配置。串口 SLCAN 是另一条独立接入路径。

参数、内核支持与 vcan 说明可查 [Linux SocketCAN 官方文档](https://docs.kernel.org/networking/can.html)。
SDK 离线测试不需要配置 can0 或 vcan，勿在自动测试中重配已有真实通道。
接口名称、速率和 up/down 状态属于操作系统配置，不写入本库。升级或回滚通信库不会恢复
此前的 `ip link` 设置；部署流程应保存并重新核对系统侧配置。

## 接入与恢复 factory

这是 Linux 专用的构造片段；创建后先订阅协议所需的回复 ID，再提交实际设备命令：

```cpp
#include <linkerbot/can/socketcan_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <memory>

int main() {
    auto session = linkerbot::transport::Session::create([] {
        return std::make_unique<linkerbot::can::SocketCANTransport>("can0", true);
    });
    auto inbox = session->subscribe_routes({{0x28, false}});
    // 0x28 仅为路由示例；实际协议、身份核验与设备命令由调用者决定。
    session->close();
}
```

使用同一接口上的多设备时，经 Pool/Session 共享连接；不要让多个上层发送器绕过批次调度。
恢复只重新创建 socket，不会重新执行 `ip link`、重新配置速率或自动重放控制帧。
接口已移除或变更名称时，需先修复系统配置，再按 [恢复流程](session.md#恢复关闭与诊断) 重建。

## 帧、等待和错误

Classic 模式接收 `can_frame`；FD 模式同时接收 Classic 和 FD 帧，保留扩展 ID、FD 和 BRS 标志。
CAN 错误帧与 RTR 不作为应用帧交付，过滤一帧后立即返回空值。FD 接收长度超过 64、读取异常尺寸或系统 I/O 错误会报错。
`Message.dlc` 是字节数；扩展 ID 需要显式 `is_extended=true`。

FD 模式发送未标记帧会沿用 FD、无 BRS 的兼容行为，不能靠 `is_fd=false` 在这个后端的
FD 模式强制发 Classic。显式设置 `Message::format = FrameFormat::kClassic` 可以发送经典帧，
要求 FD/BRS 标志为 false 且载荷不超过 8 字节。`kUnspecified` 保留原有 FD 默认行为；
Classic 后端拒绝 FD/BRS 或超过 8 字节的载荷。

配置构造入口为 `SocketCANTransport(options, interface_name)`。`SocketCANOptions` 中
`receive_own_messages` 默认 false，`timestamp_mode` 默认 `kDisabled`；原生回显及软件/原始硬件
时间戳须显式开启。`read()` 通过 `recvmsg()` 保留可用元数据，时钟、方向标志和实机验证边界
见 [CAN 元数据说明](can-metadata.md)。

socket 接收超时设为 10 ms，发送超时设为 50 ms；接收超时返回空值，但空值不表示已经等待完整接收时限。系统 `write()` 以负值
失败时抛出的 `TransportIOError` 保留 `system_error()`；异常短写也抛 `TransportIOError`，但
可能没有系统错误码，非法帧则在系统调用前抛 `ValidationError`。这些 socket 选项和 Session
deadline 不是设备执行时限，也不能保证任意驱动在硬性时限内返回。接收超时只表示等待窗口内
没有应用帧；发送超时或 `write()` 失败也不能证明帧一定未送达，应结合错误、回执和设备状态核对。

`close()` 拒绝新 I/O，等待在途调用退出后只尝试一次真实 fd 关闭。Linux close 即使失败也
不能用旧 fd 数值再关闭，以免误伤已复用描述符。`AdoptFd` 是离线 socketpair 的测试入口，
不是向应用推荐的设备配置方式。

## 常见问题

- `Failed to get interface index`：接口名、网络命名空间或驱动创建的接口不一致。
- `Failed to enable CAN FD frames`：检查内核与适配器是否支持 FD。
- 可以打开但收不到业务回复：核对接口 up、速率、布线/终端、节点身份、ID/扩展标志和路由。
- bus-off 或 TX 队列满：先保存 `ip -details -statistics link show can0`、系统错误及业务回执，
  再按设备和驱动的恢复要求处理；不要用无条件循环重放控制命令掩盖问题。
