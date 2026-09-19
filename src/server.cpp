#include "libtmux/server.hpp"
#include "libtmux/format.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "acquire.hpp"
#include "backend.hpp"
#include "environment.hpp"
#include "path.hpp"
#include "psmux.hpp"

LIBTMUX_NAMESPACE_BEGIN

namespace {

[[nodiscard]] ConnectionOptions
routed_control_options(ConnectionOptions options, std::string socket_path,
                       std::string session, const std::filesystem::path& tmux_binary) {
  options.socket_path = std::move(socket_path);
  options.session_name = std::move(session);
  // The Server's executable, unless this caller named one here — `tmux`
  // included — so a Server pinned to a particular tmux does not open a
  // connection to whatever `PATH` finds.
  if (!options.tmux_binary.has_value()) {
    options.tmux_binary = tmux_binary;
  }
  return options;
}

// A command run for its effect: its output is not a result.
expected<void, CommandFailure> applied(expected<std::string, CommandFailure> reply) {
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return {};
}

[[nodiscard]] CommandFailure unsupported_psmux_state(std::string_view operation) {
  return CommandFailure{.kind = FailureKind::unsupported,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = "psmux cannot provide " + std::string{operation} +
                                      " through the typed API"};
}

#if defined(_WIN32)
[[nodiscard]] bool unavailable_candidate(const CommandFailure& failure) noexcept {
  return failure.kind == FailureKind::missing ||
         (failure.kind == FailureKind::refused &&
          libtmux_psmux::missing_session(failure.diagnostic));
}

[[nodiscard]] CommandFailure no_sessions() {
  return CommandFailure{.kind = FailureKind::refused,
                        .delivery = DeliveryStatus::replied,
                        .exit_code = 0,
                        .diagnostic = "the server has no sessions"};
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
                         .delivery = DeliveryStatus::replied,
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
      .delivery = DeliveryStatus::not_started,
      .exit_code = 0,
      .diagnostic = std::string{to_string(error)} + ": " + std::string{selector}};
}

expected<Server, CommandFailure> subprocess_server(std::vector<std::string> connection,
                                                   CommandObserver observer,
                                                   ExecutionPolicy policy) {
  auto backend = detail::SubprocessBackend::open(std::move(connection),
                                                 std::move(observer), policy);
  if (!backend.has_value()) {
    return unexpected(std::move(backend.error()));
  }
  return detail::server_over(*std::move(backend));
}

expected<Server, CommandFailure>
startable_subprocess_server(std::vector<std::string> connection,
                            std::optional<std::filesystem::path> configuration,
                            CommandObserver observer, ExecutionPolicy policy) {
  std::optional<std::string> configuration_text;
  if (configuration.has_value()) {
    configuration_text = configuration->string();
  }
  auto backend = detail::SubprocessBackend::open_startable(
      std::move(connection), std::move(configuration_text), std::move(observer),
      policy);
  if (!backend.has_value()) {
    return unexpected(std::move(backend.error()));
  }
  return detail::server_over(*std::move(backend));
}

} // namespace

namespace {

// Adapts a caller-supplied executor to the private backend interface.
//
// Every virtual the executor does not answer keeps its `Backend` default,
// which is the point of the narrow seam: `run_in_session`, `session_belongs`,
// batching and attach preparation stay the library's business rather than
// becoming a promise a custom transport has to keep.
class ExecutorBackend final : public detail::Backend {
public:
  ExecutorBackend(std::shared_ptr<const CommandExecutor> executor,
                  ExecutorOptions options, CommandObserver observer,
                  ExecutionPolicy policy, BackendKind kind = BackendKind::custom)
      : Backend{std::move(observer), policy}, executor_{std::move(executor)},
        options_{std::move(options)}, kind_{kind} {}

