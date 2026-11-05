cmake_minimum_required(VERSION 3.16)
project(comm-installed-consumer LANGUAGES CXX)
find_package(linkerbot-comm 0.3 CONFIG REQUIRED)
add_executable(comm-installed-consumer main.cpp)
target_link_libraries(comm-installed-consumer PRIVATE linkerbot::comm-compat)
enable_testing()
add_test(NAME comm-installed-consumer COMMAND comm-installed-consumer)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(comm-consumer-allocation-failure main.cpp allocation_failure.cpp)
    target_link_libraries(comm-consumer-allocation-failure PRIVATE linkerbot::comm-compat)
    target_link_options(comm-consumer-allocation-failure PRIVATE
        -Wl,--wrap=main -Wl,--wrap=_Znwm)
    add_test(NAME comm-consumer-allocation-failure
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:comm-consumer-allocation-failure>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/expect-allocation-failure.cmake)
endif()
if(TARGET linkerbot::modbus)
    add_executable(comm-rtu-consumer rtu.cpp)
    target_link_libraries(comm-rtu-consumer PRIVATE linkerbot::modbus)
    add_test(NAME comm-rtu-consumer COMMAND comm-rtu-consumer)
endif()
