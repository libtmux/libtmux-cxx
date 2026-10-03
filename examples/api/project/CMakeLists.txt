cmake_minimum_required(VERSION 3.25)
project(libtmux_api_example LANGUAGES CXX)

find_package(libtmux REQUIRED COMPONENTS testing)

add_executable(api_example main.cpp)
target_link_libraries(api_example PRIVATE libtmux::libtmux libtmux::testing)