  [[nodiscard]] expected<std::string, CommandFailure>
  run(const CommandRequest& command, std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const override {
    auto answer = executor_->run(command, timeout, output_limit);
    if (const auto observer = command_observer(); observer.has_value()) {
      const std::string rendered = detail::rendered_command(command);
      (*observer)(rendered, answer.has_value() ? nullptr : &answer.error());
    }
    return answer;
  }

  [[nodiscard]] const std::vector<std::string>& connection() const noexcept override {
    static const std::vector<std::string> none;
    return none;
  }

  [[nodiscard]] std::string_view socket_path() const noexcept override {
    return options_.socket_path;
  }

  [[nodiscard]] ServerCapabilities capabilities() const noexcept override {
    return ServerCapabilities{.implementation = options_.implementation,
                              .backend = kind_};
  }

  [[nodiscard]] expected<Version, CommandFailure> version() const override {
    if (options_.version.has_value()) {
      return *options_.version;
    }
    auto printed =
        executor_->run(CommandRequest{{"-V"}}, policy().timeout, policy().output_limit);
    if (!printed.has_value()) {
      return unexpected(printed.error());
    }
    auto parsed = parse_version(*printed);
    if (!parsed.has_value()) {
      return unexpected(CommandFailure{
          .kind = FailureKind::refused,
          .delivery = DeliveryStatus::replied,
          .exit_code = 0,
          .diagnostic = "the executor answered -V with " + *printed +
                        "; name the version in ExecutorOptions if it cannot"});
    }
    return *parsed;
  }

private:
  std::shared_ptr<const CommandExecutor> executor_;
  ExecutorOptions options_;
  BackendKind kind_;
};

// What a control client answers completely inside a command's guarded block:
// commands the typed surface issues that tmux cannot leave running after
// `%end`. Checked against tmux, where only the twelve `cmd-*.c` files that
// return `CMD_RETURN_WAIT` can defer. Left out on purpose, and launched instead:
// those twelve; anything acting on the client itself (`attach-session`,
// `detach-client`, `switch-client`, `refresh-client`), since a control client
// is one; and anything able to end the connection it is sent on (`kill-*`,
// `new-session`). Full names only, so an alias or an abbreviation launches too.
constexpr std::array kAnsweredInBlock{
    std::string_view{"capture-pane"},    std::string_view{"clear-history"},
    std::string_view{"copy-mode"},       std::string_view{"delete-buffer"},
    std::string_view{"join-pane"},       std::string_view{"last-window"},
    std::string_view{"link-window"},     std::string_view{"list-buffers"},
    std::string_view{"list-clients"},    std::string_view{"list-commands"},
    std::string_view{"list-panes"},      std::string_view{"list-sessions"},
    std::string_view{"list-windows"},    std::string_view{"move-window"},
    std::string_view{"new-window"},      std::string_view{"next-layout"},
    std::string_view{"next-window"},     std::string_view{"paste-buffer"},
    std::string_view{"pipe-pane"},       std::string_view{"previous-layout"},
    std::string_view{"previous-window"}, std::string_view{"rename-session"},
    std::string_view{"rename-window"},   std::string_view{"resize-pane"},
    std::string_view{"resize-window"},   std::string_view{"respawn-pane"},
    std::string_view{"rotate-window"},   std::string_view{"select-layout"},
    std::string_view{"select-pane"},     std::string_view{"select-window"},
    std::string_view{"send-keys"},       std::string_view{"set-buffer"},
    std::string_view{"set-environment"}, std::string_view{"set-hook"},
    std::string_view{"set-option"},      std::string_view{"show-environment"},
    std::string_view{"show-hooks"},      std::string_view{"show-options"},
    std::string_view{"swap-pane"},       std::string_view{"swap-window"},
    std::string_view{"unlink-window"}};

// Whether any flag cluster before `--` sets one of `flags`. Over-matches an
// option value that happens to look like a cluster, which only costs a launch.
[[nodiscard]] bool sets_flag(const std::vector<std::string>& argv,
                             std::string_view flags) {
  for (std::size_t index = 1; index < argv.size(); ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--") {
      return false;
    }
    if (argument.size() > 1U && argument.front() == '-' &&
        argument.find_first_of(flags) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

// `display-message` defers only under `-I`, and `split-window` only under `-I`
// or `-W` (`cmd-split-window.c`); as sent otherwise, their block is the answer.
[[nodiscard]] bool answered_in_block(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return false;
  }
  const std::string_view name = argv.front();
  if (name == "display-message") {
    return !sets_flag(argv, "I");
  }
  if (name == "split-window") {
    return !sets_flag(argv, "IW");
  }
  return std::ranges::find(kAnsweredInBlock, name) != kAnsweredInBlock.end();
}

// Splits a flattened batch at its `;` elements. Absent when an argument ends in
// `;` without being one: tmux would split there too, and this cannot tell a
// separator from data the way tmux's own parser does.
[[nodiscard]] std::optional<std::vector<std::vector<std::string>>>
commands_in(const std::vector<std::string>& argv) {
  std::vector<std::vector<std::string>> commands(1);
  for (const std::string& argument : argv) {
    if (argument == ";") {
      commands.emplace_back();
      continue;
    }
    if (argument.ends_with(';')) {
      return std::nullopt;
    }
    commands.back().push_back(argument);
  }
  return commands;
}

class ControlExecutor final : public CommandExecutor {
public:
  ControlExecutor(std::vector<std::shared_ptr<Connection>> connections,
                  Server launching)
      : connections_{std::move(connections)}, launching_{std::move(launching)} {}

  [[nodiscard]] expected<std::string, CommandFailure>
  run(const CommandRequest& command, std::optional<std::chrono::milliseconds> timeout,
      std::optional<std::size_t> output_limit) const override {
    const std::vector<std::string> argv = command.argv();
    const auto commands = commands_in(argv);
    if (!commands.has_value() || !std::ranges::all_of(*commands, [](const auto& each) {
          return answered_in_block(each);
        })) {
      return launching_.run(command, timeout, output_limit);
    }
    ControlRequest request;
    for (const auto& each : *commands) {
      request.group.push_back(ControlCommand{.argv = each});
    }
    Connection& connection =
        *connections_[next_.fetch_add(1U, std::memory_order_relaxed) %
                      connections_.size()];
    const auto deadline =
        std::chrono::steady_clock::now() + timeout.value_or(std::chrono::seconds{30});
    const ControlRequestResult result =
        connection.execute(std::move(request), deadline);
    if (result.connection_error.has_value()) {
      // Nothing reached tmux, so launching is the same command, not a second
      // one. Past that point a mutation may have happened, and running it again
      // could do it twice.
      if (result.connection_error->delivery == DeliveryStatus::not_started) {
        return launching_.run(command, timeout, output_limit);
      }
      return unexpected(CommandFailure{
          .kind = std::chrono::steady_clock::now() >= deadline ? FailureKind::timeout
                                                               : FailureKind::pipe,
          .delivery = result.connection_error->delivery,
          .exit_code = -1,
          .diagnostic = result.connection_error->message +
                        " (running: " + detail::rendered_command(command) + ")"});
    }
    std::string answer;
    for (const ControlBlock& block : result.blocks) {
      std::string body;
      body.reserve(block.body.size());
      for (const std::byte byte : block.body) {
        body.push_back(static_cast<char>(byte));
      }
      if (block.body_truncated) {
        return unexpected(truncated(command, block.body_bytes));
      }
      if (block.terminal == ControlTerminal::error) {
        // The shape a launch reports: tmux's message, trimmed, and the command.
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
          body.pop_back();
        }
        return unexpected(CommandFailure{
            .kind = FailureKind::refused,
            .delivery = DeliveryStatus::replied,
            .exit_code = 1,
            .diagnostic =
                body + " (running: " + detail::rendered_command(command) + ")"});
      }
      answer += body;
    }
    if (output_limit.has_value() && answer.size() > *output_limit) {
      return unexpected(truncated(command, answer.size()));
    }
    return answer;
  }

private:
  [[nodiscard]] static CommandFailure truncated(const CommandRequest& command,
                                                std::size_t bytes) {
    return CommandFailure{.kind = FailureKind::truncated,
                          .delivery = DeliveryStatus::replied,
                          .exit_code = 0,
                          .diagnostic = "the reply ran to " + std::to_string(bytes) +
                                        " bytes, past what this call holds (running: " +
                                        detail::rendered_command(command) + ")"};
  }

  std::vector<std::shared_ptr<Connection>> connections_;
  Server launching_;
  mutable std::atomic<std::size_t> next_{0U};
};

} // namespace

expected<Server, CommandFailure>
Server::over(std::shared_ptr<const CommandExecutor> executor, ExecutorOptions options,
             CommandObserver observer, ExecutionPolicy policy) {
  if (executor == nullptr) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "no executor was given"});
  }
  return detail::server_over(std::make_shared<const ExecutorBackend>(
      std::move(executor), std::move(options), std::move(observer), policy));
}

