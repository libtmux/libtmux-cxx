// What tmux prints between field values, when a value contains it too.
//
// The separator is chosen so it cannot appear in a format *name*. It can
// appear in a format *value*: `rename-window` takes it without complaint, a
// directory may be named with it, and a program in a pane sets its own title.
// One such value used to make every listing on that server fail to split, so a
// name nobody else chose broke reads of everything.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libtmux/cardinality.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include "support/scoped_tmux_server.hpp"

namespace {

using libtmux::Server;

Server connect(const libtmux::test::ScopedTmuxServer& fixture) {
  auto server = Server::at_socket_path(fixture.socket_path().string());
  EXPECT_TRUE(server.has_value());
  return server.value();
}

const std::string kSeparator{libtmux::kFormatSeparator};
const std::string kEscape{libtmux::kFormatEscape};

TEST(SnapshotFraming, AWindowNamedWithTheSeparatorIsReadBack) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);
  auto session = server.session(fixture->session_name());
  ASSERT_TRUE(session.has_value()) << session.error().diagnostic;

  // Both markers, and both next to each other, so a transform that escaped in
  // the wrong order would come back with the two confused.
  const std::string hostile = "a" + kSeparator + "b" + kEscape + "S" + kEscape + "c";
  const auto made = session->new_window(hostile);
  ASSERT_TRUE(made.has_value()) << made.error().diagnostic;
  EXPECT_EQ(made->name(), hostile);

  // And every other listing still reads, which is the part that used to break:
  // one poisoned value failed the whole snapshot, not just its own row.
  const auto windows = server.windows();
  ASSERT_TRUE(windows.has_value()) << windows.error().diagnostic;
  EXPECT_TRUE(std::ranges::any_of(
      *windows, [&](const libtmux::Window& one) { return one.name() == hostile; }));

  const auto panes = server.panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  EXPECT_FALSE(panes->empty());
  const auto sessions = server.sessions();
  ASSERT_TRUE(sessions.has_value()) << sessions.error().diagnostic;
  EXPECT_FALSE(sessions->empty());
}

TEST(SnapshotFraming, APaneTitleCarriesEitherMarker) {
  auto fixture = libtmux::test::ScopedTmuxServer::start();
  ASSERT_TRUE(fixture.has_value()) << fixture.error();
  const Server server = connect(*fixture);

  const auto panes = server.panes();
  ASSERT_TRUE(panes.has_value()) << panes.error().diagnostic;
  ASSERT_FALSE(panes->empty());

  // A title is not the caller's to choose: the program running in the pane
  // sets it. This is that value arriving.
  const std::string title = kEscape + kSeparator + "title" + kSeparator;
  ASSERT_TRUE(panes->front().set_title(title).has_value());

  const auto after = server.panes();
  ASSERT_TRUE(after.has_value()) << after.error().diagnostic;
  ASSERT_FALSE(after->empty());
  EXPECT_EQ(after->front().title(), title);
}

// The encode and decode halves, without tmux: every value has to survive the
// round trip, including the ones made entirely of markers.
TEST(SnapshotFraming, DecodingInvertsTheEscapingItWasBuiltFor) {
  const auto encoded = [](std::string_view value) {
    std::string out;
    for (std::size_t index = 0; index < value.size();) {
      if (value.compare(index, kEscape.size(), kEscape) == 0) {
        out += kEscape + "E";
        index += kEscape.size();
      } else if (value.compare(index, kSeparator.size(), kSeparator) == 0) {
        out += kEscape + "S";
        index += kSeparator.size();
      } else {
        out += value[index++];
      }
    }
    return out;
  };

  for (const std::string& value :
       {std::string{}, std::string{"plain"}, kSeparator, kEscape, kEscape + kSeparator,
        kSeparator + kEscape, kEscape + "S", kEscape + "E",
        kEscape + kEscape + kSeparator + kSeparator,
        std::string{"a"} + kEscape + "b"}) {
    std::string buffer = encoded(value);
    const std::size_t size = libtmux::decode_value(buffer.data(), buffer.size());
    EXPECT_EQ(std::string_view(buffer.data(), size), value)
        << "escaped form was: " << encoded(value);
  }
}

// The escaping is in the format tmux is asked for, so it is visible in the
// request rather than applied afterwards to something already ambiguous.
TEST(SnapshotFraming, TheRequestAsksTmuxToEscapeRatherThanRepairingLater) {
  const std::array<std::string_view, 1> fields{std::string_view{"window_name"}};
  const std::string request = libtmux::format_request(fields);
  EXPECT_NE(request.find("#{s/"), std::string::npos);
  EXPECT_NE(request.find("window_name"), std::string::npos);
  EXPECT_TRUE(request.ends_with(kSeparator));
}

} // namespace
