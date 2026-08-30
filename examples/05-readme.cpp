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
#include <cstdio>
#include <filesystem>
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
    std::fprintf(stderr, "%s\n", sessions.error().diagnostic.c_str());
    return 1;
  }

  for (const libtmux::Session& session : *sessions) {
    std::printf("%s has %lld window(s)\n", std::string{session.name()}.c_str(),
                session.window_count());
  }
  // #endregion connect

  const libtmux::Session& session = sessions->at(0);

  // #region build
  // Build an arrangement without composing a single tmux argument.
  const auto editor = session.new_window({.name = "editor"});
  if (!editor.has_value()) {
    std::fprintf(stderr, "%s\n", editor.error().diagnostic.c_str());
    return 1;
  }

  const auto logs = editor->split({.horizontal = true, .percentage = 30});
  if (!logs.has_value()) {
    std::fprintf(stderr, "%s\n", logs.error().diagnostic.c_str());
    return 1;
  }

  (void)logs->send_text("journalctl -f");
  (void)logs->send_key("Enter");
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
      std::fprintf(stderr, "%s\n", editing_window.error().diagnostic.c_str());
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
    std::fprintf(stderr, "%s\n", panes.error().diagnostic.c_str());
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
    std::printf("%s is a live shell, %lld columns wide\n",
                std::string{shell.id()}.c_str(), shell.width());
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
    std::printf("%td window(s) called editor\n", found);
  }
  // #endregion query

  // #region cardinality
  // "Exactly one, or say why not" is a question the library answers directly,
  // rather than one every caller reimplements around `.size() == 1`.
  auto addressed = *panes | libtmux::matching(libtmux::pane::id == panes->at(0).id());

  if (const auto one = libtmux::exactly_one(addressed); one.has_value()) {
    std::printf("exactly one: %s\n", std::string{one->get().id()}.c_str());
  }

  // And when it is not one, the answer says which way it went wrong.
  auto absent = *panes | libtmux::matching(libtmux::pane::command == "no-such-command");

  if (const auto none = libtmux::exactly_one(absent); !none.has_value()) {
    std::printf("not one: %s\n", std::string{libtmux::to_string(none.error())}.c_str());
  }
  // #endregion cardinality

  // #region capture
  // Read a pane's visible contents, or its scrollback.
  const libtmux::Pane& pane = panes->at(0);

  const auto visible = pane.capture();
  if (visible.has_value()) {
    std::printf("%zu bytes on screen\n", visible->size());
  }

  const auto history = pane.capture({.whole_history = true});
  if (history.has_value()) {
    std::printf("%zu bytes of scrollback\n", history->size());
  }
  // #endregion capture

  // #region traverse
  // Every entity knows the server it came from, so it can reach its children
  // and its parents without a target string.
  const auto window = pane.window();
  const auto owner = pane.session();
  if (window.has_value() && owner.has_value()) {
    std::printf("%s is in %s, in %s\n", std::string{pane.id()}.c_str(),
                std::string{window->name()}.c_str(),
                std::string{owner->name()}.c_str());
  }
  // #endregion traverse

  // #region snapshot
  // An entity is one row of the listing that produced it: a moment, not a
  // live handle. Ask again for the present.
  (void)editor->rename("renamed");

  std::printf("held: %s\n", std::string{editor->name()}.c_str()); // still "editor"

  const auto now = editor->refresh();
  if (now.has_value()) {
    std::printf("now: %s\n", std::string{now->name()}.c_str()); // "renamed"
  }
  // #endregion snapshot

  // #region errors
  // Failures are values with a kind, so a caller can tell "you asked wrongly"
  // from "tmux said no" from "tmux never answered".
  const auto gone = server.run({"kill-session", "-t", "=no-such-session"});
  if (!gone.has_value()) {
    switch (gone.error().kind) {
    case libtmux::FailureKind::validation:
      std::printf("the request was malformed before it was sent\n");
      break;
    case libtmux::FailureKind::unsupported:
      std::printf("this backend cannot provide the operation safely\n");
      break;
    case libtmux::FailureKind::refused:
      std::printf("tmux refused it: %s\n", gone.error().diagnostic.c_str());
      break;
    case libtmux::FailureKind::timeout:
      std::printf("tmux did not answer in time\n");
      break;
    default:
      std::printf("%s\n", gone.error().diagnostic.c_str());
      break;
    }
  }
  // #endregion errors

  // #region async
  std::size_t observed = 0U;
  auto async_server = libtmux::Server::at_socket_path(
      scratch.socket_path().string(),
      [&observed](std::string_view, const libtmux::CommandFailure*) { ++observed; });
  if (!async_server.has_value()) {
    std::fprintf(stderr, "%s\n", async_server.error().diagnostic.c_str());
    return 1;
  }

  auto started_runtime =
      libtmux::CommandRuntime::start(libtmux::CommandRuntimeConfig{.capacity = 1U});
  if (!started_runtime.has_value()) {
    std::fprintf(stderr, "%s\n", started_runtime.error().diagnostic.c_str());
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
    std::fprintf(stderr, "%s\n", submitted.error().diagnostic.c_str());
    return 1;
  }
  auto result = std::move(*submitted).wait();
  if (!result.has_value()) { // Failed after admission.
    std::fprintf(stderr, "%s\n", result.error().diagnostic.c_str());
    return 1;
  }
  if (!wait_for_completion(1U)) {
    return 1;
  }

  const auto held = runtime.snapshot();
  std::printf("%zu/%zu slot(s), %zu observation(s) pending\n", held.in_flight,
              held.capacity, held.pending_observers);
  std::printf("dispatched %zu observation(s)\n", runtime.dispatch_ready());

  auto detached =
      async_server->try_submit(runtime, {"display-message", "-p", "detached"});
  if (!detached.has_value()) {
    std::fprintf(stderr, "%s\n", detached.error().diagnostic.c_str());
    return 1;
  }
  std::move(*detached).detach(); // Keep no result; the observation remains.
  if (!wait_for_completion(2U)) {
    return 1;
  }
  std::printf("discarded %zu observation(s)\n", runtime.discard_ready());

  const auto shutdown = runtime.close();
  if (shutdown.failure.has_value()) {
    std::fprintf(stderr, "%s\n", shutdown.failure->diagnostic.c_str());
    return 1;
  }
  std::printf("runtime stopped: %s; safe to unload: %s; observed: %zu\n",
              shutdown.transports_stopped ? "yes" : "no",
              shutdown.safe_to_unload ? "yes" : "no", observed);
  if (!shutdown.transports_stopped || !shutdown.safe_to_unload || observed != 1U) {
    return 1;
  }
  // #endregion async

  // #region escape
  // Anything tmux knows and this library does not name yet: ask it directly,
  // with a format string expanded against a pane.
  const auto running = pane.expand("#{pane_current_command}");
  if (running.has_value()) {
    std::printf("running %s\n", running->c_str());
  }

  // Or run a command and read its output.
  const auto answer = server.run({"display-message", "-p", "#{version}"});
  if (answer.has_value()) {
    std::printf("tmux %s", answer->c_str()); // tmux's answer ends in a newline
  }
  // #endregion escape

  // #region options
  // Options are read and written where tmux scopes them.
  (void)session.set_option("@project", "libtmux");

  const auto project = session.option("@project");
  if (project.has_value()) {
    std::printf("@project is %s\n", project->value.c_str());
  }
  // #endregion options

  // #region chain
  // A chain refuses a target it cannot address before reaching tmux at all,
  // so a malformed batch costs nothing.
  libtmux::Chain chain;
  chain.new_window("a:b", "unreachable");
  std::printf("chain valid: %s\n", chain.valid() ? "yes" : "no"); // no
  // #endregion chain

  // #region fixture
  // A private tmux for a suite of your own, gone when the scope ends.
  auto fixture = libtmux::test::ScopedTmuxServer::start(
      {.socket_namespace = libtmux::test::SocketNamespace::consumer("my-suite")});
  if (!fixture.has_value()) {
    std::fprintf(stderr, "%s\n", fixture.error().c_str());
    return 1;
  }
  const auto under_test =
      libtmux::Server::at_socket_path(fixture->socket_path().string());
  std::printf("sessions on it: %zu\n", under_test->sessions()->size());
  // #endregion fixture

  // The stand-in editor's directory, which the scratch server does not own.
  std::error_code cleanup;
  std::filesystem::remove_all(std::filesystem::temp_directory_path() /
                                  ("libtmux-cxx-editor-" + std::to_string(::getpid())),
                              cleanup);
  return 0;
}
