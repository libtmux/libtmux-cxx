#include "libtmux/server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "acquire.hpp"
#include "backend.hpp"
#include "control_backend.hpp"
#include "environment.hpp"
#include "path.hpp"
#include "psmux.hpp"

LIBTMUX_NAMESPACE_BEGIN

namespace {

// A command run for its effect: its output is not a result.
#if !defined(_WIN32)
expected<void, CommandFailure> applied(expected<std::string, CommandFailure> reply) {
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return {};
}
#endif

#if defined(_WIN32)
[[nodiscard]] bool unavailable_candidate(const CommandFailure& failure) noexcept {
  return failure.kind == FailureKind::missing ||
         (failure.kind == FailureKind::refused &&
          libtmux_psmux::missing_session(failure.diagnostic));
}

[[nodiscard]] CommandFailure no_sessions() {
  return CommandFailure{.kind = FailureKind::refused,
                        .dispatched = true,
                        .exit_code = 0,
                        .diagnostic = "the server has no sessions"};
}

[[nodiscard]] CommandFailure unsupported_psmux_state(std::string_view operation) {
  return CommandFailure{.kind = FailureKind::unsupported,
                        .dispatched = false,
                        .exit_code = 0,
                        .diagnostic = "psmux cannot provide " + std::string{operation} +
                                      " through the typed API"};
}

[[nodiscard]] ExecutionPolicy
remaining_policy(const ExecutionPolicy& policy,
                 std::chrono::steady_clock::time_point started) {
  ExecutionPolicy result = policy;
  if (!policy.timeout.has_value()) {
    return result;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  result.timeout = elapsed < *policy.timeout ? *policy.timeout - elapsed
                                             : std::chrono::milliseconds{0};
  return result;
}

[[nodiscard]] expected<std::vector<Session>, CommandFailure>
exact_psmux_sessions(const std::shared_ptr<const detail::Backend>& backend,
                     const ExecutionPolicy& policy,
                     std::chrono::steady_clock::time_point started) {
  auto candidates = detail::list_entities<Session>(backend, {"list-sessions"}, {},
                                                   remaining_policy(policy, started));
  if (!candidates.has_value()) {
    return unexpected(candidates.error());
  }

  std::vector<Session> exact;
  exact.reserve(candidates->size());
  for (const Session& candidate : *candidates) {
    auto call_policy = remaining_policy(policy, started);
    auto belongs =
        backend->session_belongs(candidate.id(), candidate.name(), call_policy.timeout);
    if (!belongs.has_value()) {
      if (unavailable_candidate(belongs.error())) {
        continue;
      }
      return unexpected(belongs.error());
    }
    if (!*belongs) {
      continue;
    }

    auto refreshed = detail::describe<Session>(
        backend, ":", candidate.id(), {.id = candidate.id(), .name = candidate.name()},
        remaining_policy(policy, started));
    if (!refreshed.has_value()) {
      if (unavailable_candidate(refreshed.error())) {
        continue;
      }
      return unexpected(refreshed.error());
    }
    if (std::ranges::any_of(exact, [&](const Session& retained) {
          return retained.id() == refreshed->id();
        })) {
      return unexpected(
          CommandFailure{.kind = FailureKind::refused,
                         .dispatched = true,
                         .exit_code = 0,
                         .diagnostic = "psmux reported one session ID more than once"});
    }
    exact.push_back(*std::move(refreshed));
  }
  return exact;
}

#endif

} // namespace

namespace detail {

Server server_over(std::shared_ptr<const Backend> backend) {
  return Server{std::move(backend)};
}

} // namespace detail

namespace {

CommandFailure rejected_selector(std::string_view selector, SocketError error) {
  return CommandFailure{
      .kind = error == SocketError::path_unsupported ? FailureKind::unsupported
                                                     : FailureKind::validation,
      .dispatched = false,
      .exit_code = 0,
      .diagnostic = std::string{to_string(error)} + ": " + std::string{selector}};
}

} // namespace

expected<Server, CommandFailure> Server::at_socket_path(std::string_view path,
                                                        CommandObserver observer,
                                                        ExecutionPolicy policy) {
  auto arguments = socket_path_arguments(path);
  if (!arguments.has_value()) {
    return unexpected(rejected_selector(path, arguments.error()));
  }
  return detail::server_over(std::make_shared<const detail::SubprocessBackend>(
      *std::move(arguments), std::move(observer), policy));
}

expected<Server, CommandFailure> Server::from_env(CommandObserver observer,
                                                  ExecutionPolicy policy) {
  const auto inherited = libtmux_env::value("TMUX");
  if (!inherited.has_value()) {
    return unexpected(CommandFailure{
        .kind = FailureKind::validation,
        .dispatched = false,
        .exit_code = 0,
        .diagnostic = "TMUX is not set: this process is not running inside tmux"});
  }
  // `<socket path>,<server pid>,<session id>`. Only the first field is
  // trustworthy, and a socket path may itself contain a comma, so the split is
  // at the last one that could begin the pid.
  const std::string_view value{*inherited};
  const auto pid_start = value.find_last_of(',', value.find_last_of(',') - 1U);
  const std::string_view socket =
      pid_start == std::string_view::npos ? value : value.substr(0, pid_start);
  if (socket.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "TMUX names no socket path"});
  }
