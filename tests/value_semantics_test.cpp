// An entity is a value, and a failure is something a caller can say out loud.
//
// These are the six ordinary things a C++ caller does with a value the moment
// they have one — compare it, store it, key a map with it, print it, chain
// past a failure, and name that failure — and each of them is part of the
// surface that cannot change once the package is published.

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::CommandFailure;
using libtmux::FailureKind;
using libtmux::Pane;
using libtmux::Server;
using libtmux::Session;
using libtmux::Window;

// What "an entity is a value" has to keep meaning, checked where it is cheap
// to check: at compile time, on every build, rather than by remembering.
//
// A member that is const or a reference silently takes assignment away, and
// one that allocates takes the nothrow move away — which is what lets a
// vector of these grow without copying. Neither shows up in a behaviour test.
template <typename T> constexpr bool is_an_entity_value() {
  static_assert(std::copy_constructible<T>);
  static_assert(std::is_copy_assignable_v<T>);
  static_assert(std::is_nothrow_move_constructible_v<T>);
  static_assert(std::is_nothrow_move_assignable_v<T>);
  static_assert(std::equality_comparable<T>);
  static_assert(requires(const T& value) { std::hash<T>{}(value); });
  // Deliberately not default constructible, so `std::regular` does not hold:
  // an entity is a row of a snapshot, and there is no such thing as one that
  // is not. A default-constructed entity could only be a null to check for.
  static_assert(!std::is_default_constructible_v<T>);
  // The representation the entity bake-off chose: a shared snapshot and which
  // of its rows this is, and nothing else. Stated structurally so it stays a
  // claim about the design rather than a byte count that drifts.
  static_assert(sizeof(T) <= sizeof(std::shared_ptr<void>) + sizeof(std::size_t));
  return true;
}

static_assert(is_an_entity_value<Session>());
static_assert(is_an_entity_value<Window>());
static_assert(is_an_entity_value<Pane>());
static_assert(is_an_entity_value<libtmux::Client>());

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(ValueSemantics, TheSameObjectFromTwoListingsIsOneValue) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto first = server.sessions();
  auto second = server.sessions();
  ASSERT_TRUE(first.has_value() && second.has_value());
  ASSERT_EQ(first->size(), 1U);

  // Different listings, different snapshots, same tmux object.
  EXPECT_EQ(first->at(0), second->at(0));

  const auto created = first->at(0).new_window("valued");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  const auto renamed = created->rename("renamed");
  ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;
  const auto current = created->refresh();
  ASSERT_TRUE(current.has_value()) << current.error().diagnostic;

  // Equality is identity, not observation: a window that has been renamed is
  // still the window it was.
  EXPECT_EQ(*current, *created);
  EXPECT_NE(current->name(), created->name());
}

TEST(ValueSemantics, EntitiesKeyTheOrdinaryContainers) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value());
  ASSERT_TRUE(sessions->at(0).new_window("second").has_value());

  const auto windows = server.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_EQ(windows->size(), 2U);

  std::unordered_set<Window> unique{windows->begin(), windows->end()};
  EXPECT_EQ(unique.size(), 2U);

  // Listed again, the same windows land on the entries already there.
  const auto again = server.windows();
  ASSERT_TRUE(again.has_value()) << again.error().diagnostic;
  for (const Window& window : *again) {
    EXPECT_TRUE(unique.contains(window)) << window.id() << " hashed elsewhere";
  }

  std::unordered_map<Window, std::string> named;
  for (const Window& window : *windows) {
    named.emplace(window, std::string{window.name()});
  }
  EXPECT_EQ(named.at(again->at(0)), again->at(0).name());
}

TEST(ValueSemantics, AnEntityPrintsAsSomethingAReaderRecognises) {
  const auto recorded = libtmux::Snapshot::from_recording(
      Pane::kFields,
      "%4␞nvim␞1␞@2␞$1␞0␞editor␞991␞/dev/pts/7␞/tmp␞80␞24␞0␞0␞1␞0␞1␞1␞0␞\n");
  ASSERT_NE(recorded, nullptr);

  std::ostringstream out;
  out << Pane{recorded, 0};
  EXPECT_EQ(out.str(), "Pane(%4 nvim)");
}

TEST(ValueSemantics, AFailureComposesAndCanBeNamed) {
  // The factory reports what every other call reports, so the first two calls
  // anyone writes chain instead of failing to compile.
  const auto sessions = Server::at_socket_path("").and_then(
      [](const Server& server) { return server.sessions(); });
  ASSERT_FALSE(sessions.has_value());
  EXPECT_EQ(sessions.error().kind, FailureKind::validation);
  EXPECT_FALSE(sessions.error().dispatched);

  // Every failure a caller can be handed says what it is.
  for (const FailureKind kind :
       {FailureKind::validation, FailureKind::spawn, FailureKind::pre_exec,
        FailureKind::pipe, FailureKind::timeout, FailureKind::refused,
        FailureKind::missing, FailureKind::truncated}) {
    EXPECT_FALSE(libtmux::to_string(kind).empty());
  }
  EXPECT_FALSE(libtmux::to_string(libtmux::CardinalityError::several_matched).empty());
  EXPECT_FALSE(libtmux::to_string(libtmux::SocketError::path_too_long).empty());
}

} // namespace