expected<Server, CommandFailure> Server::at_socket_path(std::string_view path,
                                                        CommandObserver observer,
                                                        ExecutionPolicy policy) {
  auto arguments = socket_path_arguments(path);
  if (!arguments.has_value()) {
    return unexpected(rejected_selector(path, arguments.error()));
  }
  return subprocess_server(*std::move(arguments), std::move(observer), policy);
}

expected<Server, CommandFailure>
Server::startable_at_socket_path(std::string_view path,
                                 std::optional<std::filesystem::path> configuration,
                                 CommandObserver observer, ExecutionPolicy policy) {
  auto arguments = socket_path_arguments(path);
  if (!arguments.has_value()) {
    return unexpected(rejected_selector(path, arguments.error()));
  }
  return startable_subprocess_server(*std::move(arguments), std::move(configuration),
                                     std::move(observer), policy);
}

expected<Server, CommandFailure> Server::from_env(CommandObserver observer,
                                                  ExecutionPolicy policy) {
  const auto inherited = libtmux_env::value("TMUX");
  if (!inherited.has_value()) {
    return unexpected(CommandFailure{
        .kind = FailureKind::validation,
        .delivery = DeliveryStatus::not_started,
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
                                     .delivery = DeliveryStatus::not_started,
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
  return subprocess_server({}, std::move(observer), policy);
}

expected<Server, CommandFailure>
Server::startable_at_default(std::optional<std::filesystem::path> configuration,
                             CommandObserver observer, ExecutionPolicy policy) {
  return startable_subprocess_server({}, std::move(configuration), std::move(observer),
                                     policy);
}

expected<Server, CommandFailure> Server::at_socket_name(std::string_view name,
                                                        CommandObserver observer,
                                                        ExecutionPolicy policy) {
  if (auto invalid = libtmux_psmux::invalid_socket_name(name); invalid.has_value()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = std::move(*invalid)});
  }
  auto arguments = socket_name_arguments(name);
  if (!arguments.has_value()) {
    return unexpected(rejected_selector(name, arguments.error()));
  }
  return subprocess_server(*std::move(arguments), std::move(observer), policy);
}

expected<Server, CommandFailure>
Server::startable_at_socket_name(std::string_view name,
                                 std::optional<std::filesystem::path> configuration,
                                 CommandObserver observer, ExecutionPolicy policy) {
  if (auto invalid = libtmux_psmux::invalid_socket_name(name); invalid.has_value()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = std::move(*invalid)});
  }
  auto arguments = socket_name_arguments(name);
  if (!arguments.has_value()) {
    return unexpected(rejected_selector(name, arguments.error()));
  }
  return startable_subprocess_server(*std::move(arguments), std::move(configuration),
                                     std::move(observer), policy);
}

