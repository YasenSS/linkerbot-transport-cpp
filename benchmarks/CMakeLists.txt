cmake_minimum_required(VERSION 3.16)
project(linkerbot-perf LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "These process CPU/context-switch measurements require Linux")
endif()
if(NOT TARGET linkerbot::can)
    # This directory already supplies the benchmark target. Do not re-enter it
    # when configuring the library below, even if the parent option was set.
    set(LINKERBOT_TRANSPORT_BUILD_BENCHMARKS OFF)
    set(LINKERBOT_TRANSPORT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LINKERBOT_TRANSPORT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LINKERBOT_ALLOW_DEPENDENCY_DOWNLOADS OFF CACHE BOOL "" FORCE)
    add_subdirectory(.. transport)
endif()
add_executable(linkerbot-perf micro/main.cpp)
target_link_libraries(linkerbot-perf PRIVATE linkerbot::can)
target_compile_features(linkerbot-perf PRIVATE cxx_std_17)
option(LINKERBOT_PERF_COUNT_ALLOCATIONS "Instrument benchmark C++ allocations" OFF)
target_compile_definitions(linkerbot-perf PRIVATE
    LINKERBOT_PERF_COUNT_ALLOCATIONS=$<BOOL:${LINKERBOT_PERF_COUNT_ALLOCATIONS}>)

# Optional convenience target; the stress runner normally builds this separately
# in a fresh temporary directory against the selected local source tree.
option(LINKERBOT_PERF_BUILD_STRESS "Build end-to-end stress workers" OFF)
if(LINKERBOT_PERF_BUILD_STRESS)
    add_subdirectory(stress)
endif()
