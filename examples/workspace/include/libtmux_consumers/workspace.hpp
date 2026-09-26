#pragma once

// Build a described workspace on a real tmux server.
//
// This is a consumer, not part of the library: it exists to put weight on the
// public surface and report where that surface is awkward. It takes a
// workspace as data rather than as YAML, because parsing a config file is a
// serialization concern that belongs in an opt-in integration — the shape
// below is what a tmuxp document would deserialize into.
//
// The `tmux-workspace` CLI links this builder rather than carrying one of its
// own, so there is a single implementation of what a workspace means here:
// the rebalance between splits, the wait for a pane's prompt, the rollback of
// a session a failed build created. A fix to any of them is a fix for both.
// It is not installed with the library; building it means building this repo.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtmux/expected.hpp"
#include "libtmux/format.hpp"
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
  // What the document says that this builds around rather than refuses: a
  // setting it does not act on, a path that is not there. Each is one
  // sentence for a reader, in document order.
  std::vector<std::string> warnings{};
};

struct BuildError {
  // Which window was being built, so a failure points at the description
  // rather than at an opaque tmux message.
  std::size_t window_index{};
  std::string reason;
  std::vector<std::string> retained_windows{};
};

// What a callback says when it stops a build: the sentence for the caller,
// and whether what the build has made so far is kept. A cancellation keeps
// it -- tidying up after a signal can itself be interrupted, and destroying a
// session on a keystroke is worse than leaving one behind. A failure does
// not: a session this build created and could not finish is removed.
struct BuildStop {
  std::string reason;
  bool retain{false};
  BuildStop(const char* text) : reason{text} {}
  BuildStop(std::string text, bool keep = false)
      : reason{std::move(text)}, retain{keep} {}
};

// Runs after session selection and before settings or windows. A returned stop
// uses the builder's owned-session rollback and borrowed-session preservation.
using BeforeBuild = std::function<std::optional<BuildStop>(const libtmux::Session&)>;

enum class BuildPhase {
  session_started,
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
  // Populated for the five named phases; empty for waiting.
  std::string session_id{};
  std::string window_id{};
  std::string pane_id{};
};
// A refusal uses owned-session rollback or reports retained borrowed windows.
using BuildObserver = std::function<std::optional<BuildStop>(const BuildEvent&)>;

// Check all layouts before sessions, settings, or before-build callbacks change.
inline std::optional<BuildError> validate_layouts(const Server& server,
                                                  const Workspace& description) {
  std::vector<LayoutRequest> layouts;
  std::vector<std::size_t> indexes;
  layouts.reserve(description.windows.size());
  indexes.reserve(description.windows.size());
  for (std::size_t index = 0; index < description.windows.size(); ++index) {
    const auto& window = description.windows[index];
    if (!window.layout.empty()) {
      layouts.push_back({window.layout, window.panes.size()});
      indexes.push_back(index);
    }
  }
  if (auto checked = server.validate_layouts(layouts); !checked) {
    auto& error = checked.error();
    return BuildError{indexes[error.index], std::move(error.cause.diagnostic)};
  }
  return std::nullopt;
}

