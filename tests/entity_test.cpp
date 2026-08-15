// What an entity can do besides be read.
//
// These are the contracts that separate a tmux command runner from a tmux
// library: an entity knows the server it came from, so it can reach its
// children, its parents, and the commands that change it.

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::CommandFailure;
using libtmux::FailureKind;
using libtmux::first;
using libtmux::matching;
using libtmux::Pane;
using libtmux::Server;
using libtmux::Session;
using libtmux::Window;
namespace pane = libtmux::pane;
namespace window = libtmux::window;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

// The directory a pane reports is its process's, and tmux answers before that
// process exists: immediately after a split the field is empty, and fills in.
std::string settled_path(const Pane& pane) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto current = pane.refresh();
    if (current.has_value() && !current->path().empty()) {
      return std::string{current->path()};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return {};
}

Session only_session(const Server& server) {
  auto sessions = server.sessions();
  EXPECT_TRUE(sessions.has_value());
  EXPECT_EQ(sessions->size(), 1U);
  return sessions.value().at(0);
}

TEST(Entity, ASessionReachesItsWindowsAndTheirPanes) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto created = session.new_window("editor");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  EXPECT_EQ(created->name(), "editor");
  EXPECT_EQ(created->session_id(), session.id());

  const auto windows = session.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  EXPECT_EQ(windows->size(), 2U);

  const auto panes = created->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 1U);
  EXPECT_EQ(panes->front().window_id(), created->id());
}

TEST(Entity, APaneReachesItsWindowAndSession) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto panes = server.panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const Pane& pane = panes->front();

  const auto window = pane.window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  EXPECT_EQ(window->id(), pane.window_id());

  const auto owner = pane.session();
  ASSERT_TRUE(owner.has_value()) << owner.error().diagnostic;
  EXPECT_EQ(owner->id(), session.id());
}

TEST(Entity, EntitiesOutliveTheCallThatListedThem) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();

  // Both the server value and the listing call are gone by the time the
  // entity is read: an entity keeps alive everything it needs.
  const std::vector<Window> windows = [&fixture] {
    const Server server = connect(*fixture);
    auto listed = server.windows();
    EXPECT_TRUE(listed.has_value());
    return std::move(listed).value();
  }();

  ASSERT_FALSE(windows.empty());
  EXPECT_FALSE(windows.front().id().empty());

  const auto panes = windows.front().panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  EXPECT_FALSE(panes->empty());
}

TEST(Entity, ASnapshotIsAMomentAndRefreshTakesANewOne) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto created = session.new_window("before");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;

  const auto renamed = created->rename("after");
  ASSERT_TRUE(renamed.has_value()) << renamed.error().diagnostic;

  // The entity still reads what tmux said when it was listed.
  EXPECT_EQ(created->name(), "before");

  const auto current = created->refresh();
  ASSERT_TRUE(current.has_value()) << current.error().diagnostic;
  EXPECT_EQ(current->name(), "after");
  EXPECT_EQ(current->id(), created->id());
}

TEST(Entity, RefreshingSomethingTmuxNoLongerHasIsMissingNotEmpty) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto created = session.new_window("doomed");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  const auto killed = created->kill();
  ASSERT_TRUE(killed.has_value()) << killed.error().diagnostic;

  // tmux answers a format query about a dead window with empty fields and a
  // zero exit status, so the library has to notice rather than hand back an
  // entity whose id is the empty string.
  const auto current = created->refresh();
  ASSERT_FALSE(current.has_value());
  EXPECT_EQ(current.error().kind, FailureKind::missing);
  EXPECT_NE(current.error().diagnostic.find(created->id()), std::string::npos);
}

TEST(Entity, RefreshingADeadWindowDoesNotAnswerAboutAnotherOne) {
  // A window target is qualified by its session, because a window linked into
  // several sessions has several homes. That makes the missing case harder,
  // not easier: asked about `$0:@dead`, tmux resolves the session, fails to
  // find the window, and answers with whatever that session is showing —
  // fields for a real, different window, with a zero exit status.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto doomed = session.new_window("doomed");
  ASSERT_TRUE(doomed.has_value()) << doomed.error().diagnostic;
  const std::string dead_id{doomed->id()};
  ASSERT_TRUE(doomed->kill().has_value());

  const auto current = doomed->refresh();
  ASSERT_FALSE(current.has_value())
      << "refresh answered about " << current->id() << " when asked about " << dead_id;
  EXPECT_EQ(current.error().kind, FailureKind::missing);
}

