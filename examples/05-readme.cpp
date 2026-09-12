// Every C++ example in the top-level README, compiled and run.
//
// A README is the most-read file in a repository and the least-tested, so the
// snippets in it live here instead: each `#region` below is quoted verbatim by
// README.md, and `tools/docs/check_readme.py` fails the build if the two ever
// disagree. A code sample that no longer compiles is a bug that greets every
// new reader, and this is the cheapest way to never ship one.
//
// It runs against a tmux server of its own, like every other example here.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>

#include "scratch_server.hpp"

int main() {
  const example::ScratchServer scratch = example::ScratchServer::open();
  const libtmux::Server& server = scratch.get();

  // #region connect
  // No tmux failure is thrown. Every call answers with a value that is either
  // the result or the reason there isn't one.
  const auto sessions = server.sessions();
  if (!sessions.has_value()) {
    std::cerr << std::format("{}\n", sessions.error());
    return 1;
  }

  for (const libtmux::Session& session : *sessions) {
    std::cout << std::format("{} has {} window(s)\n", session.name(),
                             session.window_count());
  }
  // #endregion connect

  const libtmux::Session& session = sessions->at(0);

  // #region build
  // Build an arrangement without composing a single tmux argument.
  const auto editor = session.new_window({.name = "editor"});
  if (!editor.has_value()) {
    std::cerr << std::format("{}\n", editor.error());
    return 1;
  }

  const auto logs = editor->split({.horizontal = true, .percentage = 30});
  if (!logs.has_value()) {
    std::cerr << std::format("{}\n", logs.error());
    return 1;
  }

  // Text and Enter in one invocation, which is what running a command in a
  // pane means. `send_text` and `send_key` stay available for the times the
  // two halves are separate acts.
  (void)logs->send_line("journalctl -f");
  // #endregion build

  // Something for the filter below to actually find, so the snippet the README
  // opens with runs its action rather than only compiling. tmux names a pane by
  // the program running in it, so a link called `nvim` stands in for an editor
  // without needing one installed.
  const auto editor_command = [] {
    const auto sleeper = std::filesystem::exists("/bin/sleep")
                             ? std::filesystem::path{"/bin/sleep"}
                             : std::filesystem::path{"/usr/bin/sleep"};
    // The link's own name is what tmux will report, so it has to be exactly
    // `nvim`; a unique directory keeps that name free.
    const auto directory = std::filesystem::temp_directory_path() /
                           ("libtmux-cxx-editor-" + std::to_string(::getpid()));
    std::error_code failed;
    std::filesystem::create_directories(directory, failed);
    const auto link = directory / "nvim";
    std::filesystem::remove(link, failed);
    std::filesystem::create_symlink(sleeper, link, failed);
    return failed ? std::string{} : link.string() + " 300";
  }();
  if (!editor_command.empty()) {
    const auto editing_window =
        session.new_window({.name = "editing", .shell_command = editor_command});
    if (!editing_window.has_value()) {
      std::cerr << std::format("{}\n", editing_window.error());
      return 1;
    }
    // tmux names the pane after whatever is running in it, and for a moment
    // that is still the shell on its way to exec. Wait for the name to settle,
    // or the filter below looks for an editor before there is one.
    const auto settled_by = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < settled_by) {
      const auto panes = editing_window->panes();
      if (panes.has_value() && !panes->empty() &&
          panes->at(0).command().starts_with("nv")) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
  }

  // #region intro
  // Every call answers with a value: the result, or the reason there is none.
  const auto panes = server.panes();
  if (!panes.has_value()) {
    std::cerr << std::format("{}\n", panes.error());
    return 1;
  }

  // Find the active pane running an editor, and press Escape in it.
  auto editing = *panes | libtmux::matching(libtmux::pane::command.starts_with("nv") &&
                                            libtmux::pane::active);

  if (auto pane = libtmux::first(editing)) {
    (void)pane->get().send_key("Escape");
  }
  // #endregion intro

  // #region query
  // A filter is a value built from typed fields, not a string tmux parses.
  // They compose with `&&`, `||` and `!`, and the result is a standard range.
  const auto interesting =
      (libtmux::pane::command == "bash" || libtmux::pane::command == "zsh") &&
      !libtmux::pane::dead;

  for (const libtmux::Pane& shell : *panes | libtmux::matching(interesting)) {
    std::cout << std::format("{} is a live shell, {} columns wide\n", shell.id(),
                             shell.width());
  }

  // An expression owns what it compares against, so this one still works
  // after the string it was built from has gone out of scope.
  const auto by_name = [] {
    const std::string wanted = std::string{"edi"} + "tor";
    return libtmux::window::name == wanted;
  }();

  const auto windows = server.windows();
  if (windows.has_value()) {
    const auto found = std::ranges::distance(*windows | libtmux::matching(by_name));
    std::cout << std::format("{} window(s) called editor\n", found);
  }
  // #endregion query

  // #region project
  // A handle reads a row, so the name that filters also projects, and a flag
  // handle is a predicate on its own. The standard algorithms take them as
  // they are, with no lambda naming the accessor a second time.
  if (windows.has_value() && !windows->empty()) {
    std::vector<libtmux::Window> ordered = *windows;
    std::ranges::sort(ordered, {}, libtmux::window::index);

    const auto active = std::ranges::count_if(ordered, libtmux::window::active);
    const libtmux::Window widest =
        std::ranges::max(ordered, {}, libtmux::window::width);
    std::cout << std::format("{} of {} active, widest {}\n", active, ordered.size(),
                             widest);
  }
  // #endregion project

  // #region cardinality
  // "Exactly one, or say why not" is a question the library answers directly,
  // rather than one every caller reimplements around `.size() == 1`.
  auto addressed = *panes | libtmux::matching(libtmux::pane::id == panes->at(0).id());

  if (const auto one = libtmux::exactly_one(addressed); one.has_value()) {
    std::cout << std::format("exactly one: {}\n", one->get());
  }

  // And when it is not one, the answer says which way it went wrong.
  auto absent = *panes | libtmux::matching(libtmux::pane::command == "no-such-command");

  if (const auto none = libtmux::exactly_one(absent); !none.has_value()) {
    std::cout << std::format("not one: {}\n", libtmux::to_string(none.error()));
  }
  // #endregion cardinality

  // #region capture
  // Read a pane's visible contents, or its scrollback.
  const libtmux::Pane& pane = panes->at(0);

  const auto visible = pane.capture();
  if (visible.has_value()) {
    std::cout << std::format("{} bytes on screen\n", visible->size());
  }

  const auto history = pane.capture({.whole_history = true});
  if (history.has_value()) {
    std::cout << std::format("{} bytes of scrollback\n", history->size());
  }
  // #endregion capture

  // #region traverse
  // Every entity knows the server it came from, so it can reach its children
  // and its parents without a target string.
  const auto window = pane.window();
  const auto owner = pane.session();
  if (window.has_value() && owner.has_value()) {
    std::cout << std::format("{} is in {}, in {}\n", pane, *window, *owner);
  }
  // #endregion traverse

  // #region snapshot
  // An entity is one row of the listing that produced it: a moment, not a
  // live handle. Ask again for the present.
  (void)editor->rename("renamed");

  std::cout << std::format("held: {}\n", editor->name()); // still "editor"

  const auto now = editor->refresh();
  if (now.has_value()) {
    std::cout << std::format("now: {}\n", now->name()); // "renamed"
  }
  // #endregion snapshot

  // #region errors
  // Failures are values with a kind, so a caller can tell "you asked wrongly"
  // from "tmux said no" from "tmux never answered".
  const auto gone = server.run({"kill-session", "-t", "=no-such-session"});
  if (!gone.has_value()) {
    switch (gone.error().kind) {
    case libtmux::FailureKind::validation:
      std::cout << "the request was malformed before it was sent\n";
      break;
    case libtmux::FailureKind::unsupported:
      std::cout << "this backend cannot provide the operation safely\n";
      break;
    case libtmux::FailureKind::refused:
      std::cout << std::format("tmux refused it: {}\n", gone.error().diagnostic);
      break;
    case libtmux::FailureKind::timeout:
      std::cout << "tmux did not answer in time\n";
      break;
    default:
      std::cout << std::format("{}\n", gone.error());
      break;
    }
  }
  // #endregion errors

  // #region compose
  // One failure type covers the whole surface, so calls compose rather than
  // nest: each step runs only when the last one answered, and the first
  // failure is what comes out.
  const auto columns =
      server.session(session.name())
          .and_then([](const libtmux::Session& found) { return found.active_pane(); })
          .transform([](const libtmux::Pane& active) { return active.width(); });
  std::cout << std::format("the active pane is {} columns wide\n",
                           columns.value_or(-1));
  // #endregion compose

  // #region async
  std::size_t observed = 0U;
  auto async_server = libtmux::Server::at_socket_path(
      scratch.socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  if (!async_server.has_value()) {
    std::cerr << std::format("{}\n", async_server.error());
    return 1;
  }

  auto started_runtime =
      libtmux::CommandRuntime::start(libtmux::CommandRuntimeConfig{.capacity = 1U});
  if (!started_runtime.has_value()) {
    std::cerr << std::format("{}\n", started_runtime.error());
    return 1;
  }
  auto runtime = *std::move(started_runtime);
  const auto wait_for_completion = [&runtime](std::uint64_t wanted) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (runtime.snapshot().completed < wanted &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return runtime.snapshot().completed >= wanted;
  };

  auto submitted =
      async_server->try_submit(runtime, {"display-message", "-p", "async result"});
  if (!submitted.has_value()) { // Refused before admission.
    std::cerr << std::format("{}\n", submitted.error());
    return 1;
  }
  auto result = std::move(*submitted).wait();
  if (!result.has_value()) { // Failed after admission.
    std::cerr << std::format("{}\n", result.error());
    return 1;
  }
  if (!wait_for_completion(1U)) {
    return 1;
  }

  const auto held = runtime.snapshot();
  std::cout << std::format("{}/{} slot(s), {} observation(s) pending\n", held.in_flight,
                           held.capacity, held.pending_observers);
  std::cout << std::format("dispatched {} observation(s)\n", runtime.dispatch_ready());

  auto detached =
      async_server->try_submit(runtime, {"display-message", "-p", "detached"});
  if (!detached.has_value()) {
    std::cerr << std::format("{}\n", detached.error());
    return 1;
  }
  std::move(*detached).detach(); // Keep no result; the observation remains.
  if (!wait_for_completion(2U)) {
    return 1;
  }
  std::cout << std::format("discarded {} observation(s)\n", runtime.discard_ready());

  const auto shutdown = runtime.close();
  if (shutdown.failure.has_value()) {
    std::cerr << std::format("{}\n", *shutdown.failure);
    return 1;
  }
  std::cout << std::format("runtime stopped: {}; safe to unload: {}; observed: {}\n",
                           shutdown.transports_stopped, shutdown.safe_to_unload,
                           observed);
  if (!shutdown.transports_stopped || !shutdown.safe_to_unload || observed != 1U) {
    return 1;
  }
  // #endregion async

  // #region escape
  // Anything tmux knows and this library does not name yet: ask it directly,
  // with a format string expanded against a pane.
  const auto running = pane.expand("#{pane_current_command}");
  if (running.has_value()) {
    std::cout << std::format("running {}\n", *running);
  }

  // Or run a command and read its output.
  const auto answer = server.run({"display-message", "-p", "#{version}"});
  if (answer.has_value()) {
    std::cout << std::format("tmux {}", *answer); // tmux's answer ends in a newline
  }
  // #endregion escape

  // #region options
  // Options are read and written where tmux scopes them.
  (void)session.set_option("@project", "libtmux");

  const auto project = session.option("@project");
  if (project.has_value()) {
    std::cout << std::format("@project is {}\n", project->value);
  }
  // #endregion options

  // #region chain
  // A chain refuses a target it cannot address before reaching tmux at all,
  // so a malformed batch costs nothing.
  libtmux::Chain chain;
  chain.new_window("a:b", "unreachable");
  std::cout << std::format("chain valid: {}\n", chain.valid()); // false
  // #endregion chain

  // #region fixture
  // A private tmux for a suite of your own, gone when the scope ends.
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("my-suite")});
  if (!fixture.has_value()) {
    std::cerr << std::format("{}\n", fixture.error());
    return 1;
  }
  const auto under_test =
      libtmux::Server::at_socket_path(fixture->socket_path().string());
  std::cout << std::format("sessions on it: {}\n", under_test->sessions()->size());
  // #endregion fixture

  // The stand-in editor's directory, which the scratch server does not own.
  std::error_code cleanup;
  std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                                  ("libtmux-cxx-editor-" + std::to_string(::getpid())),
                              cleanup);
  return 0;
}
