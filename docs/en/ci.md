# CI packages and compatibility validation

[简体中文](../zh/ci.md) · [Documentation](../README.md)

The CI separates actual SDK production from quality checks and cross-distribution consumption.
The current release target is **12 independently built SDK archives**, declared in
`ci/package-targets.json`. This matrix applies to 0.3.2 and later builds; the four SDK archives
already published in 0.3.1 remain unchanged. A workflow target is not evidence of hardware acceptance.

## Actual package matrix

- Ubuntu 20.04, 22.04, 24.04 and 26.04 each produce x86_64 and aarch64 Release TGZs: eight
  archives. Every job uses that Ubuntu userspace's default GCC, CMake, headers and libraries,
  runs the example, and consumes its own relocated package. Newer distribution packages are
  not renamed copies of a Focal package. Linux Debug remains a quality-build configuration.
- Windows x64 and ARM64 each produce Debug and Release ZIPs: four archives. x64 uses
  windows-2022/VS2022; ARM64 uses the native windows-11-vs2026-arm runner and VS2026.
  Both select v143 14.44. Native CMake 3.31.8/x64 and 4.2.3/ARM64 downloads are checksum-checked.
  The jobs assert native OS architecture, inspect executable/DLL PE machines and every SDK
  COFF object, execute regression/loader tests, and run a matching relocated consumer.

Each archive has its own SHA256 file. With `release-manifest.json` and `SHA256SUMS.txt`, a full
release has **26 uploaded assets**, excluding GitHub's automatically generated source archives.
Only the named 64-bit architectures are included; x86_64 is not 32-bit x86, and ARM64 is not ARM32.

Linux package artifacts include the distribution: `sdk-linux-ubuntu<version>-<arch>`.
Windows artifacts are `sdk-windows-<arch>-<configuration>`. They cannot overwrite another target.
Linux runs on same-architecture hosted runners inside the selected Ubuntu container; those
containers share the host kernel, so they validate userspace, not each Ubuntu release's kernel.

## Compatibility checks retained alongside production

The native producers build all eight Linux packages by default, including on pull requests.
An explicit `baseline_only=true` is available for focused Focal debugging, but a complete
release manifest rejects that incomplete asset set.

The separate compatibility matrix deliberately downloads the **Focal** artifact for the
matching architecture. PRs check consuming it on Ubuntu 20.04 and 26.04; scheduled/full manual
runs also check 22.04 and 24.04. This preserves the old baseline's cross-distribution evidence
alongside each distribution's own package validation. It does not substitute for producing
those other packages. Noetic node/nodelet integration also uses the Focal package.

Ubuntu 26.04 checks its default CMake 4 toolchain. CMake 3.16 checks run CTest from the build
directory, while complete Catch2 regression still requires 3.19+. Noetic uses real catkin,
node/nodelet loading and unloading with FakeTransport; it is upstream EOL and is tested only
on Focal. The core remains C++17 without a ROS dependency.

No check claims native-kernel, SocketCAN-interface, vendor-driver or physical-device acceptance.
A Windows ARM64 SDK does not establish availability of an ARM64 vendor driver/DLL. Ubuntu
18.04/Melodic, Debian, musl and 32-bit targets remain outside this matrix.

## Quality and efficiency

Existing Linux Debug/Release, coverage, libc++, sanitizer, style and report checks remain.
Linux build/report jobs use ccache in runner temporary storage, isolated by architecture,
compiler and build mode. Release producers do not restore that cache. Minimum CMake 3.16.9
uses a verified-source installed-tool cache and checks its version on every run. Windows
cancels superseded workflow/ref runs. None of these changes claims a controlled speedup.

A native producer builds the SDK/example once, then runs `ci/ubuntu-smoke.sh --consume-only`
against the package rather than compiling the source again. Cross-distribution jobs separately consume the Focal package without repeating the native
source build. Container digests are recorded and executed by digest;
apt inputs can change, so byte-reproducible builds are not promised. Prebuilt toolchain images
and further matrix reduction need separate measurements.

## Package identity and validation

Archive names and `share/linkerbot-transport/build-info.json` distinguish source SHA, distribution,
architecture, compiler, probed standard library/dual ABI, configuration and Windows CRT.
PIC and instrumentation state are recorded. Probe caches include configuration flags;
multi-config CPack names use the selected `-C` configuration. Reconfigure when inputs change.

`LINKERBOT_TRANSPORT_RELEASE_BUILD=ON` requires clean source/full SHA and rejects recognized
coverage/sanitizer flags. `LINKERBOT_TRANSPORT_BUILD_IMAGE` records an optional image identity.
Developer dirty/unknown-source builds remain allowed. Provenance is not an exhaustive ABI
attestation, and a caller-supplied SHA for a source export is not a content signature.