TEST(Entity, APaneRunsWhatItIsSentAndCapturesTheResult) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto window = session.new_window("shell");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());
  const Pane& pane = panes->front();

  // The quotes make the produced text differ from the typed text, so the
  // shell's echo of the command cannot satisfy the assertion: only running it
  // can. Without them this passes with no Enter sent at all.
  ASSERT_TRUE(pane.send_text("echo libtmux''-marker").has_value());

  const auto captured_now = [&pane] {
    const auto text = pane.capture();
    return text.has_value() ? *text : std::string{};
  };
  EXPECT_EQ(captured_now().find("libtmux-marker"), std::string::npos)
      << "the echo of the command already satisfied the assertion";

  ASSERT_TRUE(pane.send_key("Enter").has_value());

  std::string captured;
  for (int attempt = 0; attempt < 200; ++attempt) {
    captured = captured_now();
    if (captured.find("libtmux-marker") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  EXPECT_NE(captured.find("libtmux-marker"), std::string::npos);
}

TEST(Entity, AnAnswerThatDoesNotFitIsReportedNotCut) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto window = session.new_window("noisy");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  const std::string pane_id{panes->at(0).id()};

  ASSERT_TRUE(server.run({"set-option", "-g", "history-limit", "5000"}).has_value());
  ASSERT_TRUE(
      panes->at(0)
          .send_text("for i in $(seq 1 3000); do echo \"padding padding $i\"; done")
          .has_value());
  ASSERT_TRUE(panes->at(0).send_key("Enter").has_value());

  // Wait for the shell to actually produce the scrollback. How long three
  // thousand lines take to render is a property of the machine, not of the
  // library: a fixed budget generous on a workstation expires on a loaded
  // continuous integration runner, and reports 1978 lines as a library bug.
  //
  // So this waits for progress to stop rather than for a clock to run out —
  // slow is tolerated, stalled is not, and the two are told apart.
  int history_size = 0;
  int reads_without_progress = 0;
  const auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds{120};
  while (history_size <= 2500 && std::chrono::steady_clock::now() < give_up_at) {
    const auto history =
        server.run({"display-message", "-p", "-t", pane_id, "#{history_size}"});
    ASSERT_TRUE(history.has_value()) << history.error().diagnostic;
    const int seen = std::stoi(*history);
    reads_without_progress = seen == history_size ? reads_without_progress + 1 : 0;
    history_size = seen;
    // Five seconds without a new line means the shell has finished and simply
    // did not get there. That is a real failure, and worth reaching quickly.
    if (reads_without_progress >= 200) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  ASSERT_GT(history_size, 2500) << "the shell never produced the scrollback";

  // The scrollback, which is the answer that outgrows a bound in practice.
  const std::vector<std::string> whole_history{"capture-pane", "-p",   "-S", "-",
                                               "-t",           pane_id};

  // Too small: reported, rather than returned as a prefix whose last line is
  // cut mid-word and reads like data.
  const auto cut = server.run(whole_history, {}, 1024U);
  ASSERT_FALSE(cut.has_value())
      << "a capture of " << cut->size() << " bytes fitted 1024";
  EXPECT_EQ(cut.error().kind, FailureKind::truncated);
  EXPECT_TRUE(cut.error().dispatched);
  EXPECT_NE(cut.error().diagnostic.find("1024"), std::string::npos);

  // Room enough, and the whole thing arrives.
  const auto whole = server.run(whole_history, {}, 8U * 1024U * 1024U);
  ASSERT_TRUE(whole.has_value()) << whole.error().diagnostic;
  EXPECT_GT(whole->size(), 1024U);
}

TEST(Entity, SplittingAWindowReturnsTheNewPane) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto window = session.new_window("split");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;

  const auto added = window->split();
  ASSERT_TRUE(added.has_value()) << added.error().diagnostic;
  EXPECT_EQ(added->window_id(), window->id());

  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  EXPECT_EQ(panes->size(), 2U);
}

