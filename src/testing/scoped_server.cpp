#include "libtmux/testing/scoped_server.hpp"
#include "libtmux/expected.hpp"

#include "process.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// `mkdtemp` is POSIX, but the two platforms disagree about where it lives:
// glibc declares it in <stdlib.h> and macOS in <unistd.h>. `<cstdlib>` alone
// promises only the `std::` names, and on macOS leaves `::mkdtemp` undeclared.
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace libtmux::test {
namespace {

using detail::ChildProcess;
using detail::ProcessClock;
using detail::ProcessOptions;

struct CommandResult {
  std::string stdout_text;
  std::string stderr_text;
};

std::mutex& teardown_report_mutex() {
  // A fixture may report during static destruction, so the lock has process
  // lifetime just like the report-writing path it protects.
  static auto* mutex = new std::mutex;
  return *mutex;
}

std::string trim_trailing_newlines(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

std::string encode_tmux_token(std::string_view value) {
  // if-shell parses each branch a second time. Octal escapes keep every byte
  // inside one argument without exposing tmux command separators or formats.
  std::string encoded;
  encoded.reserve(value.size() * 4U);
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    encoded.push_back('\\');
    encoded.push_back(static_cast<char>('0' + ((byte >> 6U) & 0x07U)));
    encoded.push_back(static_cast<char>('0' + ((byte >> 3U) & 0x07U)));
    encoded.push_back(static_cast<char>('0' + (byte & 0x07U)));
  }
  return encoded;
}

libtmux::expected<CommandResult, std::string>
run_command(const std::filesystem::path& executable, std::vector<std::string> arguments,
            const std::vector<std::string>& environment,
            ProcessClock::time_point deadline) {
  auto child = ChildProcess::spawn({.executable = executable,
                                    .arguments = std::move(arguments),
                                    .environment = environment});
  if (!child.has_value()) {
    return libtmux::unexpected(child.error());
  }
  if (!child->wait_until(deadline)) {
    static_cast<void>(child->send_signal(SIGKILL));
    child->terminate_and_reap(deadline);
    return libtmux::unexpected("tmux client exceeded its deadline");
  }
  const auto status = child->wait_status();
  if (!status.has_value() || !WIFEXITED(*status) || WEXITSTATUS(*status) != 0) {
    auto error = trim_trailing_newlines(child->stderr_text());
    if (error.empty()) {
      // How it ended, rather than only that it did. A tmux that fails with
      // nothing on stderr used to report as "exited unsuccessfully", which is
      // the one sentence that cannot be acted on: it hides whether the client
      // exited with a status, died on a signal, or was never reaped at all.
      error = "tmux client failed with no diagnostic: ";
      if (!status.has_value()) {
        error += "no wait status";
      } else if (WIFSIGNALED(*status)) {
        error += "killed by signal " + std::to_string(WTERMSIG(*status));
      } else if (WIFEXITED(*status)) {
        error += "exit status " + std::to_string(WEXITSTATUS(*status));
      } else {
        error += "wait status " + std::to_string(*status);
      }
    }
    return libtmux::unexpected(std::move(error));
  }
  return CommandResult{.stdout_text = child->stdout_text(),
                       .stderr_text = child->stderr_text()};
}

libtmux::expected<std::filesystem::path, std::string> create_private_tree() {
  const auto* temporary = std::getenv("TMPDIR");
  const auto parent = temporary != nullptr && temporary[0] != '\0'
                          ? std::filesystem::path{temporary}
                          : std::filesystem::path{"/tmp"};
  std::error_code error;
  const auto absolute_parent = std::filesystem::canonical(parent, error);
  if (error) {
    return libtmux::unexpected("temporary directory canonicalization failed: " +
                               error.message());
  }
  // Named for the workspace, not just for this library: several checkouts of
  // libtmux in different languages run tests on one machine, and a stray
  // server should say which one left it. Kept short because the whole socket
  // path has to fit in `sun_path`, which `socket_path_fits` then checks.
  auto pattern = (absolute_parent / "libtmux-cxx-test-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    return libtmux::unexpected(std::string{"mkdtemp: "} + std::strerror(errno));
  }
  return std::filesystem::path{pattern};
}

bool socket_path_fits(const std::filesystem::path& path) {
  sockaddr_un address{};
  return path.native().size() < sizeof(address.sun_path);
}

libtmux::expected<bool, std::string>
path_is_within(const std::filesystem::path& path, const std::filesystem::path& parent) {
  std::error_code error;
  const auto canonical_parent = std::filesystem::weakly_canonical(parent, error);
  if (error) {
    return libtmux::unexpected("fixture tree canonicalization failed: " +
                               error.message());
  }
  const auto canonical_path = std::filesystem::weakly_canonical(path, error);
  if (error) {
    return libtmux::unexpected("resolved socket canonicalization failed: " +
                               error.message());
  }

  auto parent_component = canonical_parent.begin();
  auto path_component = canonical_path.begin();
  while (parent_component != canonical_parent.end()) {
    if (path_component == canonical_path.end() ||
        *path_component != *parent_component) {
      return false;
    }
    ++parent_component;
    ++path_component;
  }
  return true;
}

void wait_for_next_probe(ProcessClock::time_point deadline) {
  std::this_thread::sleep_until(
      std::min(deadline, ProcessClock::now() + std::chrono::milliseconds{10}));
}

libtmux::expected<pid_t, std::string> parse_pid(std::string text) {
  text = trim_trailing_newlines(std::move(text));
  long raw_pid = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), raw_pid);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      raw_pid <= 0) {
    return libtmux::unexpected("tmux returned an invalid server pid");
  }
  return static_cast<pid_t>(raw_pid);
}

} // namespace