bool Server::refuses(ServerFeature feature) const noexcept {
  return backend_ != nullptr && backend_->capabilities().refuses(feature);
}

ServerCapabilities Server::capabilities() const noexcept {
  return backend_->capabilities();
}

std::string_view Server::socket_path() const noexcept {
  return backend_ == nullptr ? std::string_view{} : backend_->selected_socket_path();
}

expected<std::string, CommandFailure>
Server::run(const CommandRequest& command,
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
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "empty batch"});
  }
  const ExecutionPolicy& policy = backend_->policy();
  return backend_->run_batch(batch, policy.timeout, policy.output_limit);
}

expected<std::string, CommandFailure> Server::run_chain(const Chain& chain) const {
  if (!chain.valid()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
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
        ProtocolError{.message = std::string{to_string(available.implementation)} +
                                 " backend does not support persistent control mode",
                      .delivery = DeliveryStatus::not_started});
  }
  // The route is not the identity: a pinned POSIX handle identifies an inode
  // incarnation while connecting through its private alias.
  const std::string_view socket_path = backend_->socket_path();
  if (socket_path.empty()) {
    return unexpected(
        ProtocolError{.message = "this server has no socket to connect to",
                      .delivery = DeliveryStatus::not_started});
  }
  return Connection::connect(
      routed_control_options(std::move(options), std::string{socket_path},
                             std::string{session}, backend_->policy().tmux_binary));
}