TEST(Entity, ListedEntitiesFilterWithoutReachingTmuxAgain) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  ASSERT_TRUE(session.new_window("editor").has_value());
  ASSERT_TRUE(session.new_window("logs").has_value());

  const auto windows = session.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;

  auto editors = *windows | matching(window::name == "editor");
  const auto found = first(editors);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->get().name(), "editor");
}

TEST(Entity, PanesAreArrangedByLayoutAndSize) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto window = session.new_window("arranged");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  ASSERT_TRUE(window->split().has_value());

  // Side by side, so a width is a thing a pane can be given.
  ASSERT_TRUE(window->select_layout("even-horizontal").has_value());
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 2U);

  ASSERT_TRUE(panes->front().set_width(20).has_value());
  const auto narrowed = panes->front().refresh();
  ASSERT_TRUE(narrowed.has_value()) << narrowed.error().diagnostic;
  EXPECT_EQ(narrowed->width(), 20);

  // A layout description round-trips: the one tmux reports is one it accepts.
  const auto described = window->refresh();
  ASSERT_TRUE(described.has_value()) << described.error().diagnostic;
  EXPECT_TRUE(window->select_layout(described->layout()).has_value());

  EXPECT_FALSE(window->select_layout("").has_value());
  EXPECT_FALSE(panes->front().set_width(0).has_value());
}

TEST(Entity, AWindowMovesToAnIndexInItsOwnSession) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  // A second session whose indexes overlap, so a bare index would be
  // ambiguous and could land in the wrong place.
  const auto other = server.new_session("elsewhere");
  ASSERT_TRUE(other.has_value()) << other.error().diagnostic;

  const auto window = session.new_window("travelling");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  ASSERT_TRUE(window->move_to(9).has_value());

  const auto moved = window->refresh();
  ASSERT_TRUE(moved.has_value()) << moved.error().diagnostic;
  EXPECT_EQ(moved->index(), 9);
  EXPECT_EQ(moved->session_id(), session.id());

  const auto elsewhere = other->windows();
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().diagnostic;
  EXPECT_EQ(elsewhere->size(), 1U);
}

TEST(Entity, MovingAWindowLeavesItsOtherLinksAlone) {
  // A window can be linked into several sessions, so a bare window id is not
  // a unique target: tmux resolves it to one of its homes and moving that one
  // unlinks the window from the session the caller never mentioned.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto shared = session.new_window("shared");
  ASSERT_TRUE(shared.has_value()) << shared.error().diagnostic;

  const auto elsewhere = server.new_session("elsewhere");
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().diagnostic;
  ASSERT_TRUE(server
                  .run({"link-window", "-s", std::string{shared->id()}, "-t",
                        std::string{elsewhere->id()} + ":9"})
                  .has_value());

  ASSERT_TRUE(shared->move_to(5).has_value());

  const auto moved = shared->refresh();
  ASSERT_TRUE(moved.has_value()) << moved.error().diagnostic;
  EXPECT_EQ(moved->index(), 5);

  // The link into the other session is still there, at the index it had.
  const auto linked = elsewhere->windows();
  ASSERT_TRUE(linked.has_value()) << linked.error().diagnostic;
  bool still_linked = false;
  for (const Window& window : *linked) {
    if (window.id() == shared->id()) {
      still_linked = true;
      EXPECT_EQ(window.index(), 9);
    }
  }
  EXPECT_TRUE(still_linked) << "moving a window unlinked it from another session";

  // And the move did not drag the user's focus along with it.
  const auto active = session.active_window();
  ASSERT_TRUE(active.has_value()) << active.error().diagnostic;
  EXPECT_NE(active->id(), shared->id());
}

TEST(Entity, APaneBreaksOutIntoAWindowOfItsOwn) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto window = session.new_window("crowded");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto added = window->split();
  ASSERT_TRUE(added.has_value()) << added.error().diagnostic;

  const auto broken = added->break_out("roomy");
  ASSERT_TRUE(broken.has_value()) << broken.error().diagnostic;
  EXPECT_EQ(broken->name(), "roomy");
  EXPECT_NE(broken->id(), window->id());

  const auto moved = added->refresh();
  ASSERT_TRUE(moved.has_value()) << moved.error().diagnostic;
  EXPECT_EQ(moved->window_id(), broken->id());

  // Unnamed is the path that crashes tmux 3.7, so it is exercised too.
  const auto second = window->split();
  ASSERT_TRUE(second.has_value()) << second.error().diagnostic;
  const auto unnamed = second->break_out();
  ASSERT_TRUE(unnamed.has_value()) << unnamed.error().diagnostic;
  EXPECT_FALSE(unnamed->id().empty());
  EXPECT_TRUE(server.is_alive()) << "break-pane took the server down";
}

