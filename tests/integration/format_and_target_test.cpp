// Composing what tmux parses: format strings and targets.
//
// Both are string builders whose correctness is a fact about tmux, not about
// C++. `escape_literal` is right if tmux prints the text back unchanged, and
// `pane_target` is right if tmux resolves what it composed — so each case is
// asserted against a real server rather than against the builder's own idea of
// its output.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "libtmux/command.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/format.hpp"
#include "libtmux/server.hpp"
#include "libtmux/target.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::FormatBuilder;
using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// tmux is the only authority on whether a format was escaped correctly: it
// owns the parser. Expanding it and comparing is the whole test.
std::string expanded(const Server& server, const std::string& format) {
  const auto result = server.expand(format);
  EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().diagnostic);
  return result.has_value() ? *result : std::string{};
}

TEST(FormatComposition, LiteralTextSurvivesTmuxUnchanged) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Every one of these means something to tmux's format parser unescaped.
  for (const std::string_view literal :
       {"plain", "#1", "#{pane_id}", "#(echo run-me)", "##", "a # b #{x}"}) {
    const auto format = FormatBuilder{}.literal(literal).str();
    EXPECT_EQ(expanded(server, format), literal)
        << "literal " << literal << " built as " << format;
  }
}

TEST(FormatComposition, FieldExpandsToWhatTmuxKnows) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  ASSERT_FALSE(sessions->empty());

  const auto format = FormatBuilder{}.field("session_name").str();
  EXPECT_EQ(format, "#{session_name}");
  EXPECT_EQ(expanded(server, format), std::string{fixture->session_name()});
}

TEST(FormatComposition, MixesLiteralAndFieldWithoutEitherEatingTheOther) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto format = FormatBuilder{}
                          .literal("#{not-a-field} ")
                          .field("session_name")
                          .literal(" #done")
                          .str();
  EXPECT_EQ(expanded(server, format),
            "#{not-a-field} " + std::string{fixture->session_name()} + " #done");
}

TEST(TargetComposition, ResolvesToTheSameObjectTmuxDoes) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const std::string session{fixture->session_name()};

  const auto made = server.session(session);
  ASSERT_TRUE(made.has_value()) << made.error().diagnostic;
  const auto window = made->new_window({.name = "targeted"});
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;

  // Composed from names, and asked of tmux by that composition.
  const auto composed = libtmux::pane_target(session, "targeted", "0");
  ASSERT_TRUE(composed.has_value()) << libtmux::to_string(composed.error());
  EXPECT_EQ(*composed, session + ":targeted.0");

  const auto pane = server.pane(*composed);
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  EXPECT_TRUE(libtmux::is_pane_id(pane->id())) << pane->id();
}

TEST(TargetComposition, PassesAnIdStraightThroughWithoutASession) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto panes = server.panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const std::string id{panes->at(0).id()};

  // An id is already unambiguous, so neither name is consulted — which is what
  // lets a caller pass one through without knowing the session it is in.
  const auto composed = libtmux::pane_target("", "", id);
  ASSERT_TRUE(composed.has_value()) << libtmux::to_string(composed.error());
  EXPECT_EQ(*composed, id);

  const auto resolved = server.pane(*composed);
  ASSERT_TRUE(resolved.has_value()) << resolved.error().diagnostic;
  EXPECT_EQ(resolved->id(), id);
}

TEST(TargetComposition, RefusesANameTmuxWouldMisread) {
  // A colon separates session from window and a dot separates window from
  // pane, so a name holding one cannot be composed into a target at all.
  const auto colon = libtmux::pane_target("has:colon", "w", "0");
  ASSERT_FALSE(colon.has_value());
  EXPECT_FALSE(libtmux::to_string(colon.error()).empty());

  const auto empty = libtmux::pane_target("", "w", "0");
  EXPECT_FALSE(empty.has_value());
}

// These answer "is it written as an id", which is what decides whether the
// separator validation applies — not "would tmux resolve it".
TEST(TargetComposition, TellsIdsApartByTheirPrefix) {
  EXPECT_TRUE(libtmux::is_pane_id("%0"));
  EXPECT_TRUE(libtmux::is_window_id("@12"));
  EXPECT_TRUE(libtmux::is_session_id("$3"));

  EXPECT_FALSE(libtmux::is_pane_id("@0"));
  EXPECT_FALSE(libtmux::is_pane_id("%"));
  EXPECT_FALSE(libtmux::is_pane_id(""));
  EXPECT_FALSE(libtmux::is_window_id("0"));

  // Deliberately loose: `%x` carries no separator, so it passes through
  // composition and tmux refuses it as the pane it is not.
  EXPECT_TRUE(libtmux::is_pane_id("%x"));
  const auto composed = libtmux::pane_target("session", "window", "%x");
  ASSERT_TRUE(composed.has_value());
  EXPECT_EQ(*composed, "%x");
}

} // namespace