expected<Server, ProtocolError> Server::over_control(std::string_view session,
                                                     std::size_t connections) const {
  if (connections == 0U) {
    return unexpected(
        ProtocolError{.message = "a control-backed Server needs a connection",
                      .delivery = DeliveryStatus::not_started});
  }
  std::vector<std::shared_ptr<Connection>> pool;
  pool.reserve(connections);
  for (std::size_t index = 0; index < connections; ++index) {
    auto connected = control(session);
    if (!connected.has_value()) {
      return unexpected(std::move(connected.error()));
    }
    pool.push_back(std::make_shared<Connection>(*std::move(connected)));
  }
  // Without this Server's observer: the control-backed Server reports every
  // command, launched or not, and a launch reported again would count twice.
  auto launching =
      Server::at_socket_path(std::string{socket_path()}, {}, backend_->policy());
  if (!launching.has_value()) {
    return unexpected(ProtocolError{.message = launching.error().diagnostic,
                                    .delivery = DeliveryStatus::not_started});
  }
  // Named here rather than asked through the executor: `-V` is not a command a
  // control client can answer, and this Server has usually asked already.
  const auto version = backend_->version();
  auto executor =
      std::make_shared<const ControlExecutor>(std::move(pool), *std::move(launching));
  return detail::server_over(std::make_shared<const ExecutorBackend>(
      std::move(executor),
      ExecutorOptions{.implementation = capabilities().implementation,
                      .socket_path = std::string{socket_path()},
                      .version = version.has_value() ? std::optional<Version>{*version}
                                                     : std::nullopt},
      backend_->command_observer().value_or(CommandObserver{}), backend_->policy(),
      BackendKind::control));
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
                                     .delivery = DeliveryStatus::replied,
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
          .delivery = DeliveryStatus::replied,
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
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("clients"));
  }
  return detail::list_entities<Client>(backend_, {"list-clients"});
}

expected<void, CommandFailure>
Server::wait_for(std::string_view channel,
                 std::optional<std::chrono::milliseconds> timeout) const {
  if (channel.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "a channel needs a name"});
  }
  if (refuses(ServerFeature::wait_channels)) {
    return unexpected(unsupported_psmux_state("wait channels"));
  }
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
        .delivery = DeliveryStatus::replied,
        .exit_code = 0,
        .diagnostic = "the server ended while waiting on " + std::string{channel}});
  }
  return {};
}

expected<void, CommandFailure> Server::signal(std::string_view channel) const {
  if (channel.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "a channel needs a name"});
  }
  if (refuses(ServerFeature::wait_channels)) {
    return unexpected(unsupported_psmux_state("wait channels"));
  }
  return applied(run({"wait-for", "-S", std::string{channel}}));
}

expected<std::vector<Command>, CommandFailure> Server::commands() const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("command metadata"));
  }
  return detail::list_entities<Command>(backend_, {"list-commands"});
}

expected<std::vector<Buffer>, CommandFailure> Server::buffers() const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("buffers"));
  }
  return detail::list_entities<Buffer>(backend_, {"list-buffers"});
}

expected<void, CommandFailure>
Server::load_buffer(std::string_view name, const std::filesystem::path& from) const {
  if (name.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "a buffer needs a name"});
  }
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("buffers"));
  }
  return applied(run({"load-buffer", "-b", std::string{name}, "--",
                      libtmux_path::command_string(from)}));
}

expected<void, CommandFailure>
Server::save_buffer(std::string_view name, const std::filesystem::path& to) const {
  if (name.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "a buffer needs a name"});
  }
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("buffers"));
  }
  return applied(run({"save-buffer", "-b", std::string{name}, "--",
                      libtmux_path::command_string(to)}));
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
                                     .delivery = DeliveryStatus::not_started,
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
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "a binding needs a command to run"});
  }
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("key bindings"));
  }
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
}

expected<void, CommandFailure> Server::unbind_key(std::string_view table,
                                                  std::string_view key) const {
  if (const auto usable = usable_table(table); !usable.has_value()) {
    return unexpected(usable.error());
  }
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("key bindings"));
  }
  std::vector<std::string> argv{"unbind-key", "-T", std::string{table}};
  argv.emplace_back("--");
  argv.emplace_back(key);
  return applied(run(argv));
}