#if defined(_WIN32)
  if (libtmux_env::value("PSMUX_SESSION").has_value()) {
    const auto separator = socket.find_last_of("/\\");
    const auto name =
        separator == std::string_view::npos ? socket : socket.substr(separator + 1U);
    if (name == "default") {
      return at_default(std::move(observer), policy);
    }
    return at_socket_name(name, std::move(observer), policy);
  }
#endif
  return at_socket_path(socket, std::move(observer), policy);
}

expected<Server, CommandFailure> Server::at_default(CommandObserver observer,
                                                    ExecutionPolicy policy) {
  // No selector at all, which is what tmux itself does: the default socket
  // under the directory it chooses, honouring TMUX_TMPDIR as tmux does.
  return detail::server_over(std::make_shared<const detail::SubprocessBackend>(
      std::vector<std::string>{}, std::move(observer), policy));
}

expected<Server, CommandFailure> Server::at_socket_name(std::string_view name,
                                                        CommandObserver observer,
                                                        ExecutionPolicy policy) {
  if (auto invalid = libtmux_psmux::invalid_socket_name(name); invalid.has_value()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = std::move(*invalid)});
  }
  auto arguments = socket_name_arguments(name);
  if (!arguments.has_value()) {
    return unexpected(rejected_selector(name, arguments.error()));
  }
  return detail::server_over(std::make_shared<const detail::SubprocessBackend>(
      *std::move(arguments), std::move(observer), policy));
}

ServerCapabilities Server::capabilities() const noexcept {
  return backend_->capabilities();
}

expected<std::string, CommandFailure>
Server::run(const std::vector<std::string>& command,
            std::optional<std::chrono::milliseconds> timeout,
            std::optional<std::size_t> output_limit) const {
  // The policy fills in what the call did not say. Applied here rather than in
  // the backend, because the backend still has to be able to be told "no
  // deadline" — `wait_for` means it, and it is the only caller that does.
  const ExecutionPolicy& policy = backend_->policy();
  return backend_->run(command, timeout.has_value() ? timeout : policy.timeout,
                       output_limit.has_value() ? output_limit : policy.output_limit);
}

expected<std::string, CommandFailure>
Server::run_batch(const CommandBatch& batch) const {
  if (batch.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "empty batch"});
  }
  const ExecutionPolicy& policy = backend_->policy();
  return backend_->run_batch(batch, policy.timeout, policy.output_limit);
}

expected<std::string, CommandFailure> Server::run_chain(const Chain& chain) const {
  if (!chain.valid()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = chain.error()});
  }
  return run_batch(chain.batch());
}

