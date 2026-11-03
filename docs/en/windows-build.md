# Windows builds and standalone communication packages

[简体中文](../zh/windows-build.md) · [Documentation](../README.md) · [General builds](build.md) ·
[Validation evidence](validation.md)

Published SDKs and checksum instructions are in the [release guide](releases.md).

Use Visual Studio 2022, v143 14.44, CMake 3.31.8 and C++17, targeting x64.
The GitHub workflow runs Debug/Release offline regression, native ABI fixtures, ZIP packaging
and extracted-package consumption on windows-2022. Fixed-commit successful runs are linked
in [Validation](validation.md). Later source changes require their own head-specific runs;
old evidence does not validate a new package. Building, testing and installing the core library
requires no vendor SDK. Test DLLs remain in the build directory and are excluded from packages;
physical hardware requires separately installed matching drivers and vendor DLLs.

Run from the source root. With a multi-configuration generator, choose the same configuration
for build, CTest, install and CPack:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T "v143,version=14.44" -DLINKERBOT_COMM_BUILD_TESTS=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure --no-tests=error --timeout 60
cmake --install build --config Release --prefix C:/sdk/comm-release
cpack --config build/CPackConfig.cmake -C Release -B dist
```

For Debug, replace Release consistently with Debug and use separate build, installation and
consumer directories. Release defaults to `/MD`, requiring the corresponding Microsoft Visual
C++ x64 Redistributable. Debug uses `/MDd`, depends on Visual Studio's debug runtime and is
intended for development rather than end-user runtime packages. A static SDK does not eliminate
dependencies on the CRT, Windows system libraries or hardware-specific vendor drivers and DLLs.

ZIP filenames include Windows, x64, MSVC toolset/compiler versions, configuration and MD/MDd;
an adjacent `.sha256` file is generated. Package file
`share/linkerbot-comm/build-info.json` records source SHA, configuration and compiler.
Before release, use a fixed clean source commit and check the recorded version, configuration and CRT.

Extract the ZIP into a different path containing Chinese characters or spaces, for example
`C:/验证 SDK/comm Release`:

```powershell
cmake -S "C:/验证 SDK/comm Release/share/linkerbot-comm/validation/installed-consumer" -B consumer -G "Visual Studio 17 2022" -A x64 -T "v143,version=14.44" "-DCMAKE_PREFIX_PATH=C:/验证 SDK/comm Release"
cmake --build consumer --config Release
ctest --test-dir consumer -C Release --output-on-failure --no-tests=error
```

The consumer links through `find_package` only, without referencing the original source or
build tree. Main-project offline regression verifies PCAN Classic/FD and MiniCANFD native
test DLLs; the installed consumer checks Session I/O and missing-driver errors.

Experimental [SLCAN Classic / CANable FD](slcan.md) is installed with the core package and
needs no extra vendor DLL, but the system serial driver must be available. `[slcan]` offline
tests use byte-stream fixtures only (Linux additionally creates its own pseudo-terminals),
without opening existing serial ports. The installed consumer checks codec/configuration symbols.
Historical Windows Debug/CANable model statements and current hardware limits are documented
in [Validation](validation.md). CI does not connect USB devices or establish a hard exit deadline
for abnormal drivers.

## Delivery checks and common issues

- CPack produces ZIPs with adjacent `.sha256` files by default. After checking the digest,
  build the installed consumer in a new directory.
- Use `Get-FileHash` to show the package digest without printing environment variables or credentials:

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath 'dist/actual-package-name.zip'
```

Compare it with the matching filename's digest in the adjacent checksum file; replace the
example path with the real artifact. `build-info.json` is generated during configuration;
rerun CMake after switching source revisions. Default package CRT labels describe MD/MDd.
A custom CRT does not establish that the package filename/metadata represents the change:
if setting `CMAKE_MSVC_RUNTIME_LIBRARY`, inspect the actual runtime, update delivery information
and validate consumers separately rather than inheriting the default package's validation claim.

Applications must match the library's x64 architecture, toolset, configuration and CRT.
A missing vendor DLL does not cause static-library link failure; runtime loading failures
require checking the actual path, dependent libraries and bitness. See
[Troubleshooting](troubleshooting.md). Documentation is installed under
`share/linkerbot-comm/docs/` and can be browsed without the original source tree.