expected<void, CommandFailure> Server::run_shell(std::string_view command,
                                                 bool background) const {
  if (command.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "a shell command cannot be empty"});
  }
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("run-shell state"));
  }
  CommandRequest argv{"run-shell"};
  if (background) {
    argv.emplace_back("-b");
  }
  argv.emplace_back("--");
  argv.push_back(CommandArgument::sensitive(std::string{command}));
  return applied(run(argv));
}

expected<void, CommandFailure>
Server::source_file(const std::filesystem::path& file) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("configuration state"));
  }
  return applied(run({"source-file", "--", libtmux_path::command_string(file)}));
}

expected<void, CommandFailure>
Server::check_file(const std::filesystem::path& file) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(CommandFailure{
        .kind = FailureKind::unsupported,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic =
            "psmux cannot check a source file without executing its commands"});
  }
  return applied(run({"source-file", "-n", "--", libtmux_path::command_string(file)}));
}

expected<std::string, CommandFailure> Server::expand(std::string_view format) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("format context"));
  }
  std::vector<std::string> command{"display-message", "-p"};
  detail::append_display_message_text(command, std::string{format});
  auto reply = run(command);
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return detail::without_trailing_newline(std::move(*reply));
}

expected<void, CommandFailure> Server::show_message(std::string_view text) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("message state"));
  }
  std::vector<std::string> command{"display-message"};
  detail::append_display_message_text(command, std::string{text});
  return applied(run(command));
}

expected<void, CommandFailure> Server::set_buffer(std::string_view name,
                                                  std::string_view data) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("buffers"));
  }
  std::vector<std::string> command{"set-buffer"};
  if (!name.empty()) {
    command.emplace_back("-b");
    command.emplace_back(name);
  }
  // `--` first: buffer text beginning with a dash is text, not a flag.
  command.emplace_back("--");
  command.emplace_back(data);
  return applied(run(command));
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
            .delivery = DeliveryStatus::not_started,
            .exit_code = 0,
            .diagnostic = "psmux session target is ambiguous: " + std::string{target}});
      }
      found = candidate;
    }
  }
  if (!found.has_value()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::missing,
                       .delivery = DeliveryStatus::replied,
                       .exit_code = 0,
                       .diagnostic = "tmux has no session " + std::string{target}});
  }
  return *std::move(found);
#else
  return detail::describe<Session>(backend_, target);
#endif
}

expected<Window, CommandFailure> Server::window(std::string_view target) const {
  if (refuses(ServerFeature::server_entity_lookup)) {
    return unexpected(
        CommandFailure{.kind = FailureKind::unsupported,
                       .delivery = DeliveryStatus::not_started,
                       .exit_code = 0,
                       .diagnostic = "psmux window targets need an owning Session: " +
                                     std::string{target}});
  }
  return detail::describe<Window>(backend_, target);
}

expected<Pane, CommandFailure> Server::pane(std::string_view target) const {
  if (refuses(ServerFeature::server_entity_lookup)) {
    return unexpected(
        CommandFailure{.kind = FailureKind::unsupported,
                       .delivery = DeliveryStatus::not_started,
                       .exit_code = 0,
                       .diagnostic = "psmux pane targets need an owning Session: " +
                                     std::string{target}});
  }
  return detail::describe<Pane>(backend_, target);
}

expected<Session, CommandFailure> Server::new_session(std::string_view name) const {
  return new_session(NewSessionOptions{.name = std::string{name}});
}

expected<Session, CommandFailure> Server::new_session(NewSessionOptions options) const {
  if (options.name.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = "session name is empty"});
  }
  if (auto invalid = libtmux_psmux::invalid_session_name(options.name);
      invalid.has_value()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .delivery = DeliveryStatus::not_started,
                                     .exit_code = 0,
                                     .diagnostic = std::move(*invalid)});
  }
  if (refuses(ServerFeature::session_creation)) {
    return unexpected(CommandFailure{
        .kind = FailureKind::unsupported,
        .delivery = DeliveryStatus::not_started,
        .exit_code = 0,
        .diagnostic =
            "psmux cannot prove ownership of a concurrently created session; "
            "create it with the psmux CLI and reacquire it from this Server"});
  }
  CommandRequest command{"new-session", "-d", "-P", "-s", escape_literal(options.name)};
  if (!options.first_window_name.empty()) {
    command.emplace_back("-n");
    command.push_back(escape_literal(options.first_window_name));
  }
  if (!options.start_directory.empty()) {
    command.emplace_back("-c");
    command.push_back(escape_literal(options.start_directory));
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
    command.push_back(CommandArgument::sensitive(std::move(options.shell_command)));
  }
  auto created = detail::one_entity<Session>(backend_, std::move(command),
                                             FormatArgument::flag, options.name);
  return created;
}