expected<Connection, ProtocolError> Server::control(std::string_view session) const {
  return control_with_options(session, {});
}

expected<Connection, ProtocolError>
Server::control_with_options(std::string_view session,
                             ConnectionOptions options) const {
  const ServerCapabilities available = capabilities();
  if (!available.supports(ServerFeature::control_mode)) {
    return unexpected(
        ProtocolError{std::string{to_string(available.implementation)} +
                      " backend does not support persistent control mode"});
  }
  // POSIX control uses the resolved socket path. The Windows implementation
  // rejects psmux control mode before consuming its logical identity.
  const std::string_view resolved = backend_->identity();
  if (resolved.empty()) {
    return unexpected(ProtocolError{"this server has no socket to connect to"});
  }
  return Connection::connect(detail::routed_control_options(
      std::move(options), std::string{resolved}, std::string{session}));
}

expected<Version, CommandFailure> Server::tmux_version() const {
  return backend_->version();
}

bool Server::is_alive(std::chrono::milliseconds timeout) const {
  return check_alive(timeout).has_value();
}

expected<void, CommandFailure>
Server::check_alive(std::chrono::milliseconds timeout) const {
#if defined(_WIN32)
  ExecutionPolicy policy = backend_->policy();
  policy.timeout = timeout;
  const auto started = std::chrono::steady_clock::now();
  for (int scan = 0; scan < 2; ++scan) {
    auto exact = exact_psmux_sessions(backend_, policy, started);
    if (!exact.has_value()) {
      return unexpected(exact.error());
    }
    if (!exact->empty()) {
      return {};
    }
  }
  return unexpected(no_sessions());
#else
  auto sessions = run({"list-sessions", "-F", "#{session_id}"}, timeout);
  if (!sessions.has_value()) {
    return unexpected(sessions.error());
  }
  // A successful empty listing still describes no live server.
  if (sessions->find_first_not_of(" \t\r\n") == std::string::npos) {
    return unexpected(CommandFailure{.kind = FailureKind::refused,
                                     .dispatched = true,
                                     .exit_code = 0,
                                     .diagnostic = "the server has no sessions"});
  }
  return {};
#endif
}

