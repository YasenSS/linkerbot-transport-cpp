cmake_minimum_required(VERSION 3.16)
project(linkerbot-comm VERSION 0.3.0 LANGUAGES CXX)
if(MSVC)
    add_compile_options(/utf-8)
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Communication layer: link connection, shared-bus fan-out, frame I/O.
# Consumers (linkerbot-hand-cpp and friends) link this target and get
# `include/` on their include path automatically.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(LINKERBOT_COMM_IS_TOP_LEVEL ON)
else()
    set(LINKERBOT_COMM_IS_TOP_LEVEL OFF)
endif()

option(LINKERBOT_COMM_BUILD_TESTS "Build Catch2 unit tests" ${LINKERBOT_COMM_IS_TOP_LEVEL})
option(LINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS "Allow missing test dependencies to be fetched" ON)
option(LINKERBOT_COMM_BUILD_EXAMPLES "Build example programs" ${LINKERBOT_COMM_IS_TOP_LEVEL})
option(LINKERBOT_COMM_ENABLE_COVERAGE "Instrument Linux tests for gcovr line coverage" OFF)

if(LINKERBOT_COMM_ENABLE_COVERAGE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "Coverage requires Linux and GCC")
    endif()
    if(NOT LINKERBOT_COMM_BUILD_TESTS)
        message(FATAL_ERROR "Coverage requires LINKERBOT_COMM_BUILD_TESTS=ON")
    endif()
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(LINKERBOT_COMM_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(linkerbot-comm-values INTERFACE)
add_library(linkerbot::comm-values ALIAS linkerbot-comm-values)
target_include_directories(linkerbot-comm-values INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(linkerbot-comm-values INTERFACE cxx_std_17)

add_library(linkerbot-comm STATIC
    src/vendor_canfd_transport.cpp
    src/vendor_canfd_loader.cpp
    src/session.cpp
    src/pcan_transport.cpp
    src/pcan_loader.cpp
    src/slcan_transport.cpp
    src/slcan_serial.cpp
)
add_library(linkerbot::comm ALIAS linkerbot-comm)

target_compile_features(linkerbot-comm PUBLIC cxx_std_17)
target_include_directories(linkerbot-comm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(linkerbot-comm PUBLIC Threads::Threads)
if(CMAKE_DL_LIBS)
    target_link_libraries(linkerbot-comm PRIVATE ${CMAKE_DL_LIBS})
endif()

# Flat-name compatibility headers (`#include "can_transport.hpp"`), for
# consumers migrating from the in-tree src/ layout. Link this instead of
# `linkerbot-comm` to get both paths; drop it once includes are updated.
add_library(linkerbot-comm-compat INTERFACE)
add_library(linkerbot::comm-compat ALIAS linkerbot-comm-compat)
target_link_libraries(linkerbot-comm-compat INTERFACE linkerbot-comm)
target_include_directories(linkerbot-comm-compat
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/compat>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/linkerbot/compat>
)

if(LINKERBOT_COMM_BUILD_TESTS)
    if(CMAKE_VERSION VERSION_LESS 3.19)
        message(FATAL_ERROR "Tests require CMake 3.19+ for Catch2 discovery; use LINKERBOT_COMM_BUILD_TESTS=OFF for the CMake 3.16 library-only build")
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

    add_executable(comm-tests
        tests/main.cpp
        tests/test_dispatcher.cpp
        tests/test_session.cpp
        tests/test_fault_contracts.cpp
        tests/test_fake_transport.cpp
        tests/test_frame.cpp
        tests/test_managed_bus.cpp
        tests/test_socketcan_lifecycle.cpp
        tests/test_vendor_canfd_transport.cpp
        tests/test_slcan_transport.cpp
        tests/test_slcan_serial_linux.cpp
    )
    target_link_libraries(comm-tests PRIVATE linkerbot-comm Catch2::Catch2)
    if(NOT LINKERBOT_COMM_IS_TOP_LEVEL)
        set_target_properties(comm-tests PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

    add_executable(session-static-exit tests/session_static_exit.cpp)
    target_link_libraries(session-static-exit PRIVATE linkerbot-comm)
    if(NOT LINKERBOT_COMM_IS_TOP_LEVEL)
        set_target_properties(session-static-exit PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
    add_test(NAME session-static-share-exit COMMAND session-static-exit shared)
    add_test(NAME session-static-borrow-exit COMMAND session-static-exit borrowed)
    set_tests_properties(session-static-share-exit session-static-borrow-exit PROPERTIES TIMEOUT 30)

    include(Catch)
    catch_discover_tests(comm-tests)

    add_library(pcan-fixture SHARED tests/pcan_fixture.cpp)
    add_library(pcan-fixture-classic SHARED tests/pcan_fixture.cpp)
    target_compile_definitions(pcan-fixture-classic PRIVATE PCAN_FIXTURE_CLASSIC_ONLY)
    add_library(pcan-fixture-missing SHARED tests/pcan_fixture.cpp)
    target_compile_definitions(pcan-fixture-missing PRIVATE PCAN_FIXTURE_MISSING_READ)
    add_executable(pcan-tests tests/main.cpp tests/test_pcan_transport.cpp)
    target_link_libraries(pcan-tests PRIVATE linkerbot-comm Catch2::Catch2)
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
    target_link_libraries(vendor-loader-tests PRIVATE linkerbot-comm Catch2::Catch2)
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

if(LINKERBOT_COMM_BUILD_EXAMPLES)
    add_executable(comm-example examples/shared_bus.cpp)
    target_link_libraries(comm-example PRIVATE linkerbot-comm)
    if(NOT LINKERBOT_COMM_IS_TOP_LEVEL)
        set_target_properties(comm-example PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()

set_target_properties(linkerbot-comm PROPERTIES
    EXPORT_NAME comm
    LINKERBOT_COMM_VERSION "${PROJECT_VERSION}"
    EXPORT_PROPERTIES LINKERBOT_COMM_VERSION)
set_target_properties(linkerbot-comm-values PROPERTIES EXPORT_NAME comm-values)
set_target_properties(linkerbot-comm-compat PROPERTIES EXPORT_NAME comm-compat)
install(TARGETS linkerbot-comm linkerbot-comm-values linkerbot-comm-compat
    EXPORT linkerbot-comm-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY compat/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/linkerbot/compat)
install(EXPORT linkerbot-comm-targets NAMESPACE linkerbot::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-comm)
configure_package_config_file(cmake/linkerbot-comm-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-comm)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/linkerbot-comm-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/linkerbot-comm)
install(FILES LICENSE NOTICE README.md README.zh-CN.md CHANGELOG.md CONTRIBUTING.md SECURITY.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-comm)
install(FILES examples/shared_bus.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-comm/examples)
install(FILES examples/quickstart/CMakeLists.txt examples/quickstart/main.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-comm/examples/quickstart)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-comm/docs
    FILES_MATCHING PATTERN "*.md")

include(cmake/ReleasePackage.cmake)
install(FILES validation/installed-consumer/CMakeLists.txt validation/installed-consumer/main.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/linkerbot-comm/validation/installed-consumer)
if(LINKERBOT_COMM_IS_TOP_LEVEL)
    include(CPack)
endif()