expected<std::vector<OptionEntry>, CommandFailure>
Server::options(std::string_view target) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("session option context"));
  }
  return show({"show-options", "-A"}, target);
}

expected<std::vector<OptionEntry>, CommandFailure> Server::server_options() const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("server options"));
  }
  return show({"show-options", "-s"}, {});
}

expected<void, CommandFailure> Server::set_server_option(std::string_view name,
                                                         std::string_view value) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("server options"));
  }
  CommandRequest command{"set-option", "-s", std::string{name}};
  command.push_back(CommandArgument::sensitive(std::string{value}));
  return applied(run(command));
}

expected<std::vector<OptionEntry>, CommandFailure> Server::global_options() const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("global options"));
  }
  return show({"show-options", "-g"}, {});
}

expected<void, CommandFailure> Server::set_global_option(std::string_view name,
                                                         std::string_view value) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("global options"));
  }
  CommandRequest command{"set-option", "-g", std::string{name}};
  command.push_back(CommandArgument::sensitive(std::string{value}));
  return applied(run(command));
}

expected<std::vector<OptionEntry>, CommandFailure>
Server::hooks(std::string_view target) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("session hook context"));
  }
  return show({"show-hooks"}, target);
}

expected<std::vector<OptionEntry>, CommandFailure> Server::global_hooks() const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("global hooks"));
  }
  return show({"show-hooks", "-g"}, {});
}

expected<void, CommandFailure> Server::set_global_hook(std::string_view name,
                                                       std::string_view command) const {
  if (refuses(ServerFeature::server_state)) {
    return unexpected(unsupported_psmux_state("global hooks"));
  }
  CommandRequest request{"set-hook", "-g", std::string{name}};
  request.push_back(CommandArgument::sensitive(std::string{command}));
  return applied(run(request));
}

expected<std::vector<EnvironmentEntry>, CommandFailure> Server::environment() const {
  auto output = run({"show-environment", "-g"});
  if (!output.has_value()) {
    return unexpected(output.error());
  }
  std::vector<EnvironmentEntry> entries;
  std::size_t start = 0;
  while (start < output->size()) {
    const auto stop = output->find('\n', start);
    const std::string_view line{output->data() + start,
                                (stop == std::string::npos ? output->size() : stop) -
                                    start};
    start = stop == std::string::npos ? output->size() : stop + 1;
    if (line.empty()) {
      continue;
    }
    // `-NAME` is a name tmux will take out of a child's environment, and
    // `NAME=value` is one it will put in. A name cannot begin with `-`, so
    // the leading byte settles which this is without splitting first.
    if (line.front() == '-') {
      entries.push_back(
          EnvironmentEntry{.name = std::string{line.substr(1)}, .value = std::nullopt});
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) {
      // tmux prints no such line; keeping it as a bound name rather than
      // dropping it means an unexpected shape is visible instead of missing.
      entries.push_back(
          EnvironmentEntry{.name = std::string{line}, .value = std::string{}});
      continue;
    }
    entries.push_back(EnvironmentEntry{.name = std::string{line.substr(0, equals)},
                                       .value = std::string{line.substr(equals + 1)}});
  }
  return entries;
}

expected<void, CommandFailure> Server::set_environment(std::string_view name,
                                                       std::string_view value) const {
  return applied(run({"set-environment", "-g", std::string{name}, std::string{value}}));
}

expected<void, CommandFailure> Server::unset_environment(std::string_view name) const {
  return applied(run({"set-environment", "-g", "-u", std::string{name}}));
}

expected<void, CommandFailure> Server::remove_environment(std::string_view name) const {
  return applied(run({"set-environment", "-g", "-r", std::string{name}}));
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