expected<void, CommandFailure> Server::kill() const {
#if defined(_WIN32)
  const ExecutionPolicy& policy = backend_->policy();
  const auto started = std::chrono::steady_clock::now();
  const auto budget = [&]() -> std::optional<std::chrono::milliseconds> {
    if (!policy.timeout.has_value()) {
      return std::nullopt;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return elapsed < *policy.timeout
               ? std::optional<std::chrono::milliseconds>{*policy.timeout - elapsed}
               : std::optional<std::chrono::milliseconds>{std::chrono::milliseconds{0}};
  };
  constexpr std::array fields{std::string_view{"session_id"},
                              std::string_view{"session_name"}};
  bool found_any = false;
  std::optional<std::string> empty_signature;
  for (;;) {
    auto output = backend_->run({"list-sessions", "-F", format_request(fields)},
                                budget(), policy.output_limit);
    if (!output.has_value()) {
      return unexpected(output.error());
    }
    const auto candidates = Snapshot::from_recording(fields, *std::move(output));
    if (candidates == nullptr) {
      return unexpected(CommandFailure{
          .kind = FailureKind::refused,
          .dispatched = true,
          .exit_code = 0,
          .diagnostic = "tmux output did not match the fields asked for"});
    }

    std::vector<std::pair<std::string, std::string>> rows;
    rows.reserve(candidates->rows().size());
    for (const auto& row : candidates->rows()) {
      rows.emplace_back(row[0], row[1]);
    }
    std::ranges::sort(rows);
    std::string signature;
    for (const auto& [id, name] : rows) {
      signature += id;
      signature.push_back('\0');
      signature += name;
      signature.push_back('\n');
    }

    std::vector<std::pair<std::string, std::string>> exact;
    for (const auto& row : candidates->rows()) {
      auto belongs = backend_->session_belongs(row[0], row[1], budget());
      if (!belongs.has_value()) {
        if (unavailable_candidate(belongs.error())) {
          continue;
        }
        return unexpected(belongs.error());
      }
      if (*belongs && std::ranges::find(exact, std::pair<std::string, std::string>{
                                                   row[0], row[1]}) == exact.end()) {
        exact.emplace_back(row[0], row[1]);
      }
    }
    if (exact.empty()) {
      // A second identical scan separates a stable nested-prefix listing from
      // an exact session renamed between listing and identity validation.
      if (empty_signature.has_value() && *empty_signature == signature) {
        if (!found_any) {
          return unexpected(no_sessions());
        }
        return {};
      }
      empty_signature = std::move(signature);
      continue;
    }
    empty_signature.reset();
    found_any = true;
    for (const auto& [id, name] : exact) {
      auto killed = backend_->run_in_session({"kill-session"}, id, name, budget(),
                                             policy.output_limit);
      if (!killed.has_value() && !unavailable_candidate(killed.error())) {
        return unexpected(killed.error());
      }
    }
  }
#else
  return applied(run({"kill-server"}));
#endif
}

std::vector<Notification> Server::take_notifications() const {
  return backend_->take_notifications();
}

std::size_t Server::dropped_notifications() const noexcept {
  return backend_->dropped_notifications();
}

expected<Server, CommandFailure> Server::over_control(std::string_view session) const {
  return over_control_with_options(session, {});
}

expected<Server, CommandFailure>
Server::over_control_with_options(std::string_view session,
                                  ConnectionOptions options) const {
  const ServerCapabilities available = capabilities();
  if (!available.supports(ServerFeature::control_mode)) {
    return unexpected(
        CommandFailure{.kind = FailureKind::unsupported,
                       .dispatched = false,
                       .exit_code = 0,
                       .diagnostic = std::string{to_string(available.implementation)} +
                                     " backend does not support control mode"});
  }
  const std::vector<std::string>& selector = backend_->connection();
  // POSIX selectors resolve to paths; Windows selectors resolve only far
  // enough for the control implementation to reject psmux explicitly.
  const std::string_view resolved = backend_->identity();
  if (resolved.empty()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::validation,
                       .dispatched = false,
                       .exit_code = 0,
                       .diagnostic = "this server has no socket to connect to"});
  }
  auto backend = detail::ControlBackend::open(
      selector, std::string{resolved}, std::string{resolved}, std::string{session},
      std::move(options), backend_->observer(), backend_->policy());
  if (!backend.has_value()) {
    // The same kind a control connection reports when it breaks mid-command,
    // because it is the same thing failing. Not dispatched: no command ran,
    // and a connection that never came up left nothing for a retry to repeat.
    return unexpected(CommandFailure{.kind = FailureKind::pipe,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = backend.error().message});
  }
  return detail::server_over(*std::move(backend));
}

expected<std::vector<Session>, CommandFailure> Server::sessions() const {
#if defined(_WIN32)
  return exact_psmux_sessions(backend_, backend_->policy(),
                              std::chrono::steady_clock::now());
#else
  return detail::list_entities<Session>(backend_, {"list-sessions"});
#endif
}

expected<std::vector<Window>, CommandFailure> Server::windows() const {
#if defined(_WIN32)
  const ExecutionPolicy& policy = backend_->policy();
  const auto started = std::chrono::steady_clock::now();
  auto owned = exact_psmux_sessions(backend_, policy, started);
  if (!owned.has_value()) {
    return unexpected(owned.error());
  }
  std::vector<Window> windows;
  for (const Session& session : *owned) {
    auto listed =
        detail::list_entities<Window>(backend_, {"list-windows", "-t", ":"},
                                      {.id = session.id(), .name = session.name()},
                                      remaining_policy(policy, started));
    if (!listed.has_value()) {
      return unexpected(listed.error());
    }
    for (Window& window : *listed) {
      windows.push_back(std::move(window));
    }
  }
  return windows;
#else
  return detail::list_entities<Window>(backend_, {"list-windows", "-a"});
#endif
}

