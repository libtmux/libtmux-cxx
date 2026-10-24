# Operation costs that ride on top of dispatch: listing depth, decode, and
# control-mode notification throughput. Its own target for the same reason
# apps/matrix is: it starts a server and belongs in no test suite.
add_executable(libtmux_operations_bench operations.cpp)
target_link_libraries(
  libtmux_operations_bench PRIVATE libtmux::libtmux libtmux::testing)
# The registry the library refuses by, so the benchmark skips what it refuses.
target_include_directories(libtmux_operations_bench PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(libtmux_operations_bench PRIVATE cxx_std_23)
# Same warning strictness as everything else in this build; absent standalone,
# where a consumer supplies their own (see examples/CMakeLists.txt).
if(TARGET libtmux_build_options)
  target_link_libraries(libtmux_operations_bench PRIVATE libtmux_build_options)
endif()