struct ScopedTmuxServer::State {
  explicit State(ScopedTmuxServerOptions requested_options)
      : options(std::move(requested_options)), mode(options.mode) {}

  ~State() noexcept { cleanup(); }
  State(const State&) = delete;
  State& operator=(const State&) = delete;

  void append_report(std::string_view message) noexcept {
    if (!options.teardown_report) {
      return;
    }
    try {
      std::lock_guard lock{teardown_report_mutex()};
      options.teardown_report->messages.emplace_back(message);
    } catch (...) {
    }
  }

  void append_report(std::string_view prefix, std::string_view detail) noexcept {
    if (!options.teardown_report) {
      return;
    }
    try {
      auto message = std::string{prefix};
      message.append(detail);
      std::lock_guard lock{teardown_report_mutex()};
      options.teardown_report->messages.push_back(std::move(message));
    } catch (...) {
    }
  }

  std::vector<std::string> initial_selector() const {
    if (mode == SocketMode::Name) {
      return {"-N", "-L", socket_name};
    }
    return {"-N", "-S", socket_path.string()};
  }

  void cleanup() noexcept {
    if (cleaned) {
      return;
    }
    cleaned = true;
    const auto started = ProcessClock::now();
    const auto deadline = started + options.teardown_timeout;
    const auto first_stage = started + options.teardown_timeout / 2;
    const auto second_stage = started + (options.teardown_timeout * 3) / 4;

    if (server) {
      if (server->is_running() && !socket_path.empty()) {
        try {
          const auto ownership_condition =
              "#{==:#{pid}," + std::to_string(server->pid()) + "}";
          auto result = run_command(options.tmux_binary,
                                    {"-N", "-S", socket_path.string(), "if-shell", "-F",
                                     ownership_condition, "kill-server", ""},
                                    environment, first_stage);
          if (!result.has_value()) {
            append_report("kill-server failed: ", result.error());
          }
        } catch (...) {
          append_report("kill-server reporting failed");
        }
      }

      if (!server->wait_until(first_stage)) {
        append_report("server remained alive after kill-server; sent SIGTERM");
        static_cast<void>(server->send_signal(SIGTERM));
      }
      if (!server->wait_until(second_stage)) {
        append_report("server remained alive after SIGTERM; sent SIGKILL");
        static_cast<void>(server->send_signal(SIGKILL));
      }
      if (!server->wait_until(deadline)) {
        append_report("server did not reap before teardown deadline");
      }
      server->terminate_and_reap(deadline);
      server.reset();
    }

    if (!private_tree.empty()) {
      try {
        std::error_code error;
        std::filesystem::remove_all(private_tree, error);
        if (error) {
          append_report("fixture tree removal failed: ", error.message());
        }
      } catch (...) {
        append_report("fixture tree removal failed");
      }
    }
    append_report("server teardown complete");
  }

  ScopedTmuxServerOptions options;
  SocketMode mode;
  std::string socket_name;
  std::filesystem::path socket_path;
  std::filesystem::path private_tree;
  std::vector<std::string> environment;
  std::unique_ptr<ChildProcess> server;
  bool cleaned{false};
};

