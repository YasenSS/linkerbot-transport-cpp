cmake_minimum_required(VERSION 3.16)
project(transport-installed-consumer LANGUAGES CXX)
find_package(linkerbot-transport 0.3 CONFIG REQUIRED)
add_executable(transport-installed-consumer main.cpp)
target_link_libraries(transport-installed-consumer PRIVATE linkerbot::can-compat)
enable_testing()
add_test(NAME transport-installed-consumer COMMAND transport-installed-consumer)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(transport-consumer-allocation-failure main.cpp allocation_failure.cpp)
    target_link_libraries(transport-consumer-allocation-failure PRIVATE linkerbot::can-compat)
    target_link_options(transport-consumer-allocation-failure PRIVATE
        -Wl,--wrap=main -Wl,--wrap=_Znwm)
    add_test(NAME transport-consumer-allocation-failure
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:transport-consumer-allocation-failure>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/expect-allocation-failure.cmake)
endif()
if(TARGET linkerbot::modbus)
    add_executable(transport-rtu-consumer rtu.cpp)
    target_link_libraries(transport-rtu-consumer PRIVATE linkerbot::modbus)
    add_test(NAME transport-rtu-consumer COMMAND transport-rtu-consumer)
endif()
