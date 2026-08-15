#pragma once

// Build a described workspace on a real tmux server.
//
// This is a consumer, not part of the library: it exists to put weight on the
// public surface and report where that surface is awkward. It takes a
// workspace as data rather than as YAML, because parsing a config file is a
// serialization concern that belongs in an opt-in integration — the shape
// below is what a tmuxp document would deserialize into.

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libtmux/batch.hpp"
#include "libtmux/server.hpp"
#include "libtmux/target.hpp"

namespace libtmux::workspace {

// One thing to run in a pane, and how to send it.
//
// A tmuxp document writes a command as a string or as a mapping carrying
// `cmd`, `enter` and sleeps, so a plain string cannot hold what a document
// says. The distinction `enter` draws is the library's own: `send_text`
// never appends a newline and `send_key` submits, so leaving it false puts
// the text on the command line and stops there.
struct Command {
  std::string text{};
  // False leaves the text unsubmitted, which is what `enter: false` means.
  bool enter{true};
  // tmuxp's `sleep_before` and `sleep_after`. A workspace that starts a
  // server and then talks to it needs the wait; nothing in tmux provides
  // one, so the builder does the waiting.
  std::chrono::milliseconds pause_before{};
  std::chrono::milliseconds pause_after{};
  // Send it with a leading space, which a shell configured for it keeps out
  // of history. Resolved from the document's levels when it is read, so the
  // builder has one thing to look at rather than three.
  bool suppress_history{true};