libtmux::expected<ScopedTmuxServer, std::string>
ScopedTmuxServer::start(ScopedTmuxServerOptions options) {
  if (options.startup_timeout <= std::chrono::milliseconds{0}) {
    return libtmux::unexpected("startup timeout must be positive");
  }
  if (options.teardown_timeout <= std::chrono::milliseconds{0}) {
    return libtmux::unexpected("teardown timeout must be positive");
  }
  if (options.session_name.empty()) {
    return libtmux::unexpected("session name must not be empty");
  }

  auto state = std::make_unique<State>(std::move(options));
  auto private_tree = create_private_tree();
  if (!private_tree.has_value()) {
    return libtmux::unexpected(private_tree.error());
  }
  state->private_tree = std::move(*private_tree);
  state->environment = detail::current_environment();
  detail::erase_environment(state->environment, "TMUX");
  detail::erase_environment(state->environment, "TMUX_PANE");

  // A pane runs $SHELL with the developer's configuration, so what a test
  // observes depends on whose machine it runs on. An interactive zsh with no
  // startup files, for one, opens a first-run wizard and swallows everything
  // typed at it — which reads as a library that sent nothing.
  //
  // A private HOME inside the tree this fixture already removes, and the
  // shell POSIX guarantees, make the pane the same everywhere.
  const std::filesystem::path home = state->private_tree / "home";
  std::error_code home_error;
  std::filesystem::create_directory(home, home_error);
  if (home_error) {
    return libtmux::unexpected("cannot create the fixture's private HOME: " +
                               home_error.message());
  }
  detail::set_environment(state->environment, "HOME", home.string());
  detail::set_environment(state->environment, "SHELL", "/bin/sh");
  for (const char* name : {"ZDOTDIR", "ENV", "BASH_ENV", "XDG_CONFIG_HOME"}) {
    detail::erase_environment(state->environment, name);
  }

  std::vector<std::string> server_arguments{"-D", "-u", "-f", "/dev/null"};
  if (state->mode == SocketMode::Name) {
    state->socket_name = "server";
    detail::set_environment(state->environment, "TMUX_TMPDIR",
                            state->private_tree.string());
    server_arguments.insert(server_arguments.end(), {"-L", state->socket_name});
  } else {
    state->socket_path = state->private_tree / "socket";
    if (!socket_path_fits(state->socket_path)) {
      return libtmux::unexpected("generated tmux socket path exceeds sun_path");
    }
    server_arguments.insert(server_arguments.end(),
                            {"-S", state->socket_path.string()});
  }

  const auto deadline = ProcessClock::now() + state->options.startup_timeout;
  auto server = ChildProcess::spawn({.executable = state->options.tmux_binary,
                                     .arguments = std::move(server_arguments),
                                     .environment = state->environment});
  if (!server.has_value()) {
    return libtmux::unexpected(server.error());
  }
  state->server = std::make_unique<ChildProcess>(std::move(*server));
  auto& owned_server = *state->server;

  std::string last_error = "tmux did not become ready";
  while (ProcessClock::now() < deadline) {
    owned_server.drain_until(
        std::min(deadline, ProcessClock::now() + std::chrono::milliseconds{1}));
    if (!owned_server.is_running()) {
      auto error = trim_trailing_newlines(owned_server.stderr_text());
      if (!error.empty()) {
        last_error = "tmux server exited during startup: " + error;
      } else {
        last_error = "tmux server exited during startup";
      }
      break;
    }

    auto pid_arguments = state->initial_selector();
    pid_arguments.insert(pid_arguments.end(), {"display-message", "-p", "#{pid}"});
    auto pid_result = run_command(state->options.tmux_binary, std::move(pid_arguments),
                                  state->environment, deadline);
    if (!pid_result.has_value()) {
      last_error = pid_result.error();
      wait_for_next_probe(deadline);
      continue;
    }
    auto queried_pid = parse_pid(pid_result->stdout_text);
    if (!queried_pid.has_value()) {
      last_error = queried_pid.error();
      wait_for_next_probe(deadline);
      continue;
    }
    if (*queried_pid != owned_server.pid()) {
      return libtmux::unexpected("queried tmux pid does not match the owned child");
    }

    auto path_arguments = state->initial_selector();
    path_arguments.insert(path_arguments.end(),
                          {"display-message", "-p", "#{socket_path}"});
    auto path_result =
        run_command(state->options.tmux_binary, std::move(path_arguments),
                    state->environment, deadline);
    if (!path_result.has_value()) {
      last_error = path_result.error();
      wait_for_next_probe(deadline);
      continue;
    }
    const auto resolved_path =
        std::filesystem::path{trim_trailing_newlines(path_result->stdout_text)};
    if (resolved_path.empty()) {
      last_error = "tmux returned an empty socket path";
      wait_for_next_probe(deadline);
      continue;
    }
    if (state->mode == SocketMode::Path && resolved_path != state->socket_path) {
      return libtmux::unexpected(
          "resolved tmux socket path does not match the exact socket path");
    }
    if (state->mode == SocketMode::Name) {
      auto contained = path_is_within(resolved_path, state->private_tree);
      if (!contained.has_value()) {
        return libtmux::unexpected(contained.error());
      }
      if (!*contained) {
        return libtmux::unexpected(
            "resolved tmux socket path is outside the fixture tree");
      }
    }
    if (!socket_path_fits(resolved_path)) {
      return libtmux::unexpected("resolved tmux socket path exceeds sun_path");
    }
    state->socket_path = resolved_path;

    auto resolved_pid_result = run_command(
        state->options.tmux_binary,
        {"-N", "-S", state->socket_path.string(), "display-message", "-p", "#{pid}"},
        state->environment, deadline);
    if (!resolved_pid_result.has_value()) {
      last_error = resolved_pid_result.error();
      wait_for_next_probe(deadline);
      continue;
    }
    auto resolved_pid = parse_pid(resolved_pid_result->stdout_text);
    if (!resolved_pid.has_value()) {
      last_error = resolved_pid.error();
      wait_for_next_probe(deadline);
      continue;
    }
    if (*resolved_pid != owned_server.pid()) {
      return libtmux::unexpected(
          "resolved tmux socket pid does not match the owned child");
    }

    constexpr std::string_view session_created = "libtmux-session-created";
    constexpr std::string_view session_rejected = "libtmux-session-rejected";
    const auto ownership_condition =
        "#{==:#{pid}," + std::to_string(owned_server.pid()) + "}";
    const auto create_session_command =
        "new-session -d -P -F libtmux-session-created -s " +
        encode_tmux_token(state->options.session_name);
    const auto reject_session_command = "display-message -p libtmux-session-rejected";
    auto session_result = run_command(state->options.tmux_binary,
                                      {"-N", "-S", state->socket_path.string(),
                                       "if-shell", "-F", ownership_condition,
                                       create_session_command, reject_session_command},
                                      state->environment, deadline);
    if (!session_result.has_value()) {
      return libtmux::unexpected("tmux session creation failed: " +
                                 session_result.error());
    }
    const auto acknowledgement =
        trim_trailing_newlines(std::move(session_result->stdout_text));
    if (acknowledgement == session_rejected) {
      return libtmux::unexpected("session creation ownership changed");
    }
    if (acknowledgement != session_created) {
      return libtmux::unexpected(
          "tmux session creation returned an invalid acknowledgement");
    }
    return ScopedTmuxServer{std::move(state)};
  }
  return libtmux::unexpected("tmux startup failed: " + last_error);
}