TEST(Entity, NumericFieldsCompareAsNumbersNotAsText) {
  // Indexes 2 and 10, which order the other way round as text.
  const auto recording = [](std::initializer_list<std::string_view> values) {
    EXPECT_EQ(values.size(), Window::kFields.size());
    std::string line;
    for (const std::string_view value : values) {
      line += value;
      line += libtmux::kFormatSeparator;
    }
    return line + "\n";
  };
  const std::string output = recording({"@0", "small", "1", "$0", "2", "1", "80", "24",
                                        "", "0", "0", "0", "1"}) +
                             recording({"@1", "large", "0", "$0", "10", "4", "200",
                                        "50", "", "0", "0", "0", "1"});

  const auto recorded = libtmux::Snapshot::from_recording(Window::kFields, output);
  ASSERT_NE(recorded, nullptr);
  const std::vector<Window> windows{Window{recorded, 0}, Window{recorded, 1}};

  auto later = windows | matching(window::index > 5);
  const auto found = first(later);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->get().name(), "large");

  auto wide = windows | matching(window::width >= 200 && window::pane_count != 1);
  EXPECT_EQ(std::ranges::distance(wide), 1);

  auto none = windows | matching(window::height < 0);
  EXPECT_EQ(std::ranges::distance(none), 0);

  // The same comparison as text would have said "10" < "2".
  EXPECT_TRUE((window::index == 10)(windows.back()));
  EXPECT_FALSE((window::index <= 2)(windows.back()));
}

TEST(Entity, CreationVerbsCarryTheFlagsTmuxHas) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  // A window that starts somewhere, split side by side, with the new pane
  // starting somewhere else — none of which was expressible before.
  const auto window = session.new_window(
      {.name = "arranged", .start_directory = "/tmp", .after_current = true});
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  EXPECT_EQ(window->name(), "arranged");

  const auto first = window->panes();
  ASSERT_TRUE(first.has_value()) << first.error().diagnostic;
  ASSERT_EQ(first->size(), 1U);
  EXPECT_EQ(settled_path(first->at(0)), "/tmp");

  const auto added =
      window->split({.horizontal = true, .start_directory = "/", .percentage = 25});
  ASSERT_TRUE(added.has_value()) << added.error().diagnostic;
  EXPECT_EQ(settled_path(*added), "/");

  const auto both = window->panes();
  ASSERT_TRUE(both.has_value()) << both.error().diagnostic;
  ASSERT_EQ(both->size(), 2U);
  // Side by side: they share a height and divide the width.
  EXPECT_EQ(both->at(0).height(), both->at(1).height());
  EXPECT_LT(both->at(1).width(), both->at(0).width());

  // Creating something does not move the user.
  const auto active = session.active_window();
  ASSERT_TRUE(active.has_value()) << active.error().diagnostic;
  EXPECT_NE(active->id(), window->id());

  EXPECT_FALSE(window->split({.percentage = 0}).has_value());
  EXPECT_FALSE(window->split({.percentage = 101}).has_value());
}

TEST(Entity, ASessionIsCreatedAtASizeAndADirectory) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto created = server.new_session({.name = "sized",
                                           .start_directory = "/tmp",
                                           .first_window_name = "shell",
                                           .width = 120,
                                           .height = 40});
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  EXPECT_EQ(created->name(), "sized");
  EXPECT_EQ(created->path(), "/tmp");

  const auto windows = created->windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  ASSERT_EQ(windows->size(), 1U);
  EXPECT_EQ(windows->at(0).name(), "shell");
  EXPECT_EQ(windows->at(0).width(), 120);
  EXPECT_EQ(windows->at(0).height(), 40);
}