  friend bool operator==(const Command&, const Command&) = default;
};

struct Pane {
  // Run in order after the pane exists. Empty leaves the pane at its shell.
  std::vector<Command> shell_commands{};
  // What this pane runs instead of the default shell. The commands above are
  // then typed into that program rather than into a shell, which is what a
  // document opening an editor or an interpreter means.
  std::string shell{};
  // Where the pane starts. Empty inherits the window's, then the session's.
  std::string start_directory{};
  // Leave active when the workspace is finished. Several in one window is the
  // last one; the description is data, and rejecting it is the parser's job.
  bool focus{false};
  // Variables the processes here start with. Session variables reach a pane
  // made later; a window's do not.
  std::vector<std::pair<std::string, std::string>> environment{};
};

struct Window {
  std::string name{};
  // One of tmux's named layouts, or a layout description a previous session
  // reported. Empty leaves the arrangement tmux chose while splitting.
  std::string layout{};
  std::string start_directory{};
  // Applied before the layout, in order: a layout reads options like
  // `main-pane-height`, so setting them afterwards arranges to the old value.
  // A vector rather than a map because tmux takes these as ordered commands
  // and two runs of the same description should issue the same ones.
  std::vector<std::pair<std::string, std::string>> options{};
  // Applied once the window's panes exist. `synchronize-panes` is the reason
  // the two lists are separate: set before the splits, it types into panes
  // that are still being made.
  std::vector<std::pair<std::string, std::string>> options_after{};
  // What the window's first pane runs instead of the default shell.
  std::string shell{};
  bool focus{false};
  // Variables the processes here start with. Session variables reach a pane
  // made later; a window's do not.
  std::vector<std::pair<std::string, std::string>> environment{};
  // Where the window sits. Absent lets tmux choose the next free index.
  std::optional<long long> index{};
  // Every window has at least one pane; the first is created with the window.
  std::vector<Pane> panes{{}};
};

struct Workspace {
  std::string session_name{};
  std::string start_directory{};
  // Server-wide options, and the session's own.
  std::vector<std::pair<std::string, std::string>> global_options{};
  std::vector<std::pair<std::string, std::string>> options{};
  // Variables the processes here start with. Session variables reach a pane
  // made later; a window's do not.
  std::vector<std::pair<std::string, std::string>> environment{};
  std::vector<Window> windows{};
};

struct BuildError {
  // Which window was being built, so a failure points at the description
  // rather than at an opaque tmux message.
  std::size_t window_index{};
  std::string reason;
};

// Create the workspace and return the session it made. The session must not
// already exist: adopting a live session is a different operation with
// different risks, and conflating them is how a builder silently reshapes
// something a user was working in.
[[nodiscard]] inline libtmux::expected<libtmux::Session, BuildError>
build(const Server& server, const Workspace& description) {
  if (description.session_name.empty() || description.windows.empty()) {
    return libtmux::unexpected(BuildError{0, "workspace names no session or window"});
  }
  const auto session = session_target(description.session_name);
  if (!session.has_value()) {
    return libtmux::unexpected(BuildError{0, "session name cannot address itself"});
  }

  // The directory a pane starts in, most specific first.
  const auto directory = [&description](const Window& window, const Pane& pane) {
    if (!pane.start_directory.empty()) {
      return pane.start_directory;
    }
    if (!window.start_directory.empty()) {
      return window.start_directory;
    }
    return description.start_directory;
  };
  const auto with_directory = [](std::vector<std::string> command,
                                 const std::string& where) {
    if (!where.empty()) {
      command.emplace_back("-c");
      command.push_back(where);
    }
    return command;
  };
  // A batch is raw argv, so the pairs are joined here the way the typed
  // options do it. Everything the description carries at this level, then
  // the level above it: tmux takes the last `-e` for a repeated name.
  // A window's shell is the positional a creation command takes last, so it
  // goes on after every flag.
  const auto with_shell = [](std::vector<std::string> command,
                             const std::string& shell) {
    if (!shell.empty()) {
      command.emplace_back("--");
      command.push_back(shell);
    }
    return command;
  };
  const auto with_environment =
      [](std::vector<std::string> command,
         const std::vector<std::pair<std::string, std::string>>& outer,
         const std::vector<std::pair<std::string, std::string>>& inner,
         const std::string& shell = {}) {
        for (const auto* level : {&outer, &inner}) {
          for (const auto& [name, value] : *level) {
            command.emplace_back("-e");
            command.push_back(name + "=" + value);
          }
        }
        if (!shell.empty()) {
          command.emplace_back("--");
          command.push_back(shell);
        }
        return command;
      };

  const Window& first = description.windows.front();
  CommandBatch batch;
  batch.add(with_shell(
      with_environment(
          with_directory({"new-session", "-d", "-s", *session, "-n", first.name},
                         directory(first, first.panes.front())),
          description.environment, first.panes.front().environment),
      first.panes.front().shell.empty() ? first.shell : first.panes.front().shell));
  for (std::size_t pane = 1; pane < first.panes.size(); ++pane) {
    batch.add(with_environment(
        with_directory({"split-window", "-t", *session + ":" + first.name},
                       directory(first, first.panes[pane])),
        first.environment, first.panes[pane].environment, first.panes[pane].shell));
  }
  for (std::size_t index = 1; index < description.windows.size(); ++index) {
    const Window& window = description.windows[index];
    const std::string where = window.index.has_value()
                                  ? *session + ":" + std::to_string(*window.index)
                                  : *session;
    batch.add(with_shell(
        with_environment(with_directory({"new-window", "-t", where, "-n", window.name},
                                        directory(window, window.panes.front())),
                         window.environment, window.panes.front().environment),
        window.panes.front().shell.empty() ? window.shell
                                           : window.panes.front().shell));
    for (std::size_t pane = 1; pane < window.panes.size(); ++pane) {
      batch.add(with_environment(
          with_directory({"split-window", "-t", *session + ":" + window.name},
                         directory(window, window.panes[pane])),
          window.environment, window.panes[pane].environment,
          window.panes[pane].shell));
    }
  }
  if (const auto created = server.run_batch(batch); !created.has_value()) {
    return libtmux::unexpected(BuildError{0, created.error().diagnostic});
  }

  // Server-wide first, then the session's: a session option set over a
  // server one is the narrower answer, and doing it in the other order
  // would leave the wider one on top.
  for (const auto& [option, value] : description.global_options) {
    if (const auto set = server.set_global_option(option, value); !set.has_value()) {
      return libtmux::unexpected(BuildError{0, set.error().diagnostic});
    }
  }
  const auto built_session = server.session(*session);
  if (!built_session.has_value()) {
    return libtmux::unexpected(BuildError{0, built_session.error().diagnostic});
  }
  for (const auto& [option, value] : description.options) {
    if (const auto set = built_session->set_option(option, value); !set.has_value()) {
      return libtmux::unexpected(BuildError{0, set.error().diagnostic});
    }
  }

  const auto built = server.session(*session);
  if (!built.has_value()) {
    return libtmux::unexpected(BuildError{0, built.error().diagnostic});
  }
  const auto windows = built->windows();
  if (!windows.has_value()) {
    return libtmux::unexpected(BuildError{0, windows.error().diagnostic});
  }
  if (windows->size() != description.windows.size()) {
    return libtmux::unexpected(BuildError{0, "tmux built a different set of windows"});
  }

  // Commands run after every pane exists, so an earlier window's command
  // cannot race the creation of a later one. Each pane is addressed by the id
  // tmux gave it: a `session:window.0` path assumes a pane numbering that
  // `pane-base-index` is free to change under the caller.
  for (std::size_t index = 0; index < description.windows.size(); ++index) {
    const Window& described = description.windows[index];
    const auto panes = (*windows)[index].panes();
    if (!panes.has_value()) {
      return libtmux::unexpected(BuildError{index, panes.error().diagnostic});
    }
    if (panes->size() != described.panes.size()) {
      return libtmux::unexpected(
          BuildError{index, "tmux built a different set of panes"});
    }
    // Options first: a layout reads them, so `main-pane-height` set after
    // `select-layout` would arrange the window to the previous value.
    for (const auto& [option, value] : described.options) {
      if (const auto set = (*windows)[index].set_option(option, value);
          !set.has_value()) {
        return libtmux::unexpected(BuildError{index, set.error().diagnostic});
      }
    }
    // The layout is applied before anything runs, so a command that reacts to
    // its pane's size sees the size it will keep.
    if (!described.layout.empty()) {
      if (const auto arranged = (*windows)[index].select_layout(described.layout);
          !arranged.has_value()) {
        return libtmux::unexpected(BuildError{index, arranged.error().diagnostic});
      }
    }
    // And the ones that cannot be set until the panes exist:
    // `synchronize-panes` set before the splits types into panes that are
    // still being made.
    for (const auto& [option, value] : described.options_after) {
      if (const auto set = (*windows)[index].set_option(option, value);
          !set.has_value()) {
        return libtmux::unexpected(BuildError{index, set.error().diagnostic});
      }
    }
    for (std::size_t pane = 0; pane < described.panes.size(); ++pane) {
      const libtmux::Pane& target = (*panes)[pane];
      for (const Command& command : described.panes[pane].shell_commands) {
        if (command.pause_before.count() != 0) {
          std::this_thread::sleep_for(command.pause_before);
        }
        // An empty command is a carriage return, which is what a tmuxp
        // document means by one: the pane is left at a fresh prompt.
        if (!command.text.empty()) {
          const std::string typed_text =
              command.suppress_history ? " " + command.text : command.text;
          if (const auto typed = target.send_text(typed_text); !typed.has_value()) {
            return libtmux::unexpected(BuildError{index, typed.error().diagnostic});
          }
        }
        if (!command.enter) {
          if (command.pause_after.count() != 0) {
            std::this_thread::sleep_for(command.pause_after);
          }
          continue;
        }
        if (const auto entered = target.send_key("Enter"); !entered.has_value()) {
          return libtmux::unexpected(BuildError{index, entered.error().diagnostic});
        }
        if (command.pause_after.count() != 0) {
          std::this_thread::sleep_for(command.pause_after);
        }
      }
      if (described.panes[pane].focus) {
        if (const auto selected = target.select(); !selected.has_value()) {
          return libtmux::unexpected(BuildError{index, selected.error().diagnostic});
        }
      }
    }
  }

  // Last, so selecting a pane in a later window cannot leave that window
  // active over the one the description asked for.
  for (std::size_t index = 0; index < description.windows.size(); ++index) {
    if (!description.windows[index].focus) {
      continue;
    }
    if (const auto selected = (*windows)[index].select(); !selected.has_value()) {
      return libtmux::unexpected(BuildError{index, selected.error().diagnostic});
    }
  }
  return *built;
}

} // namespace libtmux::workspace
