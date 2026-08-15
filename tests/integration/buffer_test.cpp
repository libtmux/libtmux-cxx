// The server's cut buffers.
//
// Behavior evidence for the buffer capabilities, one case each.

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

TEST(Buffers, AServerHoldingNoneAnswersWithAnEmptyList) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Empty, not a failure: a server with no buffers is an ordinary state and
  // tmux reports it by printing nothing.
  const auto empty = server.buffers();
  ASSERT_TRUE(empty.has_value()) << empty.error().diagnostic;
  EXPECT_TRUE(empty->empty());
}

TEST(Buffers, SetNamesABufferAndListingReportsIt) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  ASSERT_TRUE(server.set_buffer("greeting", "hello buffer").has_value());
  const auto listed = server.buffers();
  ASSERT_TRUE(listed.has_value()) << listed.error().diagnostic;
  ASSERT_EQ(listed->size(), 1U);
  EXPECT_EQ(listed->front().name(), "greeting");
  EXPECT_EQ(listed->front().size(), 12);
  EXPECT_EQ(listed->front().sample(), "hello buffer");

  // An unnamed buffer gets a name from tmux rather than replacing the
  // named one.
  ASSERT_TRUE(server.set_buffer("", "anonymous").has_value());
  const auto both = server.buffers();
  ASSERT_TRUE(both.has_value()) << both.error().diagnostic;
  EXPECT_EQ(both->size(), 2U);
}

TEST(Buffers, ContentsReadBackExactlyWhatWentIn) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Leading dash, embedded newline, and no trailing one: tmux prints a
  // buffer's bytes verbatim, so a round trip has to be exact rather than
  // line-shaped.
  const std::string awkward = "-not-a-flag\nsecond line";
  ASSERT_TRUE(server.set_buffer("awkward", awkward).has_value());
  const auto listed = server.buffers();
  ASSERT_TRUE(listed.has_value()) << listed.error().diagnostic;
  ASSERT_EQ(listed->size(), 1U);

  const auto read = listed->front().contents();
  ASSERT_TRUE(read.has_value()) << read.error().diagnostic;
  EXPECT_EQ(*read, awkward);
}

TEST(Buffers, RemoveTakesOneAwayAndLeavesTheRest) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  ASSERT_TRUE(server.set_buffer("keep", "kept").has_value());
  ASSERT_TRUE(server.set_buffer("drop", "dropped").has_value());

  const auto before = server.buffers();
  ASSERT_TRUE(before.has_value()) << before.error().diagnostic;
  ASSERT_EQ(before->size(), 2U);
  const auto doomed = std::ranges::find_if(
      *before, [](const libtmux::Buffer& one) { return one.name() == "drop"; });
  ASSERT_NE(doomed, before->end());

  ASSERT_TRUE(doomed->remove().has_value());

  const auto after = server.buffers();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  ASSERT_EQ(after->size(), 1U);
  EXPECT_EQ(after->front().name(), "keep");

  // Reading a buffer that has gone is a failure, not empty contents.
  EXPECT_FALSE(doomed->contents().has_value());
}

