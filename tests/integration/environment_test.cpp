// The environment a new session, window or pane starts with.
//
// Asserted against the process rather than against tmux's own answer:
// `show-environment` reports what tmux intends to pass down, which is not
// the same claim as the program having it.

#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/platform.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// What the process in this pane was actually started with, read from the
// kernel rather than from tmux.
std::string environment_of(const libtmux::Pane& pane, std::string_view name) {
  const auto pid = pane.expand("#{pane_pid}");
  EXPECT_TRUE(pid.has_value());
  if (!pid.has_value()) {
    return {};
  }
  std::ifstream reading{"/proc/" + *pid + "/environ", std::ios::binary};
  const std::string blob{std::istreambuf_iterator<char>{reading},
                         std::istreambuf_iterator<char>{}};
  const std::string wanted = std::string{name} + "=";
  for (std::size_t start = 0; start < blob.size();) {
    const std::size_t end = blob.find('\0', start);
    const std::string_view entry{
        blob.data() + start, (end == std::string::npos ? blob.size() : end) - start};
    if (entry.starts_with(wanted)) {
      return std::string{entry.substr(wanted.size())};
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return {};
}

TEST(Environment, ASessionPassesItsVariablesToEveryPane) {
  LIBTMUX_SKIP_WITHOUT_PROCFS("a pane process's real environment");

  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  libtmux::NewSessionOptions options;
  options.name = "with-environment";
  options.shell_command = "sh -c 'sleep 60'";
  options.environment = {{"SPIKE_SESSION", "from the session"}};
  const auto session = server.new_session(options);
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  EXPECT_EQ(environment_of(*pane, "SPIKE_SESSION"), "from the session");

  // Down to a pane made later, too: this is session state, not an argument
  // that applied once.
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto later = window->split();
  ASSERT_TRUE(later.has_value()) << later.error().diagnostic;
  EXPECT_EQ(environment_of(*later, "SPIKE_SESSION"), "from the session");
}

TEST(Environment, AWindowAndAPaneCarryTheirOwnAndNotEachOthers) {
  LIBTMUX_SKIP_WITHOUT_PROCFS("a pane process's real environment");

  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  libtmux::NewWindowOptions window_options;
  window_options.name = "carrying";
  window_options.shell_command = "sh -c 'sleep 60'";
  window_options.environment = {{"SPIKE_WINDOW", "from the window"}};
  const auto window = session->new_window(window_options);
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto first = window->panes();
  ASSERT_TRUE(first.has_value()) << first.error().diagnostic;
  ASSERT_FALSE(first->empty());
  EXPECT_EQ(environment_of(first->front(), "SPIKE_WINDOW"), "from the window");

  libtmux::SplitOptions split_options;
  split_options.shell_command = "sh -c 'sleep 60'";
  split_options.environment = {{"SPIKE_PANE", "from the pane"}};
  const auto pane = window->split(split_options);
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  EXPECT_EQ(environment_of(*pane, "SPIKE_PANE"), "from the pane");

  // Neither leaks into the other: a window variable is not a session one,
  // and the pane's is its own.
  EXPECT_EQ(environment_of(first->front(), "SPIKE_PANE"), "");
  const auto elsewhere = session->active_window();
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().diagnostic;
  const auto untouched = elsewhere->panes();
  ASSERT_TRUE(untouched.has_value()) << untouched.error().diagnostic;
  ASSERT_FALSE(untouched->empty());
  EXPECT_EQ(environment_of(untouched->front(), "SPIKE_WINDOW"), "");
}

TEST(Environment, SeveralVariablesAllArriveAndAnEmptyValueIsAValue) {
  LIBTMUX_SKIP_WITHOUT_PROCFS("a pane process's real environment");

  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  libtmux::NewWindowOptions options;
  options.name = "several";
  options.shell_command = "sh -c 'sleep 60'";
  options.environment = {
      {"SPIKE_A", "one"}, {"SPIKE_B", "two words"}, {"SPIKE_EMPTY", ""}};
  const auto window = session->new_window(options);
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());

  EXPECT_EQ(environment_of(panes->front(), "SPIKE_A"), "one");
  EXPECT_EQ(environment_of(panes->front(), "SPIKE_B"), "two words");
  // Set to empty rather than absent, which is what tmux does and is a thing
  // a caller can mean.
  EXPECT_EQ(environment_of(panes->front(), "SPIKE_EMPTY"), "");
}

TEST(Environment, ANameTmuxWouldSilentlyIgnoreIsRefused) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  // tmux takes a `-e` argument holding no `=` with a zero exit status and
  // creates nothing, so a name that would produce one is caught here.
  for (const auto& bad : std::vector<std::pair<std::string, std::string>>{
           {"", "value"}, {"HAS=EQUALS", "value"}}) {
    libtmux::NewWindowOptions options;
    options.name = "refused";
    options.environment = {bad};
    const auto refused = session->new_window(options);
    ASSERT_FALSE(refused.has_value()) << "accepted " << bad.first;
    EXPECT_EQ(refused.error().kind, libtmux::FailureKind::validation);
    EXPECT_FALSE(refused.error().dispatched);
  }

  // Nothing was made while proving it.
  const auto windows = session->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  for (const libtmux::Window& window : *windows) {
    EXPECT_NE(window.name(), "refused");
  }
}

TEST(Creation, AnEntityIsReadableWhenTheCallCarriesAShellCommand) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  // Nothing to do with the environment: a shell command puts a `--` in the
  // request, and the format asking for the new entity's fields has to go in
  // front of it. Behind it, tmux hands the format to the program being
  // started and answers in its default shape, so the entity comes back
  // unreadable while the window is created regardless.
  libtmux::NewWindowOptions window_options;
  window_options.name = "running";
  window_options.shell_command = "sh -c 'sleep 60'";
  const auto window = session->new_window(window_options);
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  EXPECT_EQ(window->name(), "running");
  EXPECT_FALSE(window->id().empty());

  libtmux::SplitOptions split_options;
  split_options.shell_command = "sh -c 'sleep 60'";
  const auto pane = window->split(split_options);
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  EXPECT_TRUE(pane->id().starts_with("%"));

  libtmux::NewSessionOptions session_options;
  session_options.name = "also-running";
  session_options.shell_command = "sh -c 'sleep 60'";
  const auto made = server.new_session(session_options);
  ASSERT_TRUE(made.has_value()) << made.error().diagnostic;
  EXPECT_EQ(made->name(), "also-running");
  EXPECT_TRUE(made->id().starts_with("$"));
}

} // namespace
