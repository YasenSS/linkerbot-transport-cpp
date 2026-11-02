# Windows 构建与独立通信包

[文档索引](../README.md) · [通用构建](build.md) · [验证记录](validation.md)

使用 Visual Studio 2022、v143 14.44、CMake 3.31.8 和 C++17，目标架构为 x64。
当前 GitHub 工作流在 windows-2022 上执行 Debug/Release 离线回归、原生 ABI fixture、
ZIP 与解压消费，固定提交的已通过记录见 [验证说明](validation.md)。后续源码仍须检查对应
head 的实际 run，不能把旧记录沿用为新包证明。核心库、测试和安装无需厂商 SDK，
测试 DLL 留在构建目录，不进入发行包；真实硬件需要另装对应驱动和厂商 DLL。

从源码根目录执行以下命令；多配置生成器构建、测试、安装和 CPack 必须选择同一配置：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T "v143,version=14.44" -DLINKERBOT_COMM_BUILD_TESTS=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure --no-tests=error --timeout 60
cmake --install build --config Release --prefix C:/sdk/comm-release
cpack --config build/CPackConfig.cmake -C Release -B dist
```

Debug 将各处的 Release 改为 Debug，并使用独立 build、安装和消费目录。
默认 Release 为 `/MD`，需要对应的 Microsoft Visual C++ x64 Redistributable；
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
历史 Windows Debug / CANable 型号陈述和当前硬件限制见 [验证记录](validation.md)。
CI 不连接 USB 实物，也不验证异常驱动的硬性退出时限。

## 交付核对与常见问题

- CPack 默认产出 ZIP 和相邻 `.sha256`；验证摘要后，再在新目录编译安装 consumer。
- 使用 `Get-FileHash` 只输出包摘要，不输出环境变量或凭据：

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath 'dist/实际包名.zip'
```

将结果与同目录校验文件中对应文件名的摘要比较；示例路径需要换成实际产物。
`build-info.json` 在配置时生成，源码切换后重新运行 CMake。
默认打包运行库标记按 MD/MDd 生成；自定义 CRT 并不自动证明包名/metadata 能表达该变化，
若更改 `CMAKE_MSVC_RUNTIME_LIBRARY`，应单独核对实际运行库、调整交付信息并验证消费者，
不能将其沿用为已验证的默认 SDK 包。

应用必须使用与库一致的 x64、工具集、配置及 CRT。缺少厂商 DLL 不是链接本静态库失败的原因；
运行时加载失败应检查实际路径、同目录依赖和位数，见 [排障指南](troubleshooting.md)。
新文档随包安装于 `share/linkerbot-comm/docs/`，不需要原源码树即可浏览。
