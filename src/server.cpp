#include "libtmux/server.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "acquire.hpp"
#include "backend.hpp"
#include "control_backend.hpp"

LIBTMUX_NAMESPACE_BEGIN

namespace {

// A command run for its effect: its output is not a result.
expected<void, CommandFailure> applied(expected<std::string, CommandFailure> reply) {
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return {};
}

} // namespace

namespace detail {

Server server_over(std::shared_ptr<const Backend> backend) {
  return Server{std::move(backend)};
}

} // namespace detail

namespace {

CommandFailure rejected_selector(std::string_view selector, SocketError error) {
  return CommandFailure{.kind = FailureKind::validation,
                        .dispatched = false,
                        .exit_code = 0,
                        .diagnostic = std::string{to_string(error)} + ": " +
                                      std::string{selector}};
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
  const char* const inherited = std::getenv("TMUX");
  if (inherited == nullptr || *inherited == '\0') {
    return unexpected(CommandFailure{
        .kind = FailureKind::validation,
        .dispatched = false,
        .exit_code = 0,
        .diagnostic = "TMUX is not set: this process is not running inside tmux"});
  }
  // `<socket path>,<server pid>,<session id>`. Only the first field is
  // trustworthy, and a socket path may itself contain a comma, so the split is
  // at the last one that could begin the pid.
  const std::string_view value{inherited};
  const auto pid_start = value.find_last_of(',', value.find_last_of(',') - 1U);
  const std::string_view socket =
      pid_start == std::string_view::npos ? value : value.substr(0, pid_start);
  if (socket.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "TMUX names no socket path"});
  }
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
  auto arguments = socket_name_arguments(name);
  if (!arguments.has_value()) {
    return unexpected(rejected_selector(name, arguments.error()));
  }
  return detail::server_over(std::make_shared<const detail::SubprocessBackend>(
      *std::move(arguments), std::move(observer), policy));
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
  // A control client is launched against a path, and every selector resolves
  // to one — tmux resolves the same rule to find the socket in the first
  // place. Only a server with no socket at all, which is a scripted backend in
  // a test, has nothing to hand it.
  const std::string_view resolved = backend_->identity();
  if (resolved.empty()) {
    return unexpected(ProtocolError{"this server has no socket to connect to"});
  }
  ConnectionOptions options;
  options.socket_path = std::string{resolved};
  options.session_name = std::string{session};
  return Connection::connect(std::move(options));
}

expected<Version, CommandFailure> Server::tmux_version() const {
  return backend_->version();
}

bool Server::is_alive(std::chrono::milliseconds timeout) const {
  return check_alive(timeout).has_value();
}

expected<void, CommandFailure>
Server::check_alive(std::chrono::milliseconds timeout) const {
  // Listing sessions is the cheapest command that requires the server to
  // answer. A server with no sessions has already exited, so an empty list is
  // not a state this can observe.
  return applied(run({"list-sessions"}, timeout));
}

expected<void, CommandFailure> Server::kill() const {
  return applied(run({"kill-server"}));
}

std::vector<Notification> Server::take_notifications() const {
  return backend_->take_notifications();
}

std::size_t Server::dropped_notifications() const noexcept {
  return backend_->dropped_notifications();
}

expected<Server, CommandFailure> Server::over_control(std::string_view session) const {
  const std::vector<std::string>& selector = backend_->connection();
  // Every selector resolves to a path, so `-L work` and the default socket
  // reach the faster transport too. They could not before, which left the
  // measured 4.6x behind whichever of the four constructors a caller had
  // happened to pick.
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
      backend_->observer(), backend_->policy());
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
  return detail::list_entities<Session>(backend_, {"list-sessions"});
}

expected<std::vector<Window>, CommandFailure> Server::windows() const {
  return detail::list_entities<Window>(backend_, {"list-windows", "-a"});
}

expected<std::vector<Pane>, CommandFailure> Server::panes() const {
  return detail::list_entities<Pane>(backend_, {"list-panes", "-a"});
}

expected<std::vector<Client>, CommandFailure> Server::clients() const {
  return detail::list_entities<Client>(backend_, {"list-clients"});
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
}

expected<void, CommandFailure> Server::signal(std::string_view channel) const {
  if (channel.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a channel needs a name"});
  }
  return applied(run({"wait-for", "-S", std::string{channel}}));
}

