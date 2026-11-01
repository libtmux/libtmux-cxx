# The tmux environment every test runs in.
#
# A bare `tmux` command follows an inherited `TMUX` to whichever server the
# caller is inside, and otherwise lands on the default socket under
# `$TMUX_TMPDIR`. A suite started from a tmux pane would therefore drive, and
# a `kill-server` would end, the session it was started from.
#
# ctest reads each directory's TEST_INCLUDE_FILES when it starts and before it
# runs a test, discovery included. The file written here clears every variable
# that selects a server and points `TMUX_TMPDIR` at a directory of this build
# tree, in ctest's own environment, so each test process inherits it however
# the test was registered. A property on each test cannot do that for the
# cases `gtest_discover_tests` defines at ctest time, which flattens a
# list-valued property.
#
# A test that starts a server in that directory and forgets it leaves the
# server running after the suite. ctest runs CTEST_CUSTOM_PRE_TEST before it
# lists the tests and CTEST_CUSTOM_POST_TEST after the last one, from
# CTestCustom.cmake in the build tree, and a command that fails fails the run
# with status 8. The script written here finds every socket in the sandbox, not
# only the ones tmux names itself: a test may start a server on
# `-S <sandbox>/custom.sock`. Before the tests it refuses while a server
# answers; after them it stops each one with `kill-server -S`, checks that a
# connect then fails for up to about three seconds, and fails the run naming
# the sockets even when every test passed. A socket nothing answers on is not a
# leak and is removed; one that still answers is left where it is, and so is
# the sandbox. Nothing here deletes the sandbox directory.
# `ctest -N` runs both commands as well, so one ctest at a time per build tree.
#
# `libtmux_test_main` is the same promise from the other side. A test binary
# started by hand has not been through ctest, so its `main` refuses to run
# unless the environment is the one written here.

include_guard(GLOBAL)

# tmux appends `tmux-<uid>/default`, and a unix socket path holds 104 bytes on
# macOS and 108 on Linux.
set(LIBTMUX_TEST_SANDBOX_PATH_BUDGET 100)