expected<std::vector<Pane>, CommandFailure> Server::panes() const {
#if defined(_WIN32)
  const ExecutionPolicy& policy = backend_->policy();
  const auto started = std::chrono::steady_clock::now();
  auto owned = exact_psmux_sessions(backend_, policy, started);
  if (!owned.has_value()) {
    return unexpected(owned.error());
  }
  std::vector<Pane> panes;
  for (const Session& session : *owned) {
    auto listed =
        detail::list_entities<Pane>(backend_, {"list-panes", "-s", "-t", ":"},
                                    {.id = session.id(), .name = session.name()},
                                    remaining_policy(policy, started));
    if (!listed.has_value()) {
      return unexpected(listed.error());
    }
    for (Pane& pane : *listed) {
      panes.push_back(std::move(pane));
    }
  }
  return panes;
#else
  return detail::list_entities<Pane>(backend_, {"list-panes", "-a"});
#endif
}

expected<std::vector<Client>, CommandFailure> Server::clients() const {
#if defined(_WIN32)
  return unexpected(unsupported_psmux_state("clients"));
#else
  return detail::list_entities<Client>(backend_, {"list-clients"});
#endif
}

expected<void, CommandFailure>
Server::wait_for(std::string_view channel,
                 std::optional<std::chrono::milliseconds> timeout) const {
  if (channel.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a channel needs a name"});
  }
#if defined(_WIN32)
  static_cast<void>(timeout);
  return unexpected(unsupported_psmux_state("wait channels"));
#else
  // Straight to the backend, so an absent timeout still means wait. Waiting is
  // the request here; the policy's floor exists for calls that should have
  // answered by now, and this one has not been asked yet.
  const auto waited =
      backend_->run({"wait-for", std::string{channel}}, timeout, std::nullopt);
  if (!waited.has_value()) {
    return unexpected(waited.error());
  }
  // tmux exits zero when the server goes away under a waiter, so success
  // alone does not mean anyone signalled. Ask whether it is still there.
  if (!is_alive()) {
    return unexpected(CommandFailure{
        .kind = FailureKind::pipe,
        .dispatched = true,
        .exit_code = 0,
        .diagnostic = "the server ended while waiting on " + std::string{channel}});
  }
  return {};
#endif
}

expected<void, CommandFailure> Server::signal(std::string_view channel) const {
  if (channel.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a channel needs a name"});
  }
#if defined(_WIN32)
  return unexpected(unsupported_psmux_state("wait channels"));
#else
  return applied(run({"wait-for", "-S", std::string{channel}}));
#endif
}

expected<std::vector<Command>, CommandFailure> Server::commands() const {
#if defined(_WIN32)
  return unexpected(unsupported_psmux_state("command metadata"));
#else
  return detail::list_entities<Command>(backend_, {"list-commands"});
#endif
}

expected<std::vector<Buffer>, CommandFailure> Server::buffers() const {
#if defined(_WIN32)
  return unexpected(unsupported_psmux_state("buffers"));
#else
  return detail::list_entities<Buffer>(backend_, {"list-buffers"});
#endif
}

expected<void, CommandFailure>
Server::load_buffer(std::string_view name, const std::filesystem::path& from) const {
  if (name.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a buffer needs a name"});
  }
#if defined(_WIN32)
  static_cast<void>(from);
  return unexpected(unsupported_psmux_state("buffers"));
#else
  return applied(run({"load-buffer", "-b", std::string{name}, "--",
                      libtmux_path::command_string(from)}));
#endif
}

expected<void, CommandFailure>
Server::save_buffer(std::string_view name, const std::filesystem::path& to) const {
  if (name.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a buffer needs a name"});
  }
#if defined(_WIN32)
  static_cast<void>(to);
  return unexpected(unsupported_psmux_state("buffers"));
#else
  return applied(run({"save-buffer", "-b", std::string{name}, "--",
                      libtmux_path::command_string(to)}));
#endif
}

