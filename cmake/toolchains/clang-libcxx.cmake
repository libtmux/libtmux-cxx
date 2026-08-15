set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-stdlib=libc++")

# Lets the build assert that this toolchain got the standard library it asked
# for. Only a build that chose this file makes that claim: Apple Clang brings
# its own libc++ at its own version and is not the pairing being pinned here.
add_compile_definitions(LIBTMUX_PINNED_LIBCXX)