# Writes the script ctest runs around the tests. `-DMODE=pre` refuses while a
# tmux server answers anywhere under ROOT; `-DMODE=post` stops each one, checks
# that it stopped, and fails. Every message names sockets, relative to ROOT, and
# nothing else.
function(libtmux_test_sandbox_leak_script output root)
  file(
    CONFIGURE
    OUTPUT "${output}"
    CONTENT [=[
find_program(tmux_program tmux)
if(NOT tmux_program)
  return()
endif()

# Whether a server answers on SOCKET. A connect that fails says so in one of
# these two ways; anything else, whether a reply, a protocol mismatch or no
# answer in time, means something is listening.
function(libtmux_sandbox_answers socket result)
  execute_process(
    COMMAND "${tmux_program}" -N -S "${socket}" list-sessions
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE reason TIMEOUT 5)
  if(status EQUAL 0 OR NOT reason MATCHES "error connecting to|no server running")
    set(${result} TRUE PARENT_SCOPE)
  else()
    set(${result} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Every socket in the tree, links not followed: `find -type s`.
execute_process(
  COMMAND find "@root@" -type s
  OUTPUT_VARIABLE found OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(REPLACE "\n" ";" sockets "${found}")
set(alive "")
foreach(socket IN LISTS sockets)
  libtmux_sandbox_answers("${socket}" up)
  if(up)
    list(APPEND alive "${socket}")
  else()
    file(REMOVE "${socket}")
  endif()
endforeach()
if(NOT alive)
  return()
endif()

if(MODE STREQUAL "pre")
  set(names "")
  foreach(socket IN LISTS alive)
    file(RELATIVE_PATH name "@root@" "${socket}")
    list(APPEND names "${name}")
  endforeach()
  list(JOIN names ", " names)
  message(FATAL_ERROR
          "tmux servers from an earlier run are alive on the sandbox: ${names}")
endif()

set(stopped "")
set(stuck "")
foreach(socket IN LISTS alive)
  file(RELATIVE_PATH name "@root@" "${socket}")
  execute_process(COMMAND "${tmux_program}" -S "${socket}" kill-server
                  OUTPUT_QUIET ERROR_QUIET TIMEOUT 5)
  foreach(attempt RANGE 60)
    libtmux_sandbox_answers("${socket}" up)
    if(NOT up)
      break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
  endforeach()
  if(up)
    list(APPEND stuck "${name}")
  else()
    list(APPEND stopped "${name}")
    file(REMOVE "${socket}")
  endif()
endforeach()
if(stuck)
  list(JOIN stuck ", " names)
  string(CONCAT text
         "tests left tmux servers running on the sandbox that did not stop, "
         "so the sandbox is left in place: ${names}")
  if(stopped)
    list(JOIN stopped ", " names)
    string(APPEND text "; now stopped: ${names}")
  endif()
  message(FATAL_ERROR "${text}")
endif()
list(JOIN stopped ", " names)
message(FATAL_ERROR
        "tests left tmux servers running on the sandbox, now stopped: ${names}")
]=]
    @ONLY)
endfunction()

# Writes the CTestCustom.cmake that runs SCRIPT before and after the tests.
function(libtmux_test_sandbox_custom_file output script)
  file(
    CONFIGURE
    OUTPUT "${output}"
    CONTENT [=[
set(CTEST_CUSTOM_PRE_TEST "\"@CMAKE_COMMAND@\" -DMODE=pre -P \"@script@\"")
set(CTEST_CUSTOM_POST_TEST "\"@CMAKE_COMMAND@\" -DMODE=post -P \"@script@\"")
]=]
    @ONLY)
endfunction()

# Call from the top-level directory, after enable_testing().
#
# Sets in the caller's scope LIBTMUX_TEST_SANDBOX_ROOT, the directory ctest
# gives every test as `TMUX_TMPDIR`, and LIBTMUX_TEST_SANDBOX_HOOK, the file
# that gives it.
function(libtmux_test_sandbox)
  set(root "${PROJECT_BINARY_DIR}/tmux-sandbox")
  string(LENGTH "${root}/tmux-4294967295/default" needed)
  if(needed GREATER LIBTMUX_TEST_SANDBOX_PATH_BUDGET)
    # A build tree too deep for a socket path gets a short directory under
    # the system temporary directory instead, one per build tree.
    string(MD5 digest "${PROJECT_BINARY_DIR}")
    string(SUBSTRING "${digest}" 0 8 digest)
    if(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
      set(parent "$ENV{TMPDIR}")
    else()
      set(parent "/tmp")
    endif()
    set(root "${parent}/libtmux-cxx-sbx-${digest}")
  endif()

  file(MAKE_DIRECTORY "${root}")
  file(CHMOD "${root}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

  set(hook "${PROJECT_BINARY_DIR}/LibtmuxTestSandbox.ctest.cmake")
  file(
    CONFIGURE
    OUTPUT "${hook}"
    CONTENT [=[
# Runs when ctest reads the test files, before the first test starts.
foreach(name TMUX TMUX_PANE LIBTMUX_SOCKET LIBTMUX_SOCKET_PATH)
  unset(ENV{${name}})
endforeach()
file(MAKE_DIRECTORY "@root@")
file(CHMOD "@root@" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
set(ENV{TMUX_TMPDIR} "@root@")
set(ENV{LIBTMUX_TEST_SANDBOX} "@root@")
]=]
    @ONLY)
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${hook}")

  set(script "${PROJECT_BINARY_DIR}/LibtmuxTestSandbox.leaks.cmake")
  libtmux_test_sandbox_leak_script("${script}" "${root}")
  libtmux_test_sandbox_custom_file("${PROJECT_BINARY_DIR}/CTestCustom.cmake"
                                   "${script}")

  set(LIBTMUX_TEST_SANDBOX_ROOT "${root}" PARENT_SCOPE)
  set(LIBTMUX_TEST_SANDBOX_HOOK "${hook}" PARENT_SCOPE)
endfunction()

# Call after GoogleTest is resolved. A test links this instead of
# GTest::gtest_main to get a `main` that refuses to run unsandboxed.
function(libtmux_add_test_main)
  add_library(libtmux_test_main STATIC
              "${PROJECT_SOURCE_DIR}/tests/support/test_main.cpp")
  target_include_directories(libtmux_test_main PUBLIC "${PROJECT_SOURCE_DIR}/tests")
  target_link_libraries(libtmux_test_main PUBLIC GTest::gtest
                        PRIVATE libtmux_build_options)
endfunction()
