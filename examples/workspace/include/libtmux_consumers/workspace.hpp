#pragma once

// Build a described workspace on a real tmux server.
//
// This is a consumer, not part of the library: it exists to put weight on the
// public surface and report where that surface is awkward. It takes a
// workspace as data rather than as YAML, because parsing a config file is a
// serialization concern that belongs in an opt-in integration — the shape
// below is what a tmuxp document would deserialize into.

#include <charconv>
#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libtmux/server.hpp"
#include "libtmux/target.hpp"
#include "libtmux_consumers/layout.hpp"

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
  // An explicit empty mapping also replaces the window-level environment.
  bool environment_overrides{false};
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
  // Default launcher for every pane without its own shell.
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
  std::vector<std::string> retained_windows{};
};

// Runs after session selection and before settings or windows. A returned error
// uses the builder's owned-session rollback and borrowed-session preservation.
using BeforeBuild = std::function<std::optional<std::string>(const libtmux::Session&)>;

enum class BuildPhase {
  window_started,
  pane_started,
  pane_completed,
  window_completed,
  waiting
};
struct BuildEvent {
  BuildPhase phase;
  std::size_t window_index;
  std::size_t pane_index;
};
// A refusal uses owned-session rollback or reports retained borrowed windows.
using BuildObserver = std::function<std::optional<std::string>(const BuildEvent&)>;

// Check all layouts before sessions, settings, or before-build callbacks change.
inline std::optional<BuildError> validate_layouts(const Server& server,
                                                  const Workspace& description) {
  std::optional<bool> mirrored;
  for (std::size_t index = 0; index < description.windows.size(); ++index) {
    const auto& window = description.windows[index];
    if (auto error = detail::layout_error(window.layout, window.panes.size()))
      return BuildError{index, std::move(*error)};
    if (!detail::layout_needs_version(window.layout))
      continue;
    if (!mirrored.has_value()) {
      // An existing daemon can differ from the client found on PATH. A cold
      // endpoint has no format context, so use the client that will start it.
      const auto running = server.run({"display-message", "-p", "#{version}"});
      if (running) {
        const auto version = parse_version("tmux " + *running);
        if (!version)
          return BuildError{index, "cannot determine tmux daemon version for layout"};
        mirrored = *version >= Version{.major = 3, .minor = 5};
      } else {
        const auto& failure = running.error();
        // The public socket path can be replaced; classify the retained route's
        // native missing state or tmux's ECONNREFUSED diagnostic.
        const bool cold =
            (failure.kind == FailureKind::missing &&
             failure.delivery == DeliveryStatus::not_started) ||
            (failure.kind == FailureKind::refused &&
             failure.delivery == DeliveryStatus::replied && failure.exit_code == 1 &&
             failure.diagnostic.starts_with("no server running on ") &&
             failure.diagnostic.ends_with(" (running: display-message -p #{version})"));
        if (!cold)
          return BuildError{index, failure.diagnostic};
        const auto version = server.tmux_version();
        if (!version)
          return BuildError{index, version.error().diagnostic};
        mirrored = *version >= Version{.major = 3, .minor = 5};
      }
    }
    if (auto error = detail::layout_error(window.layout, window.panes.size(), mirrored))
      return BuildError{index, std::move(*error)};
  }
  return std::nullopt;
}

