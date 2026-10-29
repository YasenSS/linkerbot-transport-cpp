# Windows 构建与独立通信包

使用 Visual Studio 2022、v143 14.44、CMake 3.31.8 和 C++17，目标架构为 x64。
CI 已配置 windows-2022 的 Debug / Release 离线回归；配置存在不等于实际执行通过。
核心库、测试与安装无需厂商 SDK；测试 DLL 仅在构建目录，不进入发行包。

2026-10-19 配套 hand SDK 已记录固定干净源码的 Windows Release 工程及发行包验收，
包含通信层无驱动及原生 DLL 检查、独立安装、ZIP、SHA256 和中文空格目录迁移接入。
本机执行和远端 CI 分别记录，Debug 离线检查不代替 Debug 发行包验收。
完整结果和源码 SHA 位于 hand SDK 的 `docs/windows-build.md`；后续源码和安装规则变化
需验证实际交付包，不能沿用旧包校验值。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T "v143,version=14.44" -DLINKERBOT_COMM_BUILD_TESTS=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure --no-tests=error
cmake --install build --config Release --prefix C:/sdk/comm-release
cpack --config build/CPackConfig.cmake -C Release -B dist
```

Debug 将各处的 Release 改为 Debug，并使用独立 build、安装和消费目录。
Release 为 `/MD`，需要对应的 Microsoft Visual C++ x64 Redistributable；
Debug 为 `/MDd`，依赖 Visual Studio 的调试运行库，供开发使用，不作为面向终端用户的运行包。
静态 SDK 不会消除运行库、Windows 系统库或真实通信所需厂商驱动和 DLL 的依赖。

ZIP 名包含 Windows、x64、MSVC 工具集和编译器版本、配置及 MD/MDd；旁边生成
`.sha256`。包内 `share/linkerbot-comm/build-info.json` 保存源码 SHA、配置和编译器。
发布前使用固定的干净源码提交，并核对该文件中的版本、配置和运行库。

将 ZIP 解压至另一条含中文或空格的路径，例如 `C:/验证 SDK/comm Release`：

```powershell
cmake -S "C:/验证 SDK/comm Release/share/linkerbot-comm/validation/installed-consumer" -B consumer -G "Visual Studio 17 2022" -A x64 -T "v143,version=14.44" "-DCMAKE_PREFIX_PATH=C:/验证 SDK/comm Release"
cmake --build consumer --config Release
ctest --test-dir consumer -C Release --output-on-failure --no-tests=error
```

只通过 `find_package` 链接安装包，不引用原源码或构建树。PCAN Classic/FD、MiniCANFD
的原生测试 DLL 由主工程离线回归验证；安装 consumer 验证 Session 收发及缺驱动错误。

实验性 [SLCAN Classic / CANable FD](slcan.md) 随核心包安装，无额外厂商 DLL；系统串口驱动仍须
就绪。`[slcan]` 离线测试只使用字节流桩（Linux 另有自行创建的伪终端），不访问已有串口，安装 consumer 验证 codec/配置符号。
Windows Debug 的指定 CANable 配置已有六型号发现、读取和单关节收发恢复记录，
详见 [SLCAN 验证范围](slcan.md#验证和吞吐)。Lawicel 固件、真实 USB 拔插、Release 实机和
长期负载仍无本轮通过记录；离线取消测试不等于异常实物驱动的耗时保证。