namespace {

// A key table has to survive being printed by `list-keys`, which prints the
// name unquoted with whitespace-separated columns around it.
//
// Only the table. An empty or unknown key is tmux's to refuse, and it does —
// `unknown key:` at a non-zero status, with nothing created — so a check
// here would only repeat it a round trip earlier.
expected<void, CommandFailure> usable_table(std::string_view table) {
  const auto refuse = [](std::string diagnostic) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = std::move(diagnostic)});
  };
  if (table.empty()) {
    return refuse("a key table name cannot be empty");
  }
  if (table.find_first_of(" \t\n\r\f\v") != std::string_view::npos) {
    return refuse("a key table name cannot contain whitespace: tmux lists it "
                  "unquoted, and the listing could not be read back");
  }
  return {};
}

} // namespace

expected<void, CommandFailure> Server::bind_key(std::string_view table,
                                                std::string_view key,
                                                const std::vector<std::string>& command,
                                                bool repeatable) const {
  if (const auto usable = usable_table(table); !usable.has_value()) {
    return unexpected(usable.error());
  }
  if (command.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a binding needs a command to run"});
  }
#if defined(_WIN32)
  static_cast<void>(key);
  static_cast<void>(repeatable);
  return unexpected(unsupported_psmux_state("key bindings"));
#else
  std::vector<std::string> argv{"bind-key"};
  if (repeatable) {
    argv.emplace_back("-r");
  }
  argv.emplace_back("-T");
  argv.emplace_back(table);
  argv.emplace_back("--");
  argv.emplace_back(key);
  argv.insert(argv.end(), command.begin(), command.end());
  return applied(run(argv));
#endif
}

expected<void, CommandFailure> Server::unbind_key(std::string_view table,
                                                  std::string_view key) const {
  if (const auto usable = usable_table(table); !usable.has_value()) {
    return unexpected(usable.error());
  }
#if defined(_WIN32)
  static_cast<void>(key);
  return unexpected(unsupported_psmux_state("key bindings"));
#else
  std::vector<std::string> argv{"unbind-key", "-T", std::string{table}};
  argv.emplace_back("--");
  argv.emplace_back(key);
  return applied(run(argv));
#endif
}

expected<void, CommandFailure> Server::run_shell(std::string_view command,
                                                 bool background) const {
  if (command.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a shell command cannot be empty"});
  }
#if defined(_WIN32)
  static_cast<void>(background);
  return unexpected(unsupported_psmux_state("run-shell state"));
#else
  std::vector<std::string> argv{"run-shell"};
  if (background) {
    argv.emplace_back("-b");
  }
  argv.emplace_back("--");
  argv.emplace_back(command);
  return applied(run(argv));
#endif
}

expected<void, CommandFailure>
Server::source_file(const std::filesystem::path& file) const {
#if defined(_WIN32)
  static_cast<void>(file);
  return unexpected(unsupported_psmux_state("configuration state"));
#else
  if (backend_->capabilities().backend == BackendKind::control_mode) {
    return unexpected(CommandFailure{
        .kind = FailureKind::unsupported,
        .dispatched = false,
        .exit_code = 0,
        .diagnostic = "control mode cannot attribute the commands source-file may "
                      "insert into its reply stream"});
  }
  return applied(run({"source-file", "--", libtmux_path::command_string(file)}));
#endif
}

expected<void, CommandFailure>
Server::check_file(const std::filesystem::path& file) const {
#if defined(_WIN32)
  static_cast<void>(file);
  return unexpected(CommandFailure{
      .kind = FailureKind::unsupported,
      .dispatched = false,
      .exit_code = 0,
      .diagnostic = "psmux cannot check a source file without executing its commands"});
#else
  return applied(run({"source-file", "-n", "--", libtmux_path::command_string(file)}));
#endif
}

