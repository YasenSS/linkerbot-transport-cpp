cmake_minimum_required(VERSION 3.16)
project(linkerbot-transport VERSION 0.3.4 LANGUAGES CXX)
if(MSVC)
    add_compile_options(/utf-8)
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Transport package with separate CAN, serial and Modbus link targets.
# CAN owns physical buses, frame routing and batched sends.

include(cmake/LegacyOptions.cmake)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(LINKERBOT_TRANSPORT_IS_TOP_LEVEL ON)
else()
    set(LINKERBOT_TRANSPORT_IS_TOP_LEVEL OFF)
endif()

linkerbot_transport_cache(LINKERBOT_TRANSPORT_BUILD_TESTS BOOL "Build Catch2 unit tests" ${LINKERBOT_TRANSPORT_IS_TOP_LEVEL})
option(LINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS "Allow missing test dependencies to be fetched" ON)
linkerbot_transport_cache(LINKERBOT_TRANSPORT_BUILD_EXAMPLES BOOL "Build example programs" ${LINKERBOT_TRANSPORT_IS_TOP_LEVEL})
linkerbot_transport_cache(LINKERBOT_TRANSPORT_ENABLE_COVERAGE BOOL "Instrument Linux tests for gcovr line coverage" OFF)
linkerbot_transport_cache(LINKERBOT_TRANSPORT_ENABLE_SERIAL BOOL "Build native serial byte streams" ON)
linkerbot_transport_cache(LINKERBOT_TRANSPORT_ENABLE_MODBUS BOOL "Build Modbus RTU (requires serial)" ON)
if(LINKERBOT_TRANSPORT_ENABLE_MODBUS AND NOT LINKERBOT_TRANSPORT_ENABLE_SERIAL)
    message(FATAL_ERROR "LINKERBOT_TRANSPORT_ENABLE_MODBUS requires LINKERBOT_TRANSPORT_ENABLE_SERIAL")
endif()

linkerbot_transport_cache(LINKERBOT_TRANSPORT_BUILD_BENCHMARKS BOOL "Build Linux performance measurement tools" OFF)

