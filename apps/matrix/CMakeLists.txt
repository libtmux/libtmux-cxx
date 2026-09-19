# The transport matrix: one workload, every way of reaching tmux, one table.
# Its own target because it starts a server per row and belongs in no suite.
add_executable(libtmux_matrix matrix.cpp)
target_link_libraries(libtmux_matrix PRIVATE libtmux::libtmux libtmux::testing)
target_compile_features(libtmux_matrix PRIVATE cxx_std_23)
# Same warning strictness as everything else in this build; absent standalone,
# where a consumer supplies their own (see examples/CMakeLists.txt).
if(TARGET libtmux_build_options)
  target_link_libraries(libtmux_matrix PRIVATE libtmux_build_options)
endif()