expected<std::vector<Command>, CommandFailure> Server::commands() const {
  return detail::list_entities<Command>(backend_, {"list-commands"});
}

expected<std::vector<Buffer>, CommandFailure> Server::buffers() const {
  return detail::list_entities<Buffer>(backend_, {"list-buffers"});
}

expected<void, CommandFailure>
Server::load_buffer(std::string_view name, const std::filesystem::path& from) const {
  if (name.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a buffer needs a name"});
  }
  return applied(run({"load-buffer", "-b", std::string{name}, "--", from.string()}));
}

expected<void, CommandFailure>
Server::save_buffer(std::string_view name, const std::filesystem::path& to) const {
  if (name.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a buffer needs a name"});
  }
  return applied(run({"save-buffer", "-b", std::string{name}, "--", to.string()}));
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
  return applied(run({"unbind-key", "-T", std::string{table}, "--", std::string{key}}));
}

expected<void, CommandFailure> Server::run_shell(std::string_view command,
                                                 bool background) const {
  if (command.empty()) {
    return unexpected(CommandFailure{.kind = FailureKind::validation,
                                     .dispatched = false,
                                     .exit_code = 0,
                                     .diagnostic = "a shell command cannot be empty"});
  }
  std::vector<std::string> argv{"run-shell"};
  if (background) {
    argv.emplace_back("-b");
  }
  argv.emplace_back("--");
  argv.emplace_back(command);
  return applied(run(argv));
}

expected<void, CommandFailure>
Server::source_file(const std::filesystem::path& file) const {
  return applied(run({"source-file", "--", file.string()}));
}

expected<void, CommandFailure>
Server::check_file(const std::filesystem::path& file) const {
  return applied(run({"source-file", "-n", "--", file.string()}));
}

expected<std::string, CommandFailure> Server::expand(std::string_view format) const {
  auto reply = run({"display-message", "-p", "--", std::string{format}});
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return detail::without_trailing_newline(std::move(*reply));
}

expected<void, CommandFailure> Server::show_message(std::string_view text) const {
  return applied(run({"display-message", "--", std::string{text}}));
}

expected<void, CommandFailure> Server::set_buffer(std::string_view name,
                                                  std::string_view data) const {
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
  return detail::describe<Session>(backend_, target);
}

expected<Window, CommandFailure> Server::window(std::string_view target) const {
  return detail::describe<Window>(backend_, target);
}

expected<Pane, CommandFailure> Server::pane(std::string_view target) const {
  return detail::describe<Pane>(backend_, target);
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
  std::vector<std::string> command{"new-session", "-d", "-P", "-s", options.name};
  if (!options.first_window_name.empty()) {
    command.emplace_back("-n");
    command.push_back(std::move(options.first_window_name));
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
  return detail::one_entity<Session>(backend_, std::move(command), FormatArgument::flag,
                                     options.name);
}

expected<std::vector<OptionEntry>, CommandFailure>
Server::options(std::string_view target) const {
  return show({"show-options", "-A"}, target);
}

expected<std::vector<OptionEntry>, CommandFailure> Server::server_options() const {
  return show({"show-options", "-s"}, {});
}

expected<void, CommandFailure> Server::set_server_option(std::string_view name,
                                                         std::string_view value) const {
  return applied(run({"set-option", "-s", std::string{name}, std::string{value}}));
}

expected<std::vector<OptionEntry>, CommandFailure> Server::global_options() const {
  return show({"show-options", "-g"}, {});
}

expected<void, CommandFailure> Server::set_global_option(std::string_view name,
                                                         std::string_view value) const {
  return applied(run({"set-option", "-g", std::string{name}, std::string{value}}));
}

expected<std::vector<OptionEntry>, CommandFailure>
Server::hooks(std::string_view target) const {
  return show({"show-hooks"}, target);
}

expected<std::vector<OptionEntry>, CommandFailure> Server::global_hooks() const {
  return show({"show-hooks", "-g"}, {});
}

expected<void, CommandFailure> Server::set_global_hook(std::string_view name,
                                                       std::string_view command) const {
  return applied(run({"set-hook", "-g", std::string{name}, std::string{command}}));
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
