# The transport matrix: one workload, every way of reaching tmux, one table.
# Its own target because it starts a server per row and belongs in no suite.
add_executable(libtmux_matrix matrix.cpp)
target_link_libraries(libtmux_matrix PRIVATE libtmux::libtmux libtmux::testing)
# The registry the library refuses by, so the matrix skips what it refuses.
target_include_directories(libtmux_matrix PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(libtmux_matrix PRIVATE cxx_std_23)
# Same warning strictness as everything else in this build; absent standalone,
# where a consumer supplies their own (see examples/CMakeLists.txt).
if(TARGET libtmux_build_options)
  target_link_libraries(libtmux_matrix PRIVATE libtmux_build_options)
endif()
