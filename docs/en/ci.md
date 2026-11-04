# CI packages and compatibility validation

[简体中文](../zh/ci.md) · [Documentation](../README.md)

The CI separates quality builds from baseline SDK packages and userspace compatibility checks.
A workflow definition is a validation target, not evidence that a platform or physical backend
has passed. Use the run for the exact source commit to establish the observed result.

## Build and validation targets

The baseline package workflow builds native x86_64 and aarch64 Release static SDKs with
Ubuntu 20.04's default GCC 9 and CMake. It disables test dependency downloads, coverage and
sanitizers. The production static library uses PIC so a consumer can link it into a shared
library or ROS nodelet. C++17 remains required; the library does not acquire a ROS dependency.

Pull requests run Ubuntu 20.04 and 26.04 userspace checks on both architectures, plus Noetic
node/nodelet checks on Ubuntu 20.04. Scheduled and full manual checks add Ubuntu 22.04 and
24.04. Each Ubuntu check builds the source with the target's default GCC/CMake and consumes
the same architecture's Focal package. It runs the installed quickstart, installed standalone
consumer and a shared-library consumer. CMake 3.16 runs from the test directory because it
has no `ctest --test-dir`; complete Catch2 tests retain their existing CMake 3.19+ requirement.

Ubuntu 26.04 is a new validation target, including its default CMake 4 toolchain. It does not
imply ROS1 support on 26.04. Noetic integration uses real catkin, a ROS node, a dynamically
loaded nodelet and an unload service call, with FakeTransport only. Noetic is upstream EOL.
Ubuntu 18.04/Melodic, Debian, musl and Windows ARM64 are not added to this matrix.

Containers share the runner kernel. These checks establish userspace/build integration,
not native kernels, SocketCAN interfaces, vendor driver availability or hardware acceptance.
A successful fixture or FakeTransport run must not be described as a real CAN-device test.

## Existing quality gates and efficiency

The existing Linux Debug/Release, coverage, libc++, sanitizer, style, report-card and Windows
checks remain enabled. This change does not weaken their coverage or claim measured speedups.
Linux build and report jobs use ccache in runner temporary storage. Restore prefixes separate
architecture, compiler family and build mode; ccache additionally hashes compiler content and
compilation inputs. Cached objects do not replace regression execution. Baseline release
packages do not restore this cache.

The CMake 3.16.9 check caches its installed tool directory using its source SHA256, architecture
and build-policy identity. A miss still downloads and verifies the source before building it;
each run verifies the restored version. Windows cancels superseded runs of the same workflow/ref.

Toolchain images are currently pulled by explicit release tag, their resolved digest is recorded,
and that digest is used for execution. Apt repositories can still change. This provides build
provenance, not byte-for-byte reproducibility or a permanently pinned package universe. Prebuilt
versioned toolchain images and further PR/main matrix reduction remain follow-up work, requiring
cold/warm timing and cache-hit evidence before removing existing checks.

## Package identity and provenance

New Linux archive names include system, architecture, build distribution, compiler version,
standard library and build configuration. Existing 0.3.0 Release assets remain unchanged.
Windows names retain their MSVC toolset, compiler version, configuration and CRT. Only x64
Windows packages are currently allowed; a 64-bit pointer alone is not proof of x64.

`share/linkerbot-comm/build-info.json` records source SHA, dirty state (including untracked
non-ignored files), distribution, compiler, standard library, C++ standard, PIC, instrumentation,
configuration, CRT and optional build-image identity. It is captured at configure time.
Reconfigure after changing inputs. Custom flags/toolchains still need consumer validation;
metadata is not an exhaustive binary attestation or ABI guarantee.

`LINKERBOT_COMM_RELEASE_BUILD=ON` requires a clean source tree and full source SHA and rejects
recognized coverage/sanitizer options. `LINKERBOT_COMM_BUILD_IMAGE` supplies an optional image
identity. Ordinary developer builds remain permitted with dirty/unknown provenance. Exported
sources without Git need an explicitly supplied SHA; that value is a caller assertion.

`validation/package/verify-package.py` checks expected metadata, all ELF archive members and
unwanted runtime/test components. `test-verify-package.py` exercises rejection paths. Package
consumption checks record version requirements from the final ELF consumers: `ldd` on a static
archive cannot establish its runtime compatibility. The recorded symbol list is evidence, not
an automatic promise that arbitrary compiler/standard-library combinations can be mixed.

## Running the workflows

Use **Focal baseline SDK packages** to build packages without publishing. Use **Ubuntu and
ROS1 compatibility** with `full_matrix=true` for all four Ubuntu userspaces. The default PR
matrix checks the oldest and newest endpoints. Both use read-only repository permissions.

The release workflow accepts a `vMAJOR.MINOR.PATCH` tag matching the CMake project version and
pointing into main history. Tag pushes validate only. It builds Linux and Windows packages,
runs the full Ubuntu/Noetic matrix against the exact Linux artifacts, and requires successful
Linux, Windows, style and report-card workflows for the same commit (trusted push/manual runs).
If those quality runs are still pending, wait for completion and rerun the failed release gate.

For a draft, manually run **Validate release and optionally create draft** on that version tag
and explicitly set `create_draft=true`. The workflow:

1. Requires two Linux Release packages and Windows x64 Debug/Release packages from this run.
2. Verifies checksums, source, version, baseline, configuration and CRT identities.
3. Writes `release-manifest.json` and `SHA256SUMS.txt`.
4. Rechecks the remote tag target and refuses any existing Release.
5. Uploads the already-tested bytes to a new draft and downloads them again to compare hashes.

Only the draft job receives repository write permission. No workflow automatically publishes a
Release or moves a tag. Inspect draft assets and evidence before publishing through the normal
maintainer process. A failed upload can leave an incomplete draft: inspect it before retrying;
the workflow intentionally refuses to overwrite it. Corrections to released bytes require a
new version, rather than replacing an old archive. No new tag or Release is needed to review
this CI change itself.
