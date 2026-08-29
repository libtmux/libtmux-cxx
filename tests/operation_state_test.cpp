#include <memory>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "move_only_function.hpp"

namespace {

using libtmux::detail::MoveOnlyFunction;

static_assert(!std::is_copy_constructible_v<MoveOnlyFunction<void()>>);
static_assert(std::is_nothrow_move_constructible_v<MoveOnlyFunction<void()>>);

TEST(MoveOnlyFunction, InvokesAndReleasesAMoveOnlyCapture) {
  auto value = std::make_unique<int>(41);
  MoveOnlyFunction<int(int)> function{
      [owned = std::move(value)](int addend) { return *owned + addend; }};

  ASSERT_TRUE(function);
  EXPECT_EQ(function(1), 42);

  MoveOnlyFunction<int(int)> moved{std::move(function)};
  EXPECT_FALSE(function);
  EXPECT_EQ(moved(2), 43);
  moved = {};
  EXPECT_FALSE(moved);
}

} // namespace
