#!/usr/bin/env python3
"""Linux regression checks for actual per-configuration CMake/CPack identities.

Requires Ninja, Clang and both libc++/libstdc++ development headers. The test
builds small SDK configurations with tests/examples disabled; it does not use
synthetic metadata or compare source text.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile


def run(*command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError("Command failed: %r\n%s" % (command, result.stdout))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="clang++-18")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="comm-cmake-package-") as directory:
        root = Path(directory)
        single = root / "single"
        multi = root / "multi"
        common = ("-DLINKERBOT_COMM_BUILD_TESTS=OFF", "-DLINKERBOT_COMM_BUILD_EXAMPLES=OFF",
                  "-DLINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS=OFF", "-DCMAKE_INSTALL_LIBDIR=lib",
                  "-DCMAKE_CXX_COMPILER=" + args.compiler)
        run("cmake", "-S", str(source), "-B", str(single), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -stdlib=libc++", *common)
        run("cmake", "--build", str(single), "--parallel", "4")
        info = json.loads((single / "release/Release/build-info.json").read_text())
        assert info["standard_library"] == "libcxx", info
        assert info["libstdcxx_dual_abi"] == "unknown", info
        # Reconfigure the same build tree: old cached libc++ results must not
        # survive a change to libstdc++ or the nondefault dual ABI.
        run("cmake", "-S", str(source), "-B", str(single),
            "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -stdlib=libstdc++ -D_GLIBCXX_USE_CXX11_ABI=0")
        run("cmake", "--build", str(single), "--parallel", "4")
        info = json.loads((single / "release/Release/build-info.json").read_text())
        assert info["standard_library"] == "libstdcxx" and info["libstdcxx_dual_abi"] == "0", info
        # Single-config CPack retains its configured build type without -C.
        run("cpack", "--config", str(single / "CPackConfig.cmake"), "-B", str(root / "single-dist"))
        assert len(list((root / "single-dist").glob("*-libstdcxx-abi0-Release.tar.gz"))) == 1
        run("cmake", "-S", str(source), "-B", str(multi), "-G", "Ninja Multi-Config",
            "-DCMAKE_CONFIGURATION_TYPES=Debug;Release",
            "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -stdlib=libc++", *common)
        for configuration in ("Debug", "Release"):
            run("cmake", "--build", str(multi), "--config", configuration, "--parallel", "4")
            run("cpack", "--config", str(multi / "CPackConfig.cmake"), "-C", configuration,
                "-B", str(root / "multi-dist"))
        archives = sorted((root / "multi-dist").glob("*.tar.gz"))
        assert len(archives) == 2, archives
        configurations = set()
        for archive in archives:
            with tarfile.open(archive) as package:
                entries = [m for m in package.getmembers() if m.name.endswith("/share/linkerbot-comm/build-info.json")]
                assert len(entries) == 1
                info = json.load(package.extractfile(entries[0]))
            configuration = info["configuration"]
            configurations.add(configuration)
            assert archive.name.endswith("-" + configuration + ".tar.gz"), archive
            expected = "libcxx" if configuration == "Release" else "libstdcxx"
            assert info["standard_library"] == expected, info
            assert ("-libcxx-Release" if configuration == "Release" else "-libstdcxx-abi1-Debug") in archive.name, archive
        assert configurations == {"Debug", "Release"}, configurations
        print("Passed: config-specific libc++, changed-flag cache invalidation/ABI0, single-config fallback, distinct multi-config Debug/Release archives")


if __name__ == "__main__":
    main()
