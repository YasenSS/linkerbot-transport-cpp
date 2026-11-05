#!/usr/bin/env python3
"""Build and run canonical, legacy and mixed-name consumers of a relocated SDK."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile


def run(*command, succeeds=True):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != succeeds:
        raise RuntimeError("Unexpected command result: %r\n%s" % (command, result.stdout))
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--coverage", action="store_true")
    parser.add_argument("--generator")
    parser.add_argument("--platform")
    parser.add_argument("--toolset")
    args = parser.parse_args()
    generator = []
    for option, value in (("-G", args.generator), ("-A", args.platform), ("-T", args.toolset)):
        if value:
            generator.extend((option, value))
    prefix = args.prefix.resolve()
    libraries = list(prefix.rglob("liblinkerbot-can.a")) + list(prefix.rglob("linkerbot-can.lib"))
    assert len(libraries) == 1, libraries
    canonical = libraries[0]
    legacy = canonical.with_name(canonical.name.replace("-can", "-comm"))
    assert hashlib.sha256(canonical.read_bytes()).digest() == hashlib.sha256(legacy.read_bytes()).digest()
    for relative in ("LICENSE", "build-info.json", "docs/en/build.md", "examples/quickstart/CMakeLists.txt"):
        assert (prefix / "share/linkerbot-transport" / relative).read_bytes() == (prefix / "share/linkerbot-comm" / relative).read_bytes()
    cases = {
        "canonical": ("find_package(linkerbot-transport 0.3 CONFIG REQUIRED COMPONENTS can)", "can"),
        "legacy": ("find_package(linkerbot-comm 0.3 CONFIG REQUIRED)", "comm"),
        "legacy-first": ("find_package(linkerbot-comm 0.3 CONFIG REQUIRED)\nfind_package(linkerbot-transport 0.3 CONFIG REQUIRED)", "can"),
        "canonical-first": ("find_package(linkerbot-transport 0.3 CONFIG REQUIRED)\nfind_package(linkerbot-comm 0.3 CONFIG REQUIRED)", "comm"),
        "direct-export": ('include("${CMAKE_PREFIX_PATH}/' + canonical.parent.relative_to(prefix).as_posix() + '/cmake/linkerbot-comm/linkerbot-comm-targets.cmake")', "comm"),
    }
    source_root = Path(__file__).resolve().parents[2].as_posix()
    cases["build-tree-legacy"] = (
        'set(LINKERBOT_COMM_BUILD_TESTS OFF CACHE BOOL "")\n'
        'set(LINKERBOT_COMM_BUILD_EXAMPLES OFF CACHE BOOL "")\n'
        'set(LINKERBOT_COMM_ENABLE_MODBUS OFF CACHE BOOL "")\n'
        'set(LINKERBOT_COMM_ENABLE_SERIAL OFF CACHE BOOL "")\n'
        + 'add_subdirectory("' + source_root + '" sdk)', "comm")
    with tempfile.TemporaryDirectory(prefix="transport-naming-") as directory:
        root = Path(directory)
        for name, (discovery, target) in cases.items():
            source = root / name
            source.mkdir()
            (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
project(naming-consumer LANGUAGES CXX)
''' + discovery + '''
foreach(required can headers can-compat comm comm-values comm-compat)
    if(NOT TARGET linkerbot::${required})
        message(FATAL_ERROR "Missing target ${required}")
    endif()
endforeach()
get_target_property(dependencies linkerbot::can INTERFACE_LINK_LIBRARIES)
if(dependencies MATCHES "linkerbot[^;]*(serial|modbus)")
    message(FATAL_ERROR "CAN unexpectedly links optional modules")
endif()
foreach(property VERSION SOURCE_REVISION)
    get_target_property(canonical_value linkerbot::can LINKERBOT_TRANSPORT_${property})
    get_target_property(legacy_value linkerbot::comm LINKERBOT_COMM_${property})
    if(NOT canonical_value STREQUAL legacy_value)
        message(FATAL_ERROR "Legacy ${property} property differs")
    endif()
endforeach()
add_executable(naming-consumer main.cpp)
target_link_libraries(naming-consumer PRIVATE linkerbot::''' + target + ''')
enable_testing()
add_test(NAME naming-consumer COMMAND naming-consumer)
''')
            (source / "main.cpp").write_text('''#include <linkerbot/can/fake_transport.hpp>
#include <linkerbot/transport/session.hpp>
#include <memory>
int main() {
    auto session = linkerbot::transport::Session::own(std::make_unique<linkerbot::can::FakeTransport>());
    session->close();
    return 0;
}
''')
            build = source / "build"
            flags = ["-DCMAKE_EXE_LINKER_FLAGS=--coverage"] if args.coverage else []
            run(args.cmake, "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=" + args.configuration,
                "-DCMAKE_PREFIX_PATH=" + str(prefix), *generator, *flags)
            run(args.cmake, "--build", str(build), "--config", args.configuration, "--parallel", "2")
            run("ctest", "--test-dir", str(build), "-C", args.configuration, "--output-on-failure", "--timeout", "30")
        for package, component in (("linkerbot-transport", "unknown"), ("linkerbot-comm", "unknown")):
            source = root / (package + "-invalid")
            source.mkdir()
            (source / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.16)\nproject(invalid LANGUAGES CXX)\nfind_package(' + package + ' CONFIG REQUIRED COMPONENTS ' + component + ')\n')
            run(args.cmake, "-S", str(source), "-B", str(source / "build"),
                "-DCMAKE_PREFIX_PATH=" + str(prefix), *generator, succeeds=False)
    print("Passed: canonical/legacy discovery in both orders, direct export, version aliases, archive/share identity and invalid components")


if __name__ == "__main__":
    main()