TEST(Buffers, PasteDeliversTheTextAndLeavesTheBuffer) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  // Two panes, and the paste aimed at the one that is not active: with a
  // single pane tmux would paste there anyway, and the test could not tell
  // whether the target was honoured.
  const auto window = session->active_window();
  ASSERT_TRUE(window.has_value()) << window.error().diagnostic;
  const auto second = window->split();
  ASSERT_TRUE(second.has_value()) << second.error().diagnostic;
  const auto active = session->active_pane();
  ASSERT_TRUE(active.has_value()) << active.error().diagnostic;
  const auto quiet = active->id() == second->id() ? window->panes()->front() : *second;
  ASSERT_NE(quiet.id(), active->id());
  // Wait for the pane's shell to be reading before pasting into it. A paste
  // delivered to a shell that has not started yet is simply lost, and no
  // amount of waiting afterwards brings it back — which is what a ten-second
  // wait on the capture proved on a slower machine. A drawn prompt is the
  // signal that something is there to receive it.
  const auto ready_by = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (std::chrono::steady_clock::now() < ready_by) {
    const auto drawn = quiet.capture();
    if (drawn.has_value() && !drawn->empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }

  ASSERT_TRUE(server.set_buffer("payload", "pasted-marker").has_value());
  const auto held = server.buffers();
  ASSERT_TRUE(held.has_value()) << held.error().diagnostic;
  ASSERT_EQ(held->size(), 1U);

  ASSERT_TRUE(quiet.paste(held->front()).has_value());

  // The text lands on the command line of the pane it was aimed at, without
  // being run: that is the whole difference between pasting and sending
  // keys, and the other pane never sees it.
  // tmux delivers the paste to the pane's input, and the shell has to draw it
  // before a capture can see it. Reading once turns "not yet" into "never".
  std::string shown_text;
  const auto visible_by = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  for (;;) {
    // Joined, because the pane is 80 columns and a runner whose hostname fills
    // 76 of them wraps the pasted text mid-word: the capture held "past",
    // a line break, then "ed-marker". The text arrived exactly as intended and
    // the search was looking at the terminal's line wrapping.
    const auto shown = quiet.capture({.join_wrapped = true});
    ASSERT_TRUE(shown.has_value()) << shown.error().diagnostic;
    shown_text = *shown;
    if (shown_text.find("pasted-marker") != std::string::npos ||
        std::chrono::steady_clock::now() >= visible_by) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  }
  EXPECT_NE(shown_text.find("pasted-marker"), std::string::npos)
      << "pane " << quiet.id() << " running "
      << quiet.expand("#{pane_current_command}").value_or("?")
      << ", dead=" << quiet.expand("#{pane_dead}").value_or("?") << ", "
      << quiet.expand("#{pane_width}").value_or("?") << "x"
      << quiet.expand("#{pane_height}").value_or("?") << ", capture is "
      << shown_text.size() << " bytes: [" << shown_text << "]";
  const auto elsewhere = active->capture();
  ASSERT_TRUE(elsewhere.has_value()) << elsewhere.error().diagnostic;
  EXPECT_EQ(elsewhere->find("pasted-marker"), std::string::npos);

  // Pasting reads the buffer; it does not consume it.
  const auto after = server.buffers();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_EQ(after->size(), 1U);
}

TEST(Buffers, PasteCanConsumeTheBufferItDelivered) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;
  const auto pane = session->active_pane();
  ASSERT_TRUE(pane.has_value()) << pane.error().diagnostic;
  ASSERT_TRUE(server.set_buffer("once", "one-shot").has_value());
  const auto held = server.buffers();
  ASSERT_TRUE(held.has_value()) << held.error().diagnostic;
  ASSERT_EQ(held->size(), 1U);

  ASSERT_TRUE(pane->paste(held->front(), /*consume=*/true).has_value());

  const auto after = server.buffers();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  EXPECT_TRUE(after->empty());
}

TEST(Buffers, AFileRoundTripsThroughABufferUnchanged) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  // Under the fixture's own directory, which it removes: the server reads
  // and writes these, so they must outlive the call and nothing else.
  const auto in = fixture->tmux_tmpdir() / "in.txt";
  const auto out = fixture->tmux_tmpdir() / "out.txt";
  // No trailing newline, and an embedded one, so a line-shaped
  // implementation would show up as a difference.
  const std::string original = "from a file\nsecond line";
  {
    std::ofstream writing{in, std::ios::binary};
    writing << original;
  }

  ASSERT_TRUE(server.load_buffer("loaded", in).has_value());
  const auto held = server.buffers();
  ASSERT_TRUE(held.has_value()) << held.error().diagnostic;
  ASSERT_EQ(held->size(), 1U);
  const auto contents = held->front().contents();
  ASSERT_TRUE(contents.has_value()) << contents.error().diagnostic;
  EXPECT_EQ(*contents, original);

  ASSERT_TRUE(server.save_buffer("loaded", out).has_value());
  std::ifstream reading{out, std::ios::binary};
  const std::string written{std::istreambuf_iterator<char>{reading},
                            std::istreambuf_iterator<char>{}};
  EXPECT_EQ(written, original);
}

TEST(Buffers, LoadingSaysWhenTheFileIsNotThere) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto missing = server.load_buffer("absent", fixture->tmux_tmpdir() / "no.txt");
  ASSERT_FALSE(missing.has_value());
  EXPECT_FALSE(missing.error().diagnostic.empty());

  // An unnamed buffer is refused before anything is dispatched, because
  // tmux would otherwise pick a name and the caller could not find it.
  const auto unnamed = server.load_buffer("", fixture->tmux_tmpdir() / "no.txt");
  ASSERT_FALSE(unnamed.has_value());
  EXPECT_EQ(unnamed.error().kind, libtmux::FailureKind::validation);
  EXPECT_FALSE(unnamed.error().dispatched);
}

} // namespace
