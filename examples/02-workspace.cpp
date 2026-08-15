// Building an arrangement: a session, windows, splits, and something running
// in each pane — without composing a single tmux argument.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <libtmux/libtmux.hpp>

#include "scratch_server.hpp"

namespace {

// Every call reports failure as a value. This is the shape a real program
// takes: check, say what went wrong, stop.
template <typename Result> bool failed(const Result& result, const char* doing) {
  if (result.has_value()) {
    return false;
  }
  std::fprintf(stderr, "%s: %s\n", doing, result.error().diagnostic.c_str());
  return true;
}

} // namespace

int main() {
  const example::ScratchServer scratch = example::ScratchServer::open();
  const libtmux::Server& server = scratch.get();

  const auto session = server.new_session(
      {.name = "workspace", .start_directory = "/tmp", .first_window_name = "shell"});
  if (failed(session, "creating the session")) {
    return 1;
  }

  const auto editor =
      session->new_window({.name = "editor", .start_directory = "/tmp"});
  if (failed(editor, "creating the editor window")) {
    return 1;
  }

  // Side by side, the right-hand pane taking a third of the width.
  const auto logs = editor->split({.horizontal = true, .percentage = 33});
  if (failed(logs, "splitting")) {
    return 1;
  }
  if (failed(editor->select_layout("even-horizontal"), "laying out")) {
    return 1;
  }

  // Typed text is typed literally: nothing here is interpreted as a key name.
  if (failed(logs->send_text("echo watching"), "typing") ||
      failed(logs->send_key("Enter"), "sending Enter")) {
    return 1;
  }

  const auto windows = session->windows();
  if (failed(windows, "listing windows")) {
    return 1;
  }
  for (const libtmux::Window& window : *windows) {
    std::printf("%s: %lld pane(s), %lldx%lld\n", std::string{window.name()}.c_str(),
                window.pane_count(), window.width(), window.height());
  }

  // Going back to the previously selected pane, which only the server
  // knows: a listing does not say which one that was.
  if (const auto back = windows->front().select_last_pane(); back.has_value()) {
    std::printf("back to pane %s\n", std::string{back->id()}.c_str());
  }

  // The same window can be shown in a second session: one window, two
  // places, not a copy.
  if (const auto shared = server.new_session("mirror"); shared.has_value()) {
    if (failed(windows->front().link_to(*shared), "sharing the first window")) {
      return 1;
    }
    std::printf(
        "first window is held by %lld sessions\n",
        windows->front()
            .refresh()
            .transform([](const libtmux::Window& w) { return w.linked_sessions(); })
            .value_or(0));

    // And shown in one place again. tmux refuses to remove the last link
    // rather than leaving a window no session holds, so this is not a kill
    // by another name.
    if (const auto mirrored = shared->windows(); mirrored.has_value()) {
      for (const libtmux::Window& window : *mirrored) {
        if (window.id() == windows->front().id() &&
            failed(window.unlink(), "unsharing the first window")) {
          return 1;
        }
      }
    }
  }

  // A pane broken out into its own window, then rejoined: the tree comes
  // apart and goes back together.
  if (const auto extra = windows->front().split(); extra.has_value()) {
    if (const auto lifted = extra->break_out(); lifted.has_value()) {
      if (failed(extra->join(windows->front()), "rejoining the pane")) {
        return 1;
      }
    }
  }

  // Step to tmux's next arrangement and back, which is the only way to
  // reach the preset layouts without naming each one. Rotating is a
  // different act: it moves which pane sits in which cell.
  if (failed(windows->front().next_layout(), "stepping to the next layout") ||
      failed(windows->front().previous_layout(), "stepping back") ||
      failed(windows->front().rotate(), "rotating the panes")) {
    return 1;
  }

  // A workspace can carry tmux configuration of its own. Checking it first
  // is what keeps a broken line from being half-applied: nothing in the file
  // runs until tmux says it parses.
  const auto config = std::filesystem::temp_directory_path() / "libtmux-workspace.conf";
  {
    std::ofstream writing{config};
    writing << "set-option -g @workspace built\n";
  }
  if (const auto checked = server.check_file(config); checked.has_value()) {
    if (failed(server.source_file(config), "applying the workspace config")) {
      return 1;
    }
  } else {
    std::fprintf(stderr, "the workspace config would not load: %s\n",
                 checked.error().diagnostic.c_str());
  }
  std::error_code ignored;
  std::filesystem::remove(config, ignored);

  // A workspace usually wants its programs started with variables of their
  // own, which is what a tmuxp document's `environment:` means.
  if (const auto configured = session->new_window(
          {.name = "configured", .environment = {{"EDITOR", "vi"}}});
      failed(configured, "making a window with an environment")) {
    return 1;
  }

  // A workspace can carry key bindings of its own, in a table of its own so
  // it can take them away again without touching anyone else's.
  if (failed(server.bind_key("workspace", "r", {"display-message", "reloaded"}),
             "binding the workspace key")) {
    return 1;
  }
  if (failed(server.unbind_key("workspace", "r"), "unbinding the workspace key")) {
    return 1;
  }

  // Setup a workspace often needs done on the machine tmux is on, rather
  // than in a pane where a person would see it.
  if (failed(server.run_shell("true"), "running the setup command")) {
    return 1;
  }

  // Leave the first window active, the way a workspace tool would: the
  // caller decides where the session opens, not whichever window was made
  // last.
  if (failed(windows->front().select(), "selecting the first window")) {
    return 1;
  }
  return 0;
}
