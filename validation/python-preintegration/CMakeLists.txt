cmake_minimum_required(VERSION 3.19)
project(transport-python-preintegration LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(linkerbot-transport 0.3 CONFIG REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
Python3_add_library(_transport_probe MODULE probe.cpp)
target_link_libraries(_transport_probe PRIVATE linkerbot::can)
target_compile_features(_transport_probe PRIVATE cxx_std_17)
enable_testing()
add_test(NAME python-preintegration
    COMMAND ${CMAKE_COMMAND} -E env
        "PYTHONPATH=$<TARGET_FILE_DIR:_transport_probe>"
        "PYTHONDONTWRITEBYTECODE=1"
        "PROOF_NATIVE_ARCHIVE=$<TARGET_FILE:linkerbot::can>"
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_probe.py
        --report ${CMAKE_CURRENT_BINARY_DIR}/evidence.json)
set_tests_properties(python-preintegration PROPERTIES TIMEOUT 180)

add_test(NAME deployment-evidence-gate
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONDONTWRITEBYTECODE=1"
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_deployment.py)
set_tests_properties(deployment-evidence-gate PROPERTIES TIMEOUT 30)
