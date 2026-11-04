# 发行版下载与校验

[English](../en/releases.md) · 简体中文

[文档索引](../README.md) · [源码构建](build.md) · [升级与回滚](operations.md)

## 0.3.1 产物政策

[发行页面 v0.3.1](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.1)
在发行验证和发布完成后提供下载；源码版本号本身不代表已经发布。

- Linux x86_64/aarch64 Release 静态 SDK 使用 Ubuntu 20.04、GCC 9、libstdc++ ABI1。
  新包名包含构建发行版、编译器、标准库/ABI 和配置。
- Windows x64 MSVC v143 ZIP 提供 Release `/MD` 和 Debug `/MDd`，Debug 仍需开发运行库。
- 完整发行验证在 Ubuntu 20.04/22.04/24.04/26.04 双架构消费相同 Linux 归档，并在 Focal
  运行 Noetic 节点/nodelet 集成。
- 每份归档附 SHA256 文件。`release-manifest.json` 记录来源、构建身份和验证运行；
  `SHA256SUMS.txt` 覆盖其余九个上传附件。核对签名标签和清单，不根据文件名推断兼容性。

静态 SDK 不含厂商驱动或库，用户态检查不等于原生内核或硬件验收。范围见 [CI 指南](ci.md)。
下方既有 0.3.0 资产保留原名、来源 SHA 和校验值。

## 0.3.0

从上游仓库[下载 0.3.0](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.0)。
签名标签 `v0.3.0` 对应源码提交 `a46177e86e403bbd18834aedb6e81ef3233c6c22`。
上游与 [fork 标签](https://github.com/soulteary/linkerbot-transport-cpp/tree/v0.3.0)
指向同一源码；二进制包从上游 Release 下载。
`main` 上后续的文档提交不会移动该标签或替换发行包。

- Linux：`aarch64`（ARM64）和 `x86_64`（amd64）的 Release TGZ 包。
  使用 Ubuntu 24.04 和 GCC 13/libstdc++ 构建，不保证兼容更旧的系统运行库。
- Windows：x64 MSVC v143 14.44 的 Release（`MD`）和 Debug（`MDd`）ZIP 包。
  Debug 依赖 Visual Studio 开发运行库，见 [Windows 指南](windows-build.md)。
- 每个包附带 `.sha256` 文件。`SHA256SUMS.txt` 覆盖其余九个上传附件，包括
  `release-manifest.json` 和四个校验文件，不覆盖 GitHub 自动生成的源码归档。

发行包提供静态 SDK，不是设备控制程序，也不包含厂商驱动或库。
选择时匹配进程架构、编译器和运行库；将 `share/linkerbot-comm/build-info.json`
与源码标签、发行清单核对。

## 下载并使用 Linux ARM64 包

在 ARM64 Linux 上执行，需要 `curl`、`sha256sum`、`tar`、C++17 编译器和 CMake 3.20+。
使用新目录。Linux amd64 应选择 `x86_64` 归档。

```bash
mkdir linkerbot-0.3.0
cd linkerbot-0.3.0
release_url=https://github.com/linker-bot/linkerbot-transport-cpp/releases/download/v0.3.0
archive=linkerbot-comm-0.3.0-Linux-aarch64.tar.gz
curl --fail --location --remote-name "$release_url/$archive"
curl --fail --location --remote-name "$release_url/$archive.sha256"
sha256sum --check "$archive.sha256"
tar -xzf "$archive"
prefix="$PWD/linkerbot-comm-0.3.0-Linux-aarch64"
cmake -S "$prefix/share/linkerbot-comm/validation/installed-consumer" \
  -B consumer -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix"
cmake --build consumer --parallel 4
ctest --test-dir consumer --output-on-failure --no-tests=error
```

校验值用于发现下载文件与公开校验值不一致的情况；签名标签验证源码，并不是二进制归档的
独立签名。[发行验证记录](validation.md#release-030)链接构建运行并说明验证范围。
真实硬件须单独验收。

## 发布后的文档

[标签内的文档](https://github.com/linker-bot/linkerbot-transport-cpp/tree/v0.3.0/docs)
和发行包内容保留发布时快照。其中支持文档仍含发布前“尚无稳定发行版”的表述；该表述已由
[当前维护的支持政策](../../SECURITY.zh-CN.md)和本页更正。
后续勘误以[维护中的文档索引](../README.md)为入口。
文档更新不改变 0.3.0 的二进制包或校验值。