expected<std::string, CommandFailure> Server::expand(std::string_view format) const {
#if defined(_WIN32)
  static_cast<void>(format);
  return unexpected(unsupported_psmux_state("format context"));
#else
  std::vector<std::string> command{"display-message", "-p"};
  detail::append_display_message_text(command, std::string{format});
  auto reply = run(command);
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return detail::without_trailing_newline(std::move(*reply));
#endif
}

expected<void, CommandFailure> Server::show_message(std::string_view text) const {
#if defined(_WIN32)
  static_cast<void>(text);
  return unexpected(unsupported_psmux_state("message state"));
#else
  std::vector<std::string> command{"display-message"};
  detail::append_display_message_text(command, std::string{text});
  return applied(run(command));
#endif
}

expected<void, CommandFailure> Server::set_buffer(std::string_view name,
                                                  std::string_view data) const {
#if defined(_WIN32)
  static_cast<void>(name);
  static_cast<void>(data);
  return unexpected(unsupported_psmux_state("buffers"));
#else
  std::vector<std::string> command{"set-buffer"};
  if (!name.empty()) {
    command.emplace_back("-b");
    command.emplace_back(name);
  }
  // `--` first: buffer text beginning with a dash is text, not a flag.
  command.emplace_back("--");
  command.emplace_back(data);
  return applied(run(command));
#endif
}

expected<Session, CommandFailure> Server::session(std::string_view target) const {
#if defined(_WIN32)
  auto owned = sessions();
  if (!owned.has_value()) {
    return unexpected(owned.error());
  }
  const std::string_view exact = target.starts_with('=') ? target.substr(1U) : target;
  const bool by_id =
      exact.size() > 1U && exact.starts_with('$') &&
      exact.find_first_not_of("0123456789", 1U) == std::string_view::npos;
  std::optional<Session> found;
  for (const Session& candidate : *owned) {
    if ((by_id && candidate.id() == exact) || (!by_id && candidate.name() == exact)) {
      if (found.has_value()) {
        return unexpected(CommandFailure{
            .kind = FailureKind::validation,
            .dispatched = false,
            .exit_code = 0,
            .diagnostic = "psmux session target is ambiguous: " + std::string{target}});
      }
      found = candidate;
    }
  }
  if (!found.has_value()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::missing,
                       .dispatched = true,
                       .exit_code = 0,
                       .diagnostic = "tmux has no session " + std::string{target}});
  }
  return *std::move(found);
#else
  return detail::describe<Session>(backend_, target);
#endif
}

expected<Window, CommandFailure> Server::window(std::string_view target) const {
#if defined(_WIN32)
  return unexpected(
      CommandFailure{.kind = FailureKind::unsupported,
                     .dispatched = false,
                     .exit_code = 0,
                     .diagnostic = "psmux window targets need an owning Session: " +
                                   std::string{target}});
#else
  return detail::describe<Window>(backend_, target);
#endif
}

expected<Pane, CommandFailure> Server::pane(std::string_view target) const {
#if defined(_WIN32)
  return unexpected(
      CommandFailure{.kind = FailureKind::unsupported,
                     .dispatched = false,
                     .exit_code = 0,
                     .diagnostic = "psmux pane targets need an owning Session: " +
                                   std::string{target}});
#else
  return detail::describe<Pane>(backend_, target);
#endif
}

expected<Session, CommandFailure> Server::new_session(std::string_view name) const {
  return new_session(NewSessionOptions{.name = std::string{name}});
}

expected<Session, CommandFailure> Server::new_session(NewSessionOptions options) const {
  if (options.name.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "session name is empty"});
  }
  if (auto invalid = libtmux_psmux::invalid_session_name(options.name);
      invalid.has_value()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = std::move(*invalid)});
  }
#if defined(_WIN32)
  static_cast<void>(options);
  return unexpected(CommandFailure{
      .kind = FailureKind::unsupported,
      .dispatched = false,
      .exit_code = 0,
      .diagnostic = "psmux cannot prove ownership of a concurrently created session; "
                    "create it with the psmux CLI and reacquire it from this Server"});
