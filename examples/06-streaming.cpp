// Watching tmux instead of asking it: one held-open connection, events as they
// happen, and pane output as it is printed.
//
// The rest of the library runs a tmux command and reads the answer. This is the
// other half — a control connection stays open, tmux says what changed, and a
// program reacts. Nothing here polls.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include <libtmux/libtmux.hpp>

#include "scratch_server.hpp"

namespace {

using namespace std::chrono_literals;

std::string as_text(std::span<const std::byte> bytes) {
  return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Printable, so a pane's control characters do not scribble on this program's
// own output.
std::string readable(std::string_view text) {
  std::string out;
  for (const char character : text) {
    out += (character >= ' ' && character != '\177') ? character : '.';
  }
  return out;
}

} // namespace

int main() {
  const example::ScratchServer scratch = example::ScratchServer::open();
  const libtmux::Server& server = scratch.get();

  const auto panes = server.panes();
  if (!panes.has_value()) {
    std::fprintf(stderr, "%s\n", panes.error().diagnostic.c_str());
    return 1;
  }
  const std::string pane{panes->at(0).id()};

  // `pane_output` is decided here and cannot be changed later, because tmux
  // decides it here: a control client that starts without output cannot be
  // asked for it afterwards. `docs/design/pane-output-streaming.md` measures
  // that, and what tmux does to a reader who falls behind.
  auto connected = libtmux::Connection::connect({
      .socket_path = scratch.socket_path(),
      .session_name = "example",
      .pane_output = true,
  });
  if (!connected.has_value()) {
    std::fprintf(stderr, "%s\n", connected.error().message.c_str());
    return 1;
  }
  auto connection = std::move(*connected);

  // Make something happen: a new window, and a line printed in the first pane.
  libtmux::ControlRequest request;
  request.group.push_back({{"new-window", "-d", "-n", "watched"}});
  request.group.push_back({{"send-keys", "-t", pane, "echo hello-from-tmux", "Enter"}});
  const auto ran =
      connection.execute(std::move(request), std::chrono::steady_clock::now() + 5s);
  if (ran.connection_error.has_value()) {
    std::fprintf(stderr, "%s\n", ran.connection_error->message.c_str());
    return 1;
  }

  // Wait for tmux to say so, rather than asking repeatedly. Every answer is
  // read for what it is: a kind, the ids it names, and — for pane output — the
  // bytes, already unescaped.
  bool saw_window = false;
  bool saw_output = false;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline && !(saw_window && saw_output)) {
    const auto batch = connection.wait_for_notifications(deadline);
    if (batch.empty()) {
      break;
    }
    for (const libtmux::Notification& notification : batch) {
      const auto event = libtmux::parse(notification);
      switch (event.kind) {
      case libtmux::NotificationKind::window_add:
        std::printf("window added: %s\n", std::string{event.window}.c_str());
        saw_window = true;
        break;
      case libtmux::NotificationKind::output:
        if (as_text(event.payload).find("hello-from-tmux") != std::string::npos) {
          std::printf("output from %s: %s\n", std::string{event.pane}.c_str(),
                      readable(as_text(event.payload)).c_str());
          saw_output = true;
        }
        break;
      case libtmux::NotificationKind::paused:
        // The only report that output was dropped, and it names the pane.
        std::printf("paused, output lost for %s\n", std::string{event.pane}.c_str());
        break;
      default:
        break;
      }
    }
  }

  if (!saw_window || !saw_output) {
    std::fprintf(stderr, "tmux never reported what it was asked to do\n");
    return 1;
  }

  if (const auto closed = connection.shutdown(std::chrono::steady_clock::now() + 5s);
      !closed.has_value()) {
    std::fprintf(stderr, "%s\n", closed.error().message.c_str());
    return 1;
  }
  return 0;
}