TEST(Entity, CaptureReadsTheScrollbackWhenAskedTo) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  const Session session = only_session(server);

  const auto window = session.new_window("history");
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto panes = window->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  const Pane& pane = panes->at(0);

  ASSERT_TRUE(pane.send_text("for i in $(seq 1 200); do echo scroll''back $i; done")
                  .has_value());
  ASSERT_TRUE(pane.send_key("Enter").has_value());

  std::string visible;
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto text = pane.capture();
    ASSERT_TRUE(text.has_value()) << text.error().diagnostic;
    visible = *text;
    if (visible.find("scrollback 200") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  ASSERT_NE(visible.find("scrollback 200"), std::string::npos);
  // The visible pane has scrolled past the beginning.
  EXPECT_EQ(visible.find("scrollback 1\n"), std::string::npos);

  const auto history = pane.capture({.whole_history = true});
  ASSERT_TRUE(history.has_value()) << history.error().diagnostic;
  EXPECT_NE(history->find("scrollback 1\n"), std::string::npos);
  EXPECT_GT(history->size(), visible.size());
}

TEST(Entity, AWholeWorkspaceIsBuiltFromTypedVerbsAlone) {
  // The acceptance test for the option aggregates: a session, a window in a
  // directory, two panes side by side each running something, and a layout —
  // with no raw argv anywhere. The workspace consumer had to reach past the
  // library for every one of these.
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto session = server.new_session({.name = "typed", .start_directory = "/tmp"});
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  const auto editor = session->new_window({.name = "editor", .start_directory = "/"});
  ASSERT_TRUE(editor.has_value()) << editor.error().diagnostic;

  const auto logs = editor->split({.horizontal = true, .start_directory = "/tmp"});
  ASSERT_TRUE(logs.has_value()) << logs.error().diagnostic;
  ASSERT_TRUE(editor->select_layout("even-horizontal").has_value());

  const auto panes = editor->panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_EQ(panes->size(), 2U);
  EXPECT_EQ(settled_path(panes->at(0)), "/");
  EXPECT_EQ(settled_path(panes->at(1)), "/tmp");

  // And the session the caller asked for is the one that exists.
  const auto listed = server.sessions();
  ASSERT_TRUE(listed.has_value()) << listed.error().diagnostic;
  EXPECT_TRUE(std::ranges::any_of(
      *listed, [](const Session& row) { return row.name() == "typed"; }));
}

TEST(Entity, ARecordedSnapshotFiltersButCannotAct) {
  // What `list-panes -F` prints, written out: one value per field, each
  // closed by the separator, one line per pane.
  const auto recording = [](std::initializer_list<std::string_view> values) {
    EXPECT_EQ(values.size(), Pane::kFields.size());
    std::string line;
    for (const std::string_view value : values) {
      line += value;
      line += libtmux::kFormatSeparator;
    }
    return line + "\n";
  };
  const std::string output =
      recording({"%0", "nvim", "1", "@0", "$0", "0", "editor", "4210", "/dev/pts/3",
                 "/home/user", "80", "24", "0", "0", "1", "0", "1", "1", "0"}) +
      recording({"%1", "zsh", "0", "@0", "$0", "1", "shell", "4211", "/dev/pts/4",
                 "/home/user", "80", "24", "0", "0", "0", "1", "1", "1", "1"});

  const auto recorded = libtmux::Snapshot::from_recording(Pane::kFields, output);
  ASSERT_NE(recorded, nullptr);
  ASSERT_EQ(recorded->rows().size(), 2U);

  const Pane editing{recorded, 0};
  EXPECT_EQ(editing.command(), "nvim");
  EXPECT_TRUE(editing.active());
  EXPECT_EQ(editing.pid(), 4210);
  EXPECT_EQ(editing.width(), 80);
  EXPECT_TRUE(editing.at_top());
  EXPECT_FALSE(editing.at_bottom());
  EXPECT_FALSE(editing.piping());
  EXPECT_TRUE((pane::command.starts_with("nv") && pane::active)(editing));

  const auto killed = editing.kill();
  ASSERT_FALSE(killed.has_value());
  EXPECT_EQ(killed.error().kind, FailureKind::validation);
  EXPECT_FALSE(killed.error().dispatched);
}

TEST(Entity, ANewSessionComesBackAsASession) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto created = server.new_session("second");
  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  EXPECT_EQ(created->name(), "second");

  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  EXPECT_EQ(sessions->size(), 2U);

  ASSERT_TRUE(created->kill().has_value());
  const auto remaining = server.sessions();
  ASSERT_TRUE(remaining.has_value()) << remaining.error().diagnostic;
  EXPECT_EQ(remaining->size(), 1U);
}

} // namespace
