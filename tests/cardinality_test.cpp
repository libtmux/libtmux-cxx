// Asking for one entity, and being told which way it went wrong.
//
// `first` and `exactly_one` differ in what they say about extras: one
// tolerates them, the other calls them the caller's mistake. That difference
// is the reason both exist, so each outcome is asserted here rather than
// inferred from the success path.

#include "libtmux/cardinality.hpp"

#include <ranges>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/filter_expr.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::CardinalityError;
using libtmux::exactly_one;
using libtmux::first;
using libtmux::matching;
using libtmux::Server;
using libtmux::Window;
namespace window = libtmux::window;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// Three windows, two of which share a name, so one range can supply all three
// cardinalities without rebuilding a server per case.
std::vector<Window> three_windows(const Server& server, std::string_view session) {
  auto found = server.session(std::string{session});
  EXPECT_TRUE(found.has_value());
  for (const auto* name : {"twin", "twin", "lone"}) {
    libtmux::NewWindowOptions options;
    options.name = name;
    EXPECT_TRUE(found->new_window(options).has_value());
  }
  auto windows = found->windows();
  EXPECT_TRUE(windows.has_value());
  return windows.has_value() ? *windows : std::vector<Window>{};
}

TEST(Cardinality, NothingMatchedAndSeveralMatchedAreDifferentAnswers) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto windows = three_windows(server, fixture->session_name());
  ASSERT_GE(windows.size(), 3U);

  auto absent = windows | matching(window::name == "no-such-window");
  const auto nothing = exactly_one(absent);
  ASSERT_FALSE(nothing.has_value());
  EXPECT_EQ(nothing.error(), CardinalityError::none_matched);

  auto twins = windows | matching(window::name == "twin");
  const auto several = exactly_one(twins);
  ASSERT_FALSE(several.has_value());
  EXPECT_EQ(several.error(), CardinalityError::several_matched);

  auto lone = windows | matching(window::name == "lone");
  const auto one = exactly_one(lone);
  ASSERT_TRUE(one.has_value())
      << "expected exactly one, got " << libtmux::to_string(one.error());
  EXPECT_EQ(one->get().name(), "lone");
}

TEST(Cardinality, FirstToleratesTheExtrasExactlyOneRefuses) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto windows = three_windows(server, fixture->session_name());
  ASSERT_GE(windows.size(), 3U);

  // The same range that makes exactly_one report several is an ordinary
  // answer here: `first` is how a caller says extras are acceptable.
  auto twins = windows | matching(window::name == "twin");
  const auto found = first(twins);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->get().name(), "twin");
  ASSERT_FALSE(exactly_one(twins).has_value());

  auto absent = windows | matching(window::name == "no-such-window");
  EXPECT_FALSE(first(absent).has_value());
}

TEST(Cardinality, TheAnswerRefersIntoTheRangeRatherThanCopying) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto windows = three_windows(server, fixture->session_name());
  ASSERT_FALSE(windows.empty());

  // Both hand back a reference to an element the caller still owns. Comparing
  // addresses is the only way to tell that from a copy that happens to be
  // equal, and a copy would be a lifetime the caller was never told about.
  auto lone = windows | matching(window::name == "lone");
  const auto referenced = first(lone);
  ASSERT_TRUE(referenced.has_value());
  const auto only = exactly_one(lone);
  ASSERT_TRUE(only.has_value());
  EXPECT_EQ(std::addressof(referenced->get()), std::addressof(only->get()));

  // Named, because `begin` on a temporary view is deleted — the same refusal
  // the cardinality helpers make, one layer down.
  auto again = windows | matching(window::name == "lone");
  EXPECT_EQ(std::addressof(referenced->get()),
            std::addressof(*std::ranges::begin(again)));
}

TEST(Cardinality, EveryErrorSaysWhichOneItWas) {
  EXPECT_EQ(libtmux::to_string(CardinalityError::none_matched), "nothing matched");
  EXPECT_EQ(libtmux::to_string(CardinalityError::several_matched),
            "several matched where one was required");
}

TEST(Cardinality, OwnedSelectionOutlivesTheListedSnapshotRange) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const auto selected = [&] {
    auto windows = three_windows(server, fixture->session_name());
    return libtmux::exactly_one_owned(windows | matching(window::name == "lone"));
  }();
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->name(), "lone");
  EXPECT_TRUE(selected->refresh().has_value());
}

TEST(Cardinality, OwnedSelectionCopiesBorrowedElementsAndAcceptsTemporaries) {
  std::vector<std::string> words{"retained"};
  auto copied = libtmux::first_owned(words | std::views::all);
  ASSERT_TRUE(copied.has_value());
  *copied = "changed";
  EXPECT_EQ(words.front(), "retained");

  const auto temporary = libtmux::exactly_one_owned(std::vector<std::string>{"owned"});
  ASSERT_TRUE(temporary.has_value());
  EXPECT_EQ(*temporary, "owned");
  EXPECT_FALSE(libtmux::first_owned(std::vector<int>{}).has_value());
  EXPECT_EQ(libtmux::exactly_one_owned(std::vector<int>{}).error(),
            CardinalityError::none_matched);
  EXPECT_EQ(libtmux::exactly_one_owned(std::vector<int>{1, 2}).error(),
            CardinalityError::several_matched);
}

TEST(Cardinality, OwnedSelectionMaterializesBeforeAdvancingSinglePassRanges) {
  std::istringstream input{"42"};
  const auto selected =
      libtmux::exactly_one_owned(std::ranges::istream_view<int>{input});
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, 42);

  const auto produced = libtmux::first_owned(
      std::views::iota(7, 9) |
      std::views::transform([](int value) { return std::make_unique<int>(value); }));
  ASSERT_TRUE(produced.has_value());
  EXPECT_EQ(**produced, 7);
}

TEST(Cardinality, OwnedSelectionMovesOnlyWhenTheIteratorYieldsOwnership) {
  std::vector<std::unique_ptr<int>> values;
  values.push_back(std::make_unique<int>(19));
  auto moving = std::ranges::subrange{std::make_move_iterator(values.begin()),
                                      std::make_move_iterator(values.end())};
  const auto selected = libtmux::exactly_one_owned(moving);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(**selected, 19);
  EXPECT_EQ(values.front(), nullptr);
}

} // namespace
