# Release downloads and verification

English · [简体中文](../zh/releases.md)

[Documentation index](../README.md) · [Build from source](build.md) · [Upgrade and rollback](operations.md)

Current source uses the `linkerbot-transport` package name. Published releases
through v0.3.2 retain `linkerbot-comm`; the 0.3.0 download example below deliberately
uses its historical asset names and paths. See [name migration](naming.md).

## 0.4.0

[Version 0.4.0](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.4.0)
adds shared Runtime ownership, independent device Sessions and opt-in scheduling
controls on the existing writer. It adds explicit classic/FD transmit selection,
native receive timestamp metadata and opt-in SocketCAN/PCAN receive-own messages.
Vendor and SLCAN backends reject unsupported receive-own opt-in.

Existing C++ signatures and default behavior are retained. Public structures have
grown: rebuild consumers with matching headers and libraries; binary ABI
compatibility with previous SDKs is not promised. Python pre-integration probes
are test fixtures, not a production Python binding or hand/arm integration.
Physical hardware acceptance remains pending. See [CAN metadata](can-metadata.md),
[device Sessions](device-sessions.md) and the [changelog](../../CHANGELOG.en.md).

Release validation retains the twelve-target matrix below. Archive names begin
with `linkerbot-transport-0.4.0-`. Download assets only after publication and verify
the signed tag, manifest and checksums. Older release assets remain unchanged.

## 0.3.3

[Version 0.3.3](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.3)
adds optional serial byte streams and Modbus RTU (03/04/06/10), CAN/RTU lifecycle
fixes and the `linkerbot-transport` package name. The CAN target is `linkerbot::can`;
legacy package names, targets, options and archive paths remain supported.
See [migration and rollback](naming.md) and [serial/RTU contracts](modbus-rtu.md).
The hand SDK and device register maps are not part of this release.

The release uses the same twelve-target matrix described below. New archive names
start with `linkerbot-transport-0.3.3-`; installed documents and metadata live under
`share/linkerbot-transport`. Each default SDK contains CAN, serial and Modbus
libraries plus the byte-identical legacy comm archive. Check the signed tag,
`release-manifest.json`, `SHA256SUMS.txt` and the matching package checksum.
Assets become available after validation and publication complete; source version
and CI artifacts alone do not establish publication.

For example, on Ubuntu 24.04 ARM64, use GitHub CLI and Bash to download the matching
Release SDK and its checksum, then build a relocated consumer:

```bash
gh release download v0.3.3 --repo linker-bot/linkerbot-transport-cpp \
  --pattern 'linkerbot-transport-0.3.3-Linux-aarch64-ubuntu24.04-*-Release.tar.gz*' \
  --dir sdk-0.3.3
cd sdk-0.3.3
sha256sum --check *.sha256
archives=(*.tar.gz)
test "${#archives[@]}" -eq 1
tar -xzf "${archives[0]}"
prefix="$PWD/${archives[0]%.tar.gz}"
cmake -S "$prefix/share/linkerbot-transport/validation/installed-consumer" \
  -B consumer -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix"
cmake --build consumer --parallel 4
ctest --test-dir consumer --output-on-failure --no-tests=error
```

Choose the actual distribution and process architecture. Ubuntu userspace and
offline adapter tests do not establish native-kernel or hardware acceptance.

## 0.3.2 artifact matrix

The [v0.3.2 release page](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.2)
provides downloads after validation and publication complete. The 0.3.2 workflow requires
eight actual Ubuntu-specific Release SDKs (20.04, 22.04,
24.04 and 26.04 × x86_64/aarch64) and four Windows SDKs (x64/ARM64 × Debug/Release).
Each Ubuntu package is built and consumed in its own distribution; the Focal baseline is
also retained for cross-distribution and Noetic checks. Twelve archives, twelve adjacent
checksums, a manifest and a total checksum list make 26 uploaded assets. Native Windows
ARM64 execution and object-machine checks are mandatory before its packages can be released.
Verify the signed tag, `release-manifest.json` and all checksums, then select the matching
distribution, architecture and CRT. The manifest records source/build identity and validation
run; native SDK checks do not establish vendor-driver or hardware availability.
See [CI package policy](ci.md). This does not add or replace any 0.3.1 assets below.

## 0.3.1 package policy

The [v0.3.1 release page](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.1)
is the download destination once release validation and publication complete. The source
version alone does not establish that a release has been published.

- Linux x86_64/aarch64 Release static SDKs use Ubuntu 20.04, GCC 9 and libstdc++ ABI1.
  New names include the build distribution, compiler, standard library/ABI and configuration.
- Windows x64 MSVC v143 ZIPs provide Release `/MD` and Debug `/MDd`; Debug still requires
  development runtime support.
- Full release validation consumes the same Linux archives in Ubuntu 20.04/22.04/24.04/26.04
  on both architectures and runs Noetic node/nodelet integration on Focal.
- Each archive has a SHA256 file. `release-manifest.json` records source/build identity and
  the validation run; `SHA256SUMS.txt` covers the other nine uploaded assets. Verify the
  signed tag and manifest rather than inferring compatibility from the filename.

These are static SDKs without vendor drivers or libraries. Userspace checks are not native
kernel or hardware acceptance. See [CI scope and limits](ci.md). Existing 0.3.0 assets below
retain their names, source SHA and checksums.

## 0.3.0

[Download 0.3.0](https://github.com/linker-bot/linkerbot-transport-cpp/releases/tag/v0.3.0)
from the upstream repository. The signed tag `v0.3.0` identifies source commit
`a46177e86e403bbd18834aedb6e81ef3233c6c22`. The upstream and
[fork tag](https://github.com/soulteary/linkerbot-transport-cpp/tree/v0.3.0)
identify the same source; the upstream Release is the binary download location.
Later documentation commits on `main` do not move this tag or replace its packages.

- Linux: `aarch64` (ARM64) and `x86_64` (amd64) Release TGZ packages.
  Built on Ubuntu 24.04 with GCC 13/libstdc++; compatibility with older system
  runtimes is not guaranteed.
- Windows: x64 MSVC v143 14.44 Release (`MD`) and Debug (`MDd`) ZIP packages.
  Debug requires the Visual Studio development runtime; see the [Windows guide](windows-build.md).
- Each archive has an adjacent `.sha256` file. `SHA256SUMS.txt` covers all nine
  other uploaded assets, including `release-manifest.json` and the four checksum files.
  It does not cover GitHub's automatically generated source archives.

The packages contain a static SDK, not a device-control executable. Vendor drivers
and libraries are not included. Match the process architecture, compiler and runtime;
check `share/linkerbot-comm/build-info.json` against the tag and release manifest.

## Download and consume the historical 0.3.0 Linux ARM64 package

Run on ARM64 Linux with `curl`, `sha256sum`, `tar`, a C++17 compiler and CMake 3.20+.
Use a fresh directory. For Linux amd64, select the `x86_64` archive instead.

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

The checksum detects a download differing from the published checksum; the signed
tag authenticates source, not a detached signature over the binary archives.
The [release validation record](validation.md#release-030) links the build runs and
records the verification scope. Validate physical hardware separately.

## Documentation after publication

The [tagged documentation](https://github.com/linker-bot/linkerbot-transport-cpp/tree/v0.3.0/docs)
and package contents preserve the release snapshot. Their support document still
contains the pre-publication statement that no stable release exists; that statement
is superseded by the [maintained support policy](../../SECURITY.md) and this page.
Use the [maintained documentation index](../README.md) for subsequent corrections.
Documentation updates do not alter the 0.3.0 binaries or their checksums.