namespace detail {
[[nodiscard]] inline libtmux::expected<libtmux::Session, BuildError>
build_windows(const Server& server, const Workspace& description,
              const std::optional<libtmux::Session>& borrowed,
              const BeforeBuild& before, const BuildObserver& observer) {
  if (description.session_name.empty() || description.windows.empty()) {
    return libtmux::unexpected(BuildError{0, "workspace names no session or window"});
  }
  const auto session = session_target(description.session_name);
  if (!session.has_value()) {
    return libtmux::unexpected(BuildError{0, "session name cannot address itself"});
  }

  for (std::size_t index = 0; index < description.windows.size(); ++index) {
    if (description.windows[index].panes.empty()) {
      return libtmux::unexpected(BuildError{index, "a window needs at least one pane"});
    }
  }
  if (auto error = validate_layouts(server, description))
    return libtmux::unexpected(std::move(*error));
  const auto directory = [&description](const Window& window, const Pane& pane) {
    if (!pane.start_directory.empty()) {
      return pane.start_directory;
    }
    return window.start_directory.empty() ? description.start_directory
                                          : window.start_directory;
  };
  const auto environment = [](const Window& window, const Pane& pane) {
    return pane.environment_overrides || !pane.environment.empty() ? pane.environment
                                                                   : window.environment;
  };
  const auto shell = [](const Window& window, const Pane& pane) {
    return pane.shell.empty() ? window.shell : pane.shell;
  };
  auto initial_environment = description.environment;
  if (before)
    initial_environment.clear();
  auto built = borrowed
                   ? libtmux::expected<libtmux::Session, CommandFailure>{*borrowed}
                   : server.new_session({.name = description.session_name,
                                         .start_directory = description.start_directory,
                                         .environment = initial_environment});
  if (!built.has_value()) {
    return libtmux::unexpected(BuildError{0, built.error().diagnostic});
  }
  std::vector<std::string> created_windows;
  const auto fail =
      [&built, &borrowed, &created_windows](
          std::size_t index,
          std::string reason) -> libtmux::expected<libtmux::Session, BuildError> {
    if (borrowed)
      return libtmux::unexpected(BuildError{index, std::move(reason), created_windows});
    if (const auto killed = built->kill(); !killed.has_value()) {
      reason += "; session cleanup failed: " + killed.error().diagnostic;
    }
    return libtmux::unexpected(BuildError{index, std::move(reason)});
  };
  const auto notify = [&](BuildPhase phase, std::size_t window,
                          std::size_t pane) -> std::optional<BuildError> {
    if (!observer)
      return std::nullopt;
    try {
      if (auto reason = observer({phase, window, pane}))
        return fail(window, std::move(*reason)).error();
    } catch (const std::exception& error) {
      return fail(window, error.what()).error();
    } catch (...) {
      return fail(window, "build observer failed").error();
    }
    return std::nullopt;
  };
  const auto pause = [&](std::chrono::milliseconds duration, std::size_t window,
                         std::size_t pane) -> std::optional<BuildError> {
    if (!observer) {
      std::this_thread::sleep_for(duration);
      return std::nullopt;
    }
    const auto until = std::chrono::steady_clock::now() + duration;
    do {
      if (auto error = notify(BuildPhase::waiting, window, pane))
        return error;
      std::this_thread::sleep_until(std::min(until, std::chrono::steady_clock::now() +
                                                        std::chrono::milliseconds{20}));
    } while (std::chrono::steady_clock::now() < until);
    return notify(BuildPhase::waiting, window, pane);
  };
  if (auto error = notify(BuildPhase::waiting, 0, 0))
    return libtmux::unexpected(std::move(*error));
  if (before) {
    try {
      if (auto error = before(*built))
        return fail(0, std::move(*error));
    } catch (...) {
      (void)fail(0, "before-build callback failed");
      throw;
    }
  }
  std::optional<libtmux::Window> bootstrap;
  if (!borrowed) {
    const auto window = built->active_window();
    if (!window)
      return fail(0, window.error().diagnostic);
    bootstrap = *window;
  }
  if (borrowed || before) {
    for (const auto& [name, value] : description.environment) {
      if (auto error = notify(BuildPhase::waiting, 0, 0))
        return libtmux::unexpected(std::move(*error));
      if (name.empty() || name.find('=') != std::string::npos)
        return fail(0, "an environment name must be non-empty and contain no '='");
      const auto set =
          server.run({"set-environment", "-t", std::string{built->id()}, name, value});
      if (!set)
        return fail(0, set.error().diagnostic);
    }
  }
  for (const auto& [option, value] : description.global_options) {
    if (auto error = notify(BuildPhase::waiting, 0, 0))
      return libtmux::unexpected(std::move(*error));
    if (const auto set = server.set_global_option(option, value); !set.has_value()) {
      return fail(0, set.error().diagnostic);
    }
  }
  for (const auto& [option, value] : description.options) {
    if (auto error = notify(BuildPhase::waiting, 0, 0))
      return libtmux::unexpected(std::move(*error));
    if (const auto set = built->set_option(option, value); !set.has_value()) {
      return fail(0, set.error().diagnostic);
    }
  }
  std::vector<libtmux::Window> windows;
  std::vector<std::vector<libtmux::Pane>> created_panes;
  for (std::size_t index = 0; index < description.windows.size(); ++index) {
    if (auto error = notify(BuildPhase::waiting, index, 0))
      return libtmux::unexpected(std::move(*error));
    const Window& window = description.windows[index];
    const Pane& first = window.panes.front();
    std::vector<std::string> command{
        "new-window", "-d", "-P", "-F", "#{window_id}", "-t", std::string{built->id()}};
    if ((borrowed || index != 0) && window.index.has_value()) {
      command.back() += ":" + std::to_string(*window.index);
    }
    if (!window.name.empty()) {
      command.insert(command.end(), {"-n", window.name});
    }
    if (const auto path = directory(window, first); !path.empty()) {
      command.insert(command.end(), {"-c", path});
    }
    for (const auto& [name, value] : environment(window, first)) {
      if (auto error = notify(BuildPhase::waiting, index, 0))
        return libtmux::unexpected(std::move(*error));
      if (name.empty() || name.find('=') != std::string::npos) {
        return fail(index, "an environment name must be non-empty and contain no '='");
      }
      command.insert(command.end(), {"-e", name + "=" + value});
    }
    if (const auto launcher = shell(window, first); !launcher.empty()) {
      command.insert(command.end(), {"--", launcher});
    }
    auto created = server.run(command);
    if (!created.has_value()) {
      return fail(index, created.error().diagnostic);
    }
    while (!created->empty() && (created->back() == '\n' || created->back() == '\r')) {
      created->pop_back();
    }
    created_windows.push_back(*created);
    const auto target = server.window(std::string{built->id()} + ":" + *created);
    if (!target.has_value()) {
      return fail(index, target.error().diagnostic);
    }
    windows.push_back(*target);
    if (index == 0 && !borrowed) {
      const auto base = built->option("base-index");
      if (!base.has_value()) {
        return fail(index, base.error().diagnostic);
      }
      long long desired{};
      const auto parsed = std::from_chars(
          base->value.data(), base->value.data() + base->value.size(), desired);
      if (parsed.ec != std::errc{}) {
        return fail(index, "tmux returned an invalid base-index");
      }
      desired = window.index.value_or(desired);
      if (const auto killed = bootstrap->kill(); !killed.has_value()) {
        return fail(index, killed.error().diagnostic);
      }
      if (target->index() != desired) {
        const auto moved =
            server.run({"move-window", "-s", target->target(), "-t",
                        std::string{built->id()} + ":" + std::to_string(desired)});
        if (!moved.has_value()) {
          return fail(index, moved.error().diagnostic);
        }
      }
    }
    for (const auto& [option, value] : window.options) {
      if (auto error = notify(BuildPhase::waiting, index, 0))
        return libtmux::unexpected(std::move(*error));
      if (const auto set = target->set_option(option, value); !set.has_value()) {
        return fail(index, set.error().diagnostic);
      }
    }
    const auto first_pane = target->active_pane();
    if (!first_pane.has_value()) {
      return fail(index, first_pane.error().diagnostic);
    }
    std::vector<libtmux::Pane> panes{*first_pane};
    for (std::size_t pane = 1; pane < window.panes.size(); ++pane) {
      if (auto error = notify(BuildPhase::waiting, index, pane))
        return libtmux::unexpected(std::move(*error));
      const auto& planned = window.panes[pane];
      const auto split = target->split({.start_directory = directory(window, planned),
                                        .shell_command = shell(window, planned),
                                        .environment = environment(window, planned)});
      if (!split.has_value()) {
        return fail(index, split.error().diagnostic);
      }
      panes.push_back(*split);
      if (const auto arranged = target->select_layout("tiled"); !arranged.has_value()) {
        return fail(index, arranged.error().diagnostic);
      }
    }
    created_panes.push_back(std::move(panes));
  }

  // Commands run after every pane exists, so an earlier window's command
  // cannot race the creation of a later one. Each pane is addressed by the id
  // tmux gave it: a `session:window.0` path assumes a pane numbering that
  // `pane-base-index` is free to change under the caller.
  for (std::size_t index = 0; index < description.windows.size(); ++index) {
    const Window& described = description.windows[index];
    const auto& panes = created_panes[index];
    if (auto error = notify(BuildPhase::window_started, index, 0))
      return libtmux::unexpected(std::move(*error));
    // The layout is applied before anything runs, so a command that reacts to
    // its pane's size sees the size it will keep.
    if (!described.layout.empty()) {
      if (const auto arranged = windows[index].select_layout(described.layout);
          !arranged.has_value()) {
        return fail(index, arranged.error().diagnostic);
      }
    }
    for (std::size_t pane = 0; pane < described.panes.size(); ++pane) {
      const libtmux::Pane& target = panes[pane];
      if (auto error = notify(BuildPhase::pane_started, index, pane))
        return libtmux::unexpected(std::move(*error));
      for (const Command& command : described.panes[pane].shell_commands) {
        if (auto error = notify(BuildPhase::waiting, index, pane))
          return libtmux::unexpected(std::move(*error));
        if (command.pause_before.count() != 0) {
          if (auto error = pause(command.pause_before, index, pane))
            return libtmux::unexpected(std::move(*error));
        }
        // An empty command is a carriage return, which is what a tmuxp
        // document means by one: the pane is left at a fresh prompt.
        if (!command.text.empty()) {
          const std::string typed_text =
              command.suppress_history ? " " + command.text : command.text;
          if (const auto typed = target.send_text(typed_text); !typed.has_value()) {
            return fail(index, typed.error().diagnostic);
          }
        }
        if (!command.enter) {
          if (command.pause_after.count() != 0) {
            if (auto error = pause(command.pause_after, index, pane))
              return libtmux::unexpected(std::move(*error));
          }
          continue;
        }
        if (const auto entered = target.send_key("Enter"); !entered.has_value()) {
          return fail(index, entered.error().diagnostic);
        }
        if (command.pause_after.count() != 0) {
          if (auto error = pause(command.pause_after, index, pane))
            return libtmux::unexpected(std::move(*error));
        }
      }
      if (described.panes[pane].focus) {
        if (const auto selected = target.select(); !selected.has_value()) {
          return fail(index, selected.error().diagnostic);
        }
      }
      if (auto error = notify(BuildPhase::pane_completed, index, pane))
        return libtmux::unexpected(std::move(*error));
    }
    for (const auto& [option, value] : described.options_after) {
      if (auto error = notify(BuildPhase::waiting, index, 0))
        return libtmux::unexpected(std::move(*error));
      if (const auto set = windows[index].set_option(option, value); !set.has_value()) {
        return fail(index, set.error().diagnostic);
      }
    }
    if (auto error =
            notify(BuildPhase::window_completed, index, described.panes.size() - 1))
      return libtmux::unexpected(std::move(*error));
  }

  // Last, so selecting a pane in a later window cannot leave that window
  // active over the one the description asked for.
  for (std::size_t index = 0; index < description.windows.size(); ++index) {
    if (auto error = notify(BuildPhase::waiting, index, 0))
      return libtmux::unexpected(std::move(*error));
    if (!description.windows[index].focus) {
      continue;
    }
    if (const auto selected = windows[index].select(); !selected.has_value()) {
      return fail(index, selected.error().diagnostic);
    }
  }
  if (auto error = notify(BuildPhase::waiting, description.windows.size() - 1, 0))
    return libtmux::unexpected(std::move(*error));
  return *built;
}

} // namespace detail

// Create a new session. A failed build removes only the session it created.
[[nodiscard]] inline libtmux::expected<libtmux::Session, BuildError>
build(const Server& server, const Workspace& description,
      const BeforeBuild& before = {}, const BuildObserver& observer = {}) {
  return detail::build_windows(server, description, std::nullopt, before, observer);
}

// Add windows to a borrowed session. Failure retains that session, applied
// settings and any new windows, whose IDs are included in the error.
[[nodiscard]] inline libtmux::expected<libtmux::Session, BuildError>
append(const libtmux::Session& session, const Workspace& description,
       const BeforeBuild& before = {}, const BuildObserver& observer = {}) {
  const auto server = session.server();
  if (!server)
    return libtmux::unexpected(BuildError{0, server.error().diagnostic});
  return detail::build_windows(*server, description, session, before, observer);
}

} // namespace libtmux::workspace
