// The same window shown in more than one session.

#include <algorithm>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/testing/scoped_server.hpp"

namespace {

using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(WindowLinking, AWindowIsShownInTwoSessionsAtOnce) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto home = server.session(fixture->session_name());
  ASSERT_TRUE(home.has_value()) << home.error().diagnostic;
  const auto elsewhere = server.new_session("elsewhere");
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().diagnostic;
  const auto shared = home->active_window();
  ASSERT_TRUE(shared.has_value()) << shared.error().diagnostic;
  ASSERT_EQ(shared->linked_sessions(), 1);

  ASSERT_TRUE(shared->link_to(*elsewhere).has_value());

  // One window, two places: the id is the same on both sides, and tmux
  // counts the links rather than there being a copy.
  const auto counted = shared->refresh();
  ASSERT_TRUE(counted.has_value()) << counted.error().diagnostic;
  EXPECT_EQ(counted->linked_sessions(), 2);
  const auto over_there = elsewhere->windows();
  ASSERT_TRUE(over_there.has_value()) << over_there.error().diagnostic;
  EXPECT_TRUE(std::ranges::any_of(*over_there, [&](const libtmux::Window& one) {
    return one.id() == shared->id();
  }));
}

TEST(WindowLinking, UnlinkRemovesTheLinkTheValueNamesAndRefusesTheLast) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto home = server.session(fixture->session_name());
  ASSERT_TRUE(home.has_value()) << home.error().diagnostic;
  const auto elsewhere = server.new_session("elsewhere");
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().diagnostic;
  const auto shared = home->active_window();
  ASSERT_TRUE(shared.has_value()) << shared.error().diagnostic;
  ASSERT_TRUE(shared->link_to(*elsewhere).has_value());

  // A second window in `home`, because unlinking a session's last one
  // destroys the session: tmux keeps no session without windows, and the
  // test would then be measuring that instead.
  libtmux::NewWindowOptions spare;
  spare.name = "spare";
  ASSERT_TRUE(home->new_window(spare).has_value());

  // Unlink through the value that came from `home`, while the session tmux
  // would fall back to is the other one. A bare id lets tmux choose, and it
  // chooses the wrong link here — which is why the target is qualified.
  ASSERT_TRUE(shared->unlink().has_value());

  const auto left_home = home->windows();
  ASSERT_TRUE(left_home.has_value()) << left_home.error().diagnostic;
  EXPECT_FALSE(std::ranges::any_of(*left_home, [&](const libtmux::Window& one) {
    return one.id() == shared->id();
  })) << "the link named by the value should be the one removed";

  const auto kept = elsewhere->windows();
  ASSERT_TRUE(kept.has_value()) << kept.error().diagnostic;
  const auto survivor = std::ranges::find_if(
      *kept, [&](const libtmux::Window& one) { return one.id() == shared->id(); });
  ASSERT_NE(survivor, kept->end()) << "the other session should still hold it";
  EXPECT_EQ(survivor->linked_sessions(), 1);

  // tmux refuses to leave a window no session holds. Killing is the way to
  // be rid of it, and it says so.
  const auto refused = survivor->unlink();
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().kind, libtmux::FailureKind::refused);
  EXPECT_FALSE(refused.error().diagnostic.empty());
}

// A format expanded against a linked window answers about the session the
// value came from.
//
// `unlink` and `move_to` already carried the session for this reason. `expand`
// and `show_message` did not: they addressed the window by bare id, which
// leaves tmux to pick which of the links supplies the session-relative half of
// the format context. The picked one is not the one the caller is holding, so
// `#{session_name}` answered about a session they never mentioned.
TEST(WindowLinking, AFormatAnswersAboutTheSessionTheValueCameFrom) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto home = server.session(fixture->session_name());
  ASSERT_TRUE(home.has_value()) << home.error().diagnostic;
  const auto elsewhere = server.new_session("elsewhere");
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().diagnostic;
  const auto shared = home->active_window();
  ASSERT_TRUE(shared.has_value()) << shared.error().diagnostic;
  ASSERT_TRUE(shared->link_to(*elsewhere).has_value());

  // The same window, read once through each session. One id, two values, and
  // each has to answer about its own side of the link.
  const auto from_home = home->windows();
  ASSERT_TRUE(from_home.has_value()) << from_home.error().diagnostic;
  const auto here = std::ranges::find_if(
      *from_home, [&](const libtmux::Window& one) { return one.id() == shared->id(); });
  ASSERT_NE(here, from_home->end());

  const auto from_elsewhere = elsewhere->windows();
  ASSERT_TRUE(from_elsewhere.has_value()) << from_elsewhere.error().diagnostic;
  const auto there =
      std::ranges::find_if(*from_elsewhere, [&](const libtmux::Window& one) {
        return one.id() == shared->id();
      });
  ASSERT_NE(there, from_elsewhere->end());

  const auto named_here = here->expand("#{session_name}");
  ASSERT_TRUE(named_here.has_value()) << named_here.error().diagnostic;
  EXPECT_EQ(*named_here, fixture->session_name());

  const auto named_there = there->expand("#{session_name}");
  ASSERT_TRUE(named_there.has_value()) << named_there.error().diagnostic;
  EXPECT_EQ(*named_there, "elsewhere");

  // Different answers from the same window id is the whole point.
  EXPECT_NE(*named_here, *named_there);

  // And the message goes to the named link rather than to tmux's choice.
  EXPECT_TRUE(here->show_message("from home").has_value());
  EXPECT_TRUE(there->show_message("from elsewhere").has_value());
}

} // namespace