namespace detail {
// Drops the " (running: ...)" command-line suffix a diagnostic carries.
inline std::string sentence(std::string_view diagnostic) {
  const auto marker = diagnostic.rfind(" (running: ");
  return std::string{marker == std::string_view::npos ? diagnostic
                                                      : diagnostic.substr(0, marker)};
}
[[nodiscard]] inline libtmux::expected<libtmux::Session, BuildError>
build_windows(const Server& server, const Workspace& description,
              const std::optional<libtmux::Session>& borrowed,
              const BeforeBuild& before, const BuildObserver& observer,
              std::optional<int> width = std::nullopt,
              std::optional<int> height = std::nullopt) {
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
  // A session built with an explicit size stays that size until a client
  // attaches, so every window laid out before then — not only the one a
  // client happens to focus first — is built at the real terminal size
  // instead of tmux's `default-size` (80x24 unless the user changed it).
  auto built = borrowed
                   ? libtmux::expected<libtmux::Session, CommandFailure>{*borrowed}
                   : server.new_session({.name = description.session_name,
                                         .start_directory = description.start_directory,
                                         .width = width,
                                         .height = height,
                                         .environment = initial_environment});
  if (!built.has_value()) {
    return libtmux::unexpected(BuildError{0, built.error().diagnostic});
  }
  std::vector<std::string> created_windows;
  const auto fail = [&built, &borrowed, &created_windows](
                        std::size_t index, std::string reason,
                        bool retain =
                            false) -> libtmux::expected<libtmux::Session, BuildError> {
    reason = sentence(reason);
    if (borrowed || retain)
      return libtmux::unexpected(BuildError{index, std::move(reason), created_windows});
    if (const auto killed = built->kill(); !killed.has_value()) {
      reason += "; session cleanup failed: " + sentence(killed.error().diagnostic);
    }
    return libtmux::unexpected(BuildError{index, std::move(reason)});
  };
  const auto notify = [&](BuildPhase phase, std::size_t window, std::size_t pane,
                          std::string session_id = {}, std::string window_id = {},
                          std::string pane_id = {}) -> std::optional<BuildError> {
    if (!observer)
      return std::nullopt;
    try {
      if (auto stop = observer({phase, window, pane, std::move(session_id),
                                std::move(window_id), std::move(pane_id)}))
        return fail(window, std::move(stop->reason), stop->retain).error();
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
  // A freshly spawned pane echoes a command typed into it before its shell has
  // drawn a prompt, and the shell then redraws the prompt over it, so the
  // command appears twice. Wait for the cursor to leave the origin, which is
  // where a pane starts and where its shell leaves it until the prompt is
  // drawn. The window's layout is still settling at that point (applied once
  // for the whole window, ahead of every pane in it), and a resize the shell
  // answers with its own redraw looks identical to the initial draw, so the
  // position also has to hold steady for a few polls before it counts as
  // settled. Give up and send anyway past the deadline, as tmuxp does.
  const auto ready = [&](std::size_t window, std::size_t pane,
                         const libtmux::Pane& target) -> std::optional<BuildError> {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
    std::string settled;
    int streak{};
    do {
      if (auto error = notify(BuildPhase::waiting, window, pane))
        return error;
      const auto cursor = target.expand("#{cursor_x},#{cursor_y}");
      // A query that fails says nothing about the prompt, so it is retried to
      // the deadline rather than taken as permission to send at once.
      if (!cursor) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        continue;
      }
      if (*cursor == "0,0") {
        settled.clear();
        streak = 0;
      } else if (*cursor == settled) {
        if (++streak >= 3)
          return std::nullopt;
      } else {
        settled = *cursor;
        streak = 1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    } while (std::chrono::steady_clock::now() < until);
    return std::nullopt;
  };
  // Reported as soon as the session exists -- before before_script, before
  // any window -- so a caller that only needs the new session's id is not
  // left waiting for the whole build.
  if (auto error =
          notify(BuildPhase::session_started, 0, 0, std::string{built->id().value()}))
    return libtmux::unexpected(std::move(*error));
  if (auto error = notify(BuildPhase::waiting, 0, 0))
    return libtmux::unexpected(std::move(*error));
  if (before) {
    try {
      if (auto stop = before(*built))
        return fail(0, std::move(stop->reason), stop->retain);
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
      const auto set = server.run(
          {"set-environment", "-t", std::string{built->id().value()}, name, value});
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
  // tmux keeps some options in its window table and routes a set against a
  // session to whichever window is current -- here the bootstrap window this
  // build is about to kill. Those are applied to each window below instead,
  // where the document that wrote them under the session can take effect.
  std::set<std::string> window_scoped;
  if (!description.options.empty()) {
    if (const auto listed = server.run({"show-options", "-w", "-g"})) {
      for (const auto line : *listed | std::views::split('\n')) {
        const std::string_view text{line.begin(), line.end()};
        if (const auto end = text.find(' '); end != 0 && !text.empty())
          window_scoped.emplace(text.substr(0, end));
      }
    }
  }
  for (const auto& [option, value] : description.options) {
    if (window_scoped.contains(option))
      continue;
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
    std::vector<std::string> command{"new-window",
                                     "-d",
                                     "-P",
                                     "-F",
                                     "#{window_id}",
                                     "-t",
                                     std::string{built->id().value()}};
    if ((borrowed || index != 0) && window.index.has_value()) {
      command.back() += ":" + std::to_string(*window.index);
    }
    // tmux expands formats in both, and both are text from a document, so
    // they are escaped here exactly as the library escapes them for the
    // split that makes every pane after the first.
    if (!window.name.empty()) {
      command.insert(command.end(), {"-n", escape_literal(window.name)});
    }
    if (const auto path = directory(window, first); !path.empty()) {
      command.insert(command.end(), {"-c", escape_literal(path)});
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
    const auto target =
        server.window(std::string{built->id().value()} + ":" + *created);
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
      // `renumber-windows on` re-indexes the session as the bootstrap window
      // closes, so the index this window held is read back after the kill.
      const auto placed =
          server.window(std::string{built->id().value()} + ":" + *created);
      if (!placed.has_value()) {
        return fail(index, placed.error().diagnostic);
      }
      if (placed->index() != desired) {
        const auto moved = server.run(
            {"move-window", "-s", placed->target(), "-t",
             std::string{built->id().value()} + ":" + std::to_string(desired)});
        if (!moved.has_value()) {
          return fail(index, moved.error().diagnostic);
        }
      }
    }
    // The session's window-scoped options first, so a window's own options
    // still have the last word on the window it owns.
    for (const auto& [option, value] : description.options) {
      if (!window_scoped.contains(option))
        continue;
      if (auto error = notify(BuildPhase::waiting, index, 0))
        return libtmux::unexpected(std::move(*error));
      if (const auto set = target->set_option(option, value); !set.has_value()) {
        return fail(index, set.error().diagnostic);
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
      // Split the pane just created, not the window: a window target stays
      // on the same original pane and reverses every pane after the first.
      const auto split =
          panes.back().split({.start_directory = directory(window, planned),
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
    if (auto error = notify(BuildPhase::window_started, index, 0,
                            std::string{built->id().value()},
                            std::string{windows[index].id().value()}))
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
      if (auto error = notify(BuildPhase::pane_started, index, pane,
                              std::string{built->id().value()},
                              std::string{windows[index].id().value()},
                              std::string{target.id().value()}))
        return libtmux::unexpected(std::move(*error));
      // A pane whose shell is replaced by a launcher command never draws an
      // interactive prompt, so there is nothing to wait for.
      if (!described.panes[pane].shell_commands.empty() &&
          shell(described, described.panes[pane]).empty()) {
        if (auto error = ready(index, pane, target))
          return libtmux::unexpected(std::move(*error));
      }
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
      if (auto error = notify(BuildPhase::pane_completed, index, pane,
                              std::string{built->id().value()},
                              std::string{windows[index].id().value()},
                              std::string{target.id().value()}))
        return libtmux::unexpected(std::move(*error));
    }
    // Nothing in this window asked for the cursor, so it is left in the pane
    // built last, where tmuxp leaves it. Selecting a pane does not move which
    // window the session is on, so this is per window and independent of it.
    if (std::ranges::none_of(described.panes,
                             [](const Pane& pane) { return pane.focus; })) {
      if (const auto selected = panes.back().select(); !selected.has_value()) {
        return fail(index, selected.error().diagnostic);
      }
    }
    for (const auto& [option, value] : described.options_after) {
      if (auto error = notify(BuildPhase::waiting, index, 0))
        return libtmux::unexpected(std::move(*error));
      if (const auto set = windows[index].set_option(option, value); !set.has_value()) {
        return fail(index, set.error().diagnostic);
      }
    }
    if (auto error = notify(
            BuildPhase::window_completed, index, described.panes.size() - 1,
            std::string{built->id().value()}, std::string{windows[index].id().value()}))
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
// `width`/`height` fix the session's size (tmux's `-x`/`-y`) until a client
// attaches; they are not workspace data, so the caller supplies them rather
// than the description carrying them.
[[nodiscard]] inline libtmux::expected<libtmux::Session, BuildError>
build(const Server& server, const Workspace& description,
      const BeforeBuild& before = {}, const BuildObserver& observer = {},
      std::optional<int> width = std::nullopt,
      std::optional<int> height = std::nullopt) {
  return detail::build_windows(server, description, std::nullopt, before, observer,
                               width, height);
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
