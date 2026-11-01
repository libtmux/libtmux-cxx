// The `main` every tmux-facing test binary links, in place of
// GTest::gtest_main: the same body, behind a check that the process cannot
// reach a tmux server it does not own.

#include <gtest/gtest.h>

#include "support/sandbox.hpp"

int main(int argc, char** argv) {
  libtmux::test_support::require_sandbox();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