#else
  std::vector<std::string> command{"new-session", "-d", "-P", "-s", options.name};
  if (!options.first_window_name.empty()) {
    command.emplace_back("-n");
    command.push_back(options.first_window_name);
  }
  if (!options.start_directory.empty()) {
    command.emplace_back("-c");
    command.push_back(std::move(options.start_directory));
  }
  if (options.width.has_value()) {
    command.emplace_back("-x");
    command.push_back(std::to_string(*options.width));
  }
  if (options.height.has_value()) {
    command.emplace_back("-y");
    command.push_back(std::to_string(*options.height));
  }
  if (const auto env = detail::append_environment(command, options.environment);
      !env.has_value()) {
    return unexpected(env.error());
  }
  if (!options.shell_command.empty()) {
    command.emplace_back("--");
    command.push_back(std::move(options.shell_command));
  }
  auto created = detail::one_entity<Session>(backend_, std::move(command),
                                             FormatArgument::flag, options.name);
  return created;
#endif
}

expected<std::vector<OptionEntry>, CommandFailure>
Server::options(std::string_view target) const {
#if defined(_WIN32)
  static_cast<void>(target);
  return unexpected(unsupported_psmux_state("session option context"));
#else
  return show({"show-options", "-A"}, target);
#endif
}

expected<std::vector<OptionEntry>, CommandFailure> Server::server_options() const {
#if defined(_WIN32)
  return unexpected(unsupported_psmux_state("server options"));
#else
  return show({"show-options", "-s"}, {});
#endif
}

expected<void, CommandFailure> Server::set_server_option(std::string_view name,
                                                         std::string_view value) const {
#if defined(_WIN32)
  static_cast<void>(name);
  static_cast<void>(value);
  return unexpected(unsupported_psmux_state("server options"));
#else
  return applied(run({"set-option", "-s", std::string{name}, std::string{value}}));
#endif
}

expected<std::vector<OptionEntry>, CommandFailure> Server::global_options() const {
#if defined(_WIN32)
  return unexpected(unsupported_psmux_state("global options"));
#else
  return show({"show-options", "-g"}, {});
#endif
}

expected<void, CommandFailure> Server::set_global_option(std::string_view name,
                                                         std::string_view value) const {
#if defined(_WIN32)
  static_cast<void>(name);
  static_cast<void>(value);
  return unexpected(unsupported_psmux_state("global options"));
#else
  return applied(run({"set-option", "-g", std::string{name}, std::string{value}}));
#endif
}

expected<std::vector<OptionEntry>, CommandFailure>
Server::hooks(std::string_view target) const {
#if defined(_WIN32)
  static_cast<void>(target);
  return unexpected(unsupported_psmux_state("session hook context"));
#else
  return show({"show-hooks"}, target);
#endif
}

expected<std::vector<OptionEntry>, CommandFailure> Server::global_hooks() const {
#if defined(_WIN32)
  return unexpected(unsupported_psmux_state("global hooks"));
#else
  return show({"show-hooks", "-g"}, {});
#endif
}

expected<void, CommandFailure> Server::set_global_hook(std::string_view name,
                                                       std::string_view command) const {
#if defined(_WIN32)
  static_cast<void>(name);
  static_cast<void>(command);
  return unexpected(unsupported_psmux_state("global hooks"));
#else
  return applied(run({"set-hook", "-g", std::string{name}, std::string{command}}));
#endif
}

expected<std::vector<OptionEntry>, CommandFailure>
Server::show(std::vector<std::string> request, std::string_view target) const {
  if (!target.empty()) {
    request.emplace_back("-t");
    request.emplace_back(target);
  }
  auto output = run(request);
  if (!output.has_value()) {
    return unexpected(output.error());
  }
  return parse_options(*output);
}

LIBTMUX_NAMESPACE_END
