cmake_minimum_required(VERSION 3.25)
project(libtmux_consume LANGUAGES CXX)

# Everything an outside project needs: find the package, link the alias.
find_package(libtmux REQUIRED)

add_executable(consume main.cpp)
target_link_libraries(consume PRIVATE libtmux::libtmux)
