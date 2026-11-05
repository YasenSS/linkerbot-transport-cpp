cmake_minimum_required(VERSION 3.16)
project(linkerbot-quickstart LANGUAGES CXX)

find_package(linkerbot-transport 0.3 CONFIG REQUIRED)
add_executable(linkerbot-quickstart main.cpp)
target_link_libraries(linkerbot-quickstart PRIVATE linkerbot::can)

enable_testing()
add_test(NAME linkerbot-quickstart COMMAND linkerbot-quickstart)
set_tests_properties(linkerbot-quickstart PROPERTIES TIMEOUT 10)