The generic package verifier checks expected distribution/system/configuration, metadata,
all ELF or COFF archive members and leaked runtime/test components. Regressions cover wrong
or mixed machines and missing/duplicate release identities. Final ELF consumers provide symbol
version evidence; `ldd` on a static archive cannot establish runtime compatibility.

## Release workflow

Use **Native Ubuntu SDK packages** to produce artifacts without publishing, or **Ubuntu and
ROS1 compatibility** with `full_matrix=true` for the full baseline consumption matrix.
All ordinary build jobs have read-only repository permissions.

A release tag must match the CMake version and main history. Tag pushes validate only. The
release workflow requires exact-source trusted Linux/Windows/style/report success, builds all
12 packages in the same run, and waits for full compatibility checks. Windows quality
requires four successful jobs; Focal Session requires both native GCC 9 jobs on the exact
source commit. Pending, missing or failed quality checks block release preparation.

The final tag-push job seals the exact tag object, source SHA, workflow attempt, successful
job identities, twelve artifact IDs/digests and fully verified package manifest. Its seal is
read-only evidence, not a signature over the binaries. Manual `create_draft=true` on the same
version tag also requires an explicit `validated_run_id` naming that successful tag-push run.
It rechecks the quality gates and promotes those original artifacts without rebuilding.
Fork/PR sources, other tags/commits, incomplete matrices, expired or changed artifacts,
changed tag objects, concurrent reruns and legacy runs without a seal are rejected.
No source run is selected implicitly. Revalidation still reconstructs the complete manifest.
SDK and compatibility-evidence artifact names include the workflow attempt. A full rerun can
retain earlier attempt artifacts without promoting them; rerun **all jobs**, since promotion
intentionally rejects mixed-attempt results from a failed-jobs-only rerun. If the selected
attempt or its artifacts expire, complete validation again instead of weakening these checks.

For a future version tag that includes these workflows, inspect its successful tag-push run
and then explicitly promote it (replace both placeholders):

```bash
gh workflow run release.yml --repo linker-bot/linkerbot-transport-cpp \
  --ref VERSION_TAG -f create_draft=true -f validated_run_id=SUCCESSFUL_TAG_PUSH_RUN_ID
```

This command creates a new draft only. Existing releases and old tags without a validation
seal cannot use this path; do not move or recreate those tags to adopt the new workflow.
The manifest must contain exactly the eight Linux distribution/architecture identities and
four Windows architecture/configuration identities. Hashes, sizes, metadata and all library
object machines are checked before writing the manifest and checksum list. A reused Focal
package cannot satisfy a different distribution identity.

The draft job rechecks the remote tag, refuses existing releases, uploads the already-tested
bytes and downloads them to compare hashes. It alone has repository write permission. It never
automatically publishes or moves tags. Inspect an incomplete draft before retrying; released
bytes require a new version instead of replacement. Changing these workflows does not itself create a tag or Release.

## Python pre-integration validation

The separate `python-preintegration.yml` workflow builds an isolated CPython probe
against the installed native library. It tests conventional GIL-enabled CPython
3.10–3.13 on Linux x86_64/ARM64, Windows x64 with 3.12 and Windows ARM64 with 3.13.
The source fixture is `validation/python-preintegration`; it is not a production
binding or a wheel. Subinterpreters and free-threaded builds are rejected.
Allocation/receipt, callback, lifecycle and deployment-gate checks do not establish
actual hand/arm Runtime sharing or hardware compatibility. See [CAN metadata](can-metadata.md)
for backend-specific capabilities and physical validation limits.

## CI timing and reproducible comparisons

See the [measured results and limitations](ci-performance.md), including all baseline and
optimized run URLs, three-run medians, Windows cache overhead and remaining release validation.

Style and full-report manual runs accept `analysis_jobs=2` (baseline) or `4` (optimized),
bounded by the runner's available CPU affinity. Both preserve every existing rule, translation
unit and complete-report gate. Evidence records requested/effective concurrency, source SHA
and configuration digest. Compare matching source/tool versions and analysis-step medians;
workflow wall time also includes scheduling and setup. Style saves GNU time resource records;
maximum RSS is not an aggregate peak for all simultaneous analyzer processes.

Windows manual runs accept `use_tool_cache=false` for a forced download and `true` for reuse.
The fixed CMake cache key includes version, download architecture and a freshly fetched
official archive checksum. Each hit authenticates the ZIP and every installed file before
selecting its exact executable/version. PRs do not save this cache. Run cold/warm comparisons
sequentially because the existing Windows concurrency policy cancels superseded runs; include
checksum, cache restore/save and verification overhead when assessing net benefit.