if(LINKERBOT_TRANSPORT_ENABLE_COVERAGE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "Coverage requires Linux and GCC")
    endif()
    if(NOT LINKERBOT_TRANSPORT_BUILD_TESTS)
        message(FATAL_ERROR "Coverage requires LINKERBOT_TRANSPORT_BUILD_TESTS=ON")
    endif()
    # Concurrent fixtures must not race while updating gcov counters.
    add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
    add_link_options(--coverage)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(LINKERBOT_TRANSPORT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(linkerbot-headers INTERFACE)
add_library(linkerbot::headers ALIAS linkerbot-headers)
add_library(linkerbot-comm-values ALIAS linkerbot-headers)
add_library(linkerbot::comm-values ALIAS linkerbot-headers)
target_include_directories(linkerbot-headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(linkerbot-headers INTERFACE cxx_std_17)

add_library(linkerbot-can STATIC
    src/vendor_canfd_transport.cpp
    src/vendor_canfd_loader.cpp
    src/session.cpp
    src/runtime.cpp
    src/pcan_transport.cpp
    src/pcan_loader.cpp
    src/slcan_transport.cpp
    src/slcan_serial.cpp
)
add_library(linkerbot::can ALIAS linkerbot-can)
add_library(linkerbot-comm ALIAS linkerbot-can)
add_library(linkerbot::comm ALIAS linkerbot-can)
# Preserve manual archive users; this is the same binary, not a second build.
add_custom_command(TARGET linkerbot-can POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:linkerbot-can>
        "$<TARGET_FILE_DIR:linkerbot-can>/${CMAKE_STATIC_LIBRARY_PREFIX}linkerbot-comm${CMAKE_STATIC_LIBRARY_SUFFIX}")
# Support consumers that embed this static SDK in nodelets and other shared libraries.
set_target_properties(linkerbot-can PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_features(linkerbot-can PUBLIC cxx_std_17)
target_include_directories(linkerbot-can
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(linkerbot-can PUBLIC Threads::Threads)
if(CMAKE_DL_LIBS)
    target_link_libraries(linkerbot-can PRIVATE ${CMAKE_DL_LIBS})
endif()

# Serial and RTU are independent of CAN Message/Session and have separate link targets.
set(_linkerbot_optional_targets "")
if(LINKERBOT_TRANSPORT_ENABLE_SERIAL)
    add_library(linkerbot-serial STATIC src/serial/common.cpp src/serial/native.cpp)
    add_library(linkerbot::serial ALIAS linkerbot-serial)
    target_link_libraries(linkerbot-serial PUBLIC linkerbot::headers Threads::Threads)
    set_target_properties(linkerbot-serial PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME serial)
    list(APPEND _linkerbot_optional_targets linkerbot-serial)
endif()
if(LINKERBOT_TRANSPORT_ENABLE_MODBUS)
    add_library(linkerbot-modbus STATIC src/modbus/codec.cpp src/modbus/session.cpp)
    add_library(linkerbot::modbus ALIAS linkerbot-modbus)
    target_link_libraries(linkerbot-modbus PUBLIC linkerbot-serial)
    set_target_properties(linkerbot-modbus PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME modbus)
    list(APPEND _linkerbot_optional_targets linkerbot-modbus)
endif()

# Flat-name compatibility headers (`#include "can_transport.hpp"`), for
# consumers migrating from the in-tree src/ layout. Link this instead of
# `linkerbot-can` to get both paths; drop it once includes are updated.
add_library(linkerbot-can-compat INTERFACE)
add_library(linkerbot::can-compat ALIAS linkerbot-can-compat)
add_library(linkerbot-comm-compat ALIAS linkerbot-can-compat)
add_library(linkerbot::comm-compat ALIAS linkerbot-can-compat)
target_link_libraries(linkerbot-can-compat INTERFACE linkerbot-can)
target_include_directories(linkerbot-can-compat
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/linkerbot/compat>
)

if(LINKERBOT_TRANSPORT_BUILD_TESTS)
    if(CMAKE_VERSION VERSION_LESS 3.19)
        message(FATAL_ERROR "Tests require CMake 3.19+ for Catch2 discovery; use LINKERBOT_TRANSPORT_BUILD_TESTS=OFF for the CMake 3.16 library-only build")
    endif()
    find_package(Catch2 3 QUIET)
    if(NOT Catch2_FOUND)
        if(NOT LINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS)
            message(FATAL_ERROR "Catch2 3 is required; install it or provide Catch2_DIR (downloads disabled)")
        endif()
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.8.1
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()

    enable_testing()

    add_executable(can-tests
        tests/main.cpp
        tests/test_dispatcher.cpp
        tests/test_session.cpp
        tests/test_session_receive_compat.cpp
        tests/test_session_tx_compat.cpp
        tests/test_transport_runtime.cpp
        tests/test_session_tx_edges.cpp
        tests/test_session_physical_compat.cpp
        tests/test_session_review.cpp
        tests/test_fault_contracts.cpp
        tests/test_fake_transport.cpp
        tests/test_frame.cpp
        tests/test_managed_bus.cpp
        tests/test_socketcan_lifecycle.cpp
        tests/test_vendor_canfd_transport.cpp
        tests/test_slcan_transport.cpp
        tests/test_slcan_serial_linux.cpp
    )
    target_link_libraries(can-tests PRIVATE linkerbot-can Catch2::Catch2)
    if(NOT LINKERBOT_TRANSPORT_IS_TOP_LEVEL)
        set_target_properties(can-tests PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

    add_executable(session-static-exit tests/session_static_exit.cpp)
    target_link_libraries(session-static-exit PRIVATE linkerbot-can)
    add_executable(subscriber-allocation-failure tests/subscriber_allocation_failure.cpp)
    target_link_libraries(subscriber-allocation-failure PRIVATE linkerbot-can)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # This single-threaded fixture replaces global new/delete; TSan defines
        # those symbols too. Target options override global -fsanitize=thread
        # while leaving ASan/UBSan enabled. can-tests retains TSan coverage of
        # concurrent snapshots, cancellation and lifecycle operations.
        target_compile_options(subscriber-allocation-failure PRIVATE -fno-sanitize=thread)
        target_link_options(subscriber-allocation-failure PRIVATE -fno-sanitize=thread)
    endif()
    # TSan supplies global allocation replacements. Keep this fault-injection
    # executable out of TSan builds; can-tests still checks concurrent APIs.
    if(NOT CMAKE_CXX_FLAGS MATCHES "fsanitize=thread")
        add_executable(lifecycle-allocation-failure tests/lifecycle_allocation_failure.cpp)
        target_link_libraries(lifecycle-allocation-failure PRIVATE linkerbot-can)
        if(LINKERBOT_TRANSPORT_ENABLE_MODBUS)
            target_link_libraries(lifecycle-allocation-failure PRIVATE linkerbot-modbus)
            target_compile_definitions(lifecycle-allocation-failure PRIVATE LINKERBOT_TEST_MODBUS)
        endif()
        add_test(NAME lifecycle-allocation-failure COMMAND lifecycle-allocation-failure)
        set_tests_properties(lifecycle-allocation-failure PROPERTIES TIMEOUT 30)
        if(NOT LINKERBOT_TRANSPORT_IS_TOP_LEVEL)
            set_target_properties(lifecycle-allocation-failure PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endif()
    add_test(NAME subscriber-allocation-failure COMMAND subscriber-allocation-failure)
    set_tests_properties(subscriber-allocation-failure PROPERTIES TIMEOUT 10)
    if(NOT LINKERBOT_TRANSPORT_IS_TOP_LEVEL)
        set_target_properties(session-static-exit subscriber-allocation-failure
            PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
    add_test(NAME session-static-share-exit COMMAND session-static-exit shared)
    add_test(NAME session-static-borrow-exit COMMAND session-static-exit borrowed)
    set_tests_properties(session-static-share-exit session-static-borrow-exit PROPERTIES TIMEOUT 30)

    include(Catch)
    catch_discover_tests(can-tests)
    if(LINKERBOT_TRANSPORT_ENABLE_SERIAL)
        add_executable(serial-tests tests/main.cpp tests/test_serial.cpp)
        target_link_libraries(serial-tests PRIVATE linkerbot-serial linkerbot-can Catch2::Catch2)
        catch_discover_tests(serial-tests)
    endif()
    # GNU/ELF wrapping is isolated from the ordinary PTY and concurrency tests.
    # Production targets are static; wrappers do not affect the ABI fixture DLLs.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LINKERBOT_TRANSPORT_ENABLE_SERIAL)
        add_executable(linux-syscall-tests tests/main.cpp tests/test_linux_syscalls.cpp)
        target_link_libraries(linux-syscall-tests PRIVATE linkerbot-serial linkerbot-can Catch2::Catch2)
        foreach(call open eventfd flock ioctl poll read write close dlopen dladdr stat __xstat setsockopt socket bind)
            target_link_options(linux-syscall-tests PRIVATE "-Wl,--wrap=${call}")
        endforeach()
        add_dependencies(linux-syscall-tests pcan-fixture vendor-fixture)
        add_test(NAME linux-syscall-faults COMMAND ${CMAKE_COMMAND} -E env
            "LINKERBOT_TEST_VENDOR_LIBRARY=$<TARGET_FILE:vendor-fixture>"
            "LINKERBOT_TEST_PCAN_LIBRARY=$<TARGET_FILE:pcan-fixture>"
            $<TARGET_FILE:linux-syscall-tests>)
        set_tests_properties(linux-syscall-faults PROPERTIES TIMEOUT 30)
    endif()
    if(LINKERBOT_TRANSPORT_ENABLE_MODBUS)
        # Compile the optional libFuzzer entry point in ordinary test builds too,
        # so installed-header and full-source analysis cover it without libFuzzer.
        add_library(rtu-fuzz-entry OBJECT validation/fuzz-rtu.cpp)
        target_link_libraries(rtu-fuzz-entry PRIVATE linkerbot-modbus)
        add_executable(modbus-tests tests/main.cpp tests/test_modbus_codec.cpp tests/test_modbus_session.cpp)
        target_link_libraries(modbus-tests PRIVATE linkerbot-modbus Catch2::Catch2)
        catch_discover_tests(modbus-tests)
    endif()

    add_library(pcan-fixture SHARED tests/pcan_fixture.cpp)
    add_library(pcan-fixture-classic SHARED tests/pcan_fixture.cpp)
    target_compile_definitions(pcan-fixture-classic PRIVATE PCAN_FIXTURE_CLASSIC_ONLY)
    add_library(pcan-fixture-missing SHARED tests/pcan_fixture.cpp)
    target_compile_definitions(pcan-fixture-missing PRIVATE PCAN_FIXTURE_MISSING_READ)
    add_executable(pcan-tests tests/main.cpp tests/test_pcan_transport.cpp)
    target_link_libraries(pcan-tests PRIVATE linkerbot-can Catch2::Catch2)
    if(MSVC)
        target_compile_options(pcan-tests PRIVATE /utf-8)
    endif()
    add_dependencies(pcan-tests pcan-fixture pcan-fixture-classic pcan-fixture-missing)
    add_test(NAME pcan-native-loader COMMAND ${CMAKE_COMMAND} -E env
        "LINKERBOT_TEST_PCAN_LIBRARY=$<TARGET_FILE:pcan-fixture>"
        "LINKERBOT_TEST_PCAN_CLASSIC=$<TARGET_FILE:pcan-fixture-classic>"
        "LINKERBOT_TEST_PCAN_MISSING=$<TARGET_FILE:pcan-fixture-missing>"
        $<TARGET_FILE:pcan-tests>)
    set_tests_properties(pcan-native-loader PROPERTIES TIMEOUT 60)

    # Native ABI fixtures never enter install()/CPack. Windows fixtures use
    # single-devNum stdcall exports; Linux fixtures keep the dual-index ABI.
    add_library(vendor-fixture SHARED tests/vendor_canfd_fixture.cpp)
    add_library(vendor-fixture-missing SHARED tests/vendor_canfd_fixture.cpp)
    target_compile_definitions(vendor-fixture-missing PRIVATE FIXTURE_MISSING_RX)
    add_executable(vendor-loader-tests tests/main.cpp tests/test_vendor_canfd_loader.cpp)
    target_link_libraries(vendor-loader-tests PRIVATE linkerbot-can Catch2::Catch2)
    if(MSVC)
        target_compile_options(vendor-loader-tests PRIVATE /utf-8)
    endif()
    set(_vendor_test_env
        "LINKERBOT_TEST_VENDOR_LIBRARY=$<TARGET_FILE:vendor-fixture>"
        "LINKERBOT_TEST_VENDOR_MISSING=$<TARGET_FILE:vendor-fixture-missing>")
    add_dependencies(vendor-loader-tests vendor-fixture vendor-fixture-missing)
    if(WIN32)
        add_library(vendor-fixture-dependency SHARED tests/vendor_fixture_dependency.cpp)
        add_library(vendor-fixture-dependent SHARED tests/vendor_canfd_fixture.cpp)
        target_compile_definitions(vendor-fixture-dependent PRIVATE FIXTURE_DEPENDENT)
        target_link_libraries(vendor-fixture-dependent PRIVATE vendor-fixture-dependency)
        # Keep this dependency outside the test executable's application dir.
        # A copied DLL without its dependency must genuinely fail to load.
        set_target_properties(vendor-fixture-dependent vendor-fixture-dependency PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vendor-dependent")
        list(APPEND _vendor_test_env
            "LINKERBOT_TEST_VENDOR_DEPENDENT=$<TARGET_FILE:vendor-fixture-dependent>"
            "LINKERBOT_TEST_VENDOR_DEPENDENCY=$<TARGET_FILE:vendor-fixture-dependency>")
        add_dependencies(vendor-loader-tests vendor-fixture-dependent vendor-fixture-dependency)
    else()
        add_library(vendor-fixture-partial SHARED tests/vendor_canfd_fixture.cpp)
        target_compile_definitions(vendor-fixture-partial PRIVATE FIXTURE_PARTIAL)
        add_library(vendor-fixture-legacy SHARED tests/vendor_canfd_fixture.cpp)
        target_compile_definitions(vendor-fixture-legacy PRIVATE FIXTURE_LEGACY)
        list(APPEND _vendor_test_env
            "LINKERBOT_TEST_VENDOR_PARTIAL=$<TARGET_FILE:vendor-fixture-partial>"
            "LINKERBOT_TEST_VENDOR_LEGACY=$<TARGET_FILE:vendor-fixture-legacy>")
        add_dependencies(vendor-loader-tests vendor-fixture-partial vendor-fixture-legacy)
    endif()
    add_test(NAME vendor-native-loader
        COMMAND ${CMAKE_COMMAND} -E env ${_vendor_test_env} $<TARGET_FILE:vendor-loader-tests>)
    set_tests_properties(vendor-native-loader PROPERTIES TIMEOUT 30)
endif()

if(LINKERBOT_TRANSPORT_BUILD_EXAMPLES)
    add_executable(can-example examples/shared_bus.cpp)
    target_link_libraries(can-example PRIVATE linkerbot-can)
    if(NOT LINKERBOT_TRANSPORT_IS_TOP_LEVEL)
        set_target_properties(can-example PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()

linkerbot_transport_cache(LINKERBOT_TRANSPORT_BUILD_FUZZERS BOOL "Build the Clang RTU codec fuzzer" OFF)
if(LINKERBOT_TRANSPORT_BUILD_FUZZERS)
    if(NOT LINKERBOT_TRANSPORT_ENABLE_MODBUS OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR WIN32)
        message(FATAL_ERROR "RTU fuzzing requires Modbus and a Unix Clang toolchain")
    endif()
    add_executable(rtu-codec-fuzz validation/fuzz-rtu.cpp src/modbus/codec.cpp src/serial/common.cpp)
    target_link_libraries(rtu-codec-fuzz PRIVATE linkerbot::headers)
    target_compile_options(rtu-codec-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(rtu-codec-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(LINKERBOT_TRANSPORT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

set_target_properties(linkerbot-can PROPERTIES
    EXPORT_NAME can
    LINKERBOT_TRANSPORT_VERSION "${PROJECT_VERSION}"
    LINKERBOT_COMM_VERSION "${PROJECT_VERSION}"
    EXPORT_PROPERTIES "LINKERBOT_TRANSPORT_VERSION;LINKERBOT_COMM_VERSION")
set_target_properties(linkerbot-headers PROPERTIES EXPORT_NAME headers)
set_target_properties(linkerbot-can-compat PROPERTIES EXPORT_NAME can-compat)
install(TARGETS linkerbot-can linkerbot-headers linkerbot-can-compat ${_linkerbot_optional_targets}
    EXPORT linkerbot-transport-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES $<TARGET_FILE:linkerbot-can> DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RENAME "${CMAKE_STATIC_LIBRARY_PREFIX}linkerbot-comm${CMAKE_STATIC_LIBRARY_SUFFIX}")
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY compat/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linkerbot/compat)
install(EXPORT linkerbot-transport-targets NAMESPACE linkerbot::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-transport)
configure_package_config_file(cmake/linkerbot-transport-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-transport-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-transport)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/linkerbot-transport-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-transport-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-transport-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-transport)
install(FILES cmake/LegacyTargets.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-transport)
configure_package_config_file(cmake/linkerbot-comm-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-comm)
configure_file(cmake/linkerbot-comm-targets.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-targets.cmake @ONLY)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-config-version.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-targets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-comm)
install(FILES LICENSE NOTICE README.md README.zh-CN.md CHANGELOG.md CHANGELOG.en.md
    CONTRIBUTING.md CONTRIBUTING.zh-CN.md SECURITY.md SECURITY.zh-CN.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport)
install(FILES examples/shared_bus.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/examples)
# README images use relative paths in both source and installed documentation.
install(DIRECTORY .github/assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/.github/assets
    FILES_MATCHING PATTERN "*.png")
install(FILES .github/cppreportcard/badge.svg .github/cppreportcard/README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/.github/cppreportcard)
install(FILES examples/quickstart/CMakeLists.txt examples/quickstart/main.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/examples/quickstart)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/docs
    FILES_MATCHING PATTERN "*.md" PATTERN "*.csv" PATTERN "ci-performance-*.json")
install(FILES benchmarks/README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/benchmarks)
install(FILES benchmarks/micro/README.md benchmarks/micro/summarize.py
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/benchmarks/micro)
install(FILES benchmarks/stress/README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/benchmarks/stress)
install(DIRECTORY benchmarks/results/arm64-linux-2026-11-03
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/benchmarks/results
    FILES_MATCHING PATTERN "*.md" PATTERN "*.csv")

include(cmake/ReleasePackage.cmake)
install(FILES validation/installed-consumer/CMakeLists.txt validation/installed-consumer/main.cpp
    validation/installed-consumer/session_compat.cpp
    validation/installed-consumer/rtu.cpp
    validation/installed-consumer/allocation_failure.cpp
    validation/installed-consumer/expect-allocation-failure.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-transport/validation/installed-consumer)
if(LINKERBOT_TRANSPORT_IS_TOP_LEVEL)
    include(CPack)
endif()

# Keep relative documentation/example paths from earlier packages working.
# Both trees contain the current documentation; fixed-version evidence stays labelled.
configure_file(cmake/InstallLegacyShare.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/InstallLegacyShare.cmake @ONLY)
install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/InstallLegacyShare.cmake)