ScopedTmuxServer::ScopedTmuxServer(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

ScopedTmuxServer::~ScopedTmuxServer() noexcept = default;
ScopedTmuxServer::ScopedTmuxServer(ScopedTmuxServer&&) noexcept = default;
ScopedTmuxServer& ScopedTmuxServer::operator=(ScopedTmuxServer&&) noexcept = default;

SocketMode ScopedTmuxServer::socket_mode() const noexcept {
  return state_ ? state_->mode : SocketMode::Path;
}

std::optional<std::string_view> ScopedTmuxServer::socket_name() const noexcept {
  if (!state_ || state_->mode != SocketMode::Name) {
    return std::nullopt;
  }
  return state_->socket_name;
}

const std::filesystem::path& ScopedTmuxServer::socket_path() const noexcept {
  static const std::filesystem::path empty;
  return state_ ? state_->socket_path : empty;
}

const std::filesystem::path& ScopedTmuxServer::tmux_tmpdir() const noexcept {
  static const std::filesystem::path empty;
  return state_ ? state_->private_tree : empty;
}

std::string_view ScopedTmuxServer::session_name() const noexcept {
  return state_ ? std::string_view{state_->options.session_name} : std::string_view{};
}

int ScopedTmuxServer::server_pid() const noexcept {
  return state_ && state_->server ? state_->server->pid() : -1;
}

std::vector<std::string> ScopedTmuxServer::command_prefix() const {
  if (!state_) {
    return {};
  }
  return {state_->options.tmux_binary.string(), "-S", state_->socket_path.string()};
}

bool ScopedTmuxServer::is_alive() const {
  return state_ && state_->server && state_->server->is_running();
}

} // namespace libtmux::test
