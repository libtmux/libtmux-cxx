#include "libtmux/entities.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <ostream>

#include "libtmux/format.hpp"
#include "libtmux/keys.hpp"
#include "libtmux/options.hpp"
#include "libtmux/server.hpp"

#include "acquire.hpp"
#include "psmux.hpp"

LIBTMUX_NAMESPACE_BEGIN

struct AttachCommand::State {
  State(std::vector<std::string> command,
        std::shared_ptr<const detail::SocketAlias> route_lifetime)
      : argv{std::move(command)}, route{std::move(route_lifetime)} {}

  std::vector<std::string> argv;
  std::shared_ptr<const detail::SocketAlias> route;
};

AttachCommand::AttachCommand(std::shared_ptr<const State> state) noexcept
    : state_{std::move(state)} {}
AttachCommand::AttachCommand(const AttachCommand&) noexcept = default;
AttachCommand::AttachCommand(AttachCommand&&) noexcept = default;
AttachCommand& AttachCommand::operator=(const AttachCommand&) noexcept = default;
AttachCommand& AttachCommand::operator=(AttachCommand&&) noexcept = default;
AttachCommand::~AttachCommand() = default;

const std::vector<std::string>& AttachCommand::argv() const noexcept {
  static const std::vector<std::string> empty;
  return state_ == nullptr ? empty : state_->argv;
}

expected<std::string, CommandFailure>
detail::Row::run(const CommandRequest& command,
                 std::optional<std::size_t> output_limit) const {
  if (backend() == nullptr) {
    return unexpected(detail::disconnected());
  }
  // Every typed method arrives here, and every one of them used to pass no
  // deadline at all: `window.rename(...)` against a tmux that stopped
  // answering held the calling thread for the life of the process.
  const ExecutionPolicy& policy = backend()->policy();
#if defined(_WIN32)
  const std::size_t session_id = snapshot_->index_of("session_id");
  const std::size_t session_name = snapshot_->index_of("session_name");
  if (session_id < snapshot_->rows()[row_].size()) {
    if (snapshot_->rows()[row_][session_id].empty()) {
      return unexpected(
          CommandFailure{.kind = FailureKind::validation,
                         .delivery = DeliveryStatus::not_started,
                         .exit_code = 0,
                         .diagnostic = "psmux entity has no stable session id"});
    }
    const std::string_view name = session_name < snapshot_->rows()[row_].size()
                                      ? snapshot_->rows()[row_][session_name]
                                      : std::string_view{};
    return backend()->run_in_session(
        command, snapshot_->rows()[row_][session_id], name, policy.timeout,
        output_limit.has_value() ? output_limit : policy.output_limit);
  }
#endif
  return backend()->run(command, policy.timeout,
                        output_limit.has_value() ? output_limit : policy.output_limit);
}

namespace {

// Do these two values name objects on two different tmux servers?
//
// tmux numbers ids per server, so `$0`, `@0` and `%0` exist on almost every
// socket at once. A command combining two entities runs against one of their
// servers and carries the other's id as text, so across two servers it finds a
// live, unrelated object with that id there — and tmux reports success.
// Nothing downstream can detect that, which is why it is refused here.
//
// An unidentified value — one read out of a recording — is not on a server
// rather than on a different one, and `run` says that better than this could.
template <typename Left, typename Right>
[[nodiscard]] bool from_different_servers(const Left& left,
                                          const Right& right) noexcept {
  const std::string_view mine = left.connection_identity();
  const std::string_view theirs = right.connection_identity();
  return !mine.empty() && !theirs.empty() && mine != theirs;
}

[[nodiscard]] CommandFailure crossed_servers() {
  return CommandFailure{.kind = FailureKind::validation,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic =
                            "the two values name objects on different tmux servers"};
}

} // namespace

std::string_view detail::Row::connection_identity() const noexcept {
  const auto& connection = backend();
  return connection == nullptr ? std::string_view{} : connection->identity();
}

bool detail::Row::same_connection(const Row& other) const noexcept {
  return detail::same_server(backend().get(), other.backend().get());
}

expected<Server, CommandFailure> detail::Row::server() const {
  if (backend() == nullptr) {
    return unexpected(detail::disconnected());
  }
  return detail::server_over(backend());
}

namespace {

#if !defined(_WIN32)
// A command run for its effect. The output of `kill-window` is not a result.
expected<void, CommandFailure> effect(expected<std::string, CommandFailure> reply) {
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return {};
}
#endif

CommandFailure rejected(std::string diagnostic) {
  return CommandFailure{.kind = FailureKind::validation,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}

#if defined(_WIN32)
CommandFailure unsupported(std::string diagnostic) {
  return CommandFailure{.kind = FailureKind::unsupported,
                        .delivery = DeliveryStatus::not_started,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}
#endif

#if !defined(_WIN32)
// tmux prints options one per line in the same shape at every scope, so the
// only thing that differs between the four is the flag that selects it.
expected<std::vector<OptionEntry>, CommandFailure>
listed(expected<std::string, CommandFailure> reply) {
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return parse_options(*reply);
}

// One named option. tmux answers a name it knows but has no value for with an
// empty listing and a zero exit status, which is a different outcome from the
// name being wrong: that it refuses.
expected<OptionEntry, CommandFailure> named(expected<std::string, CommandFailure> reply,
                                            std::string_view name) {
  auto entries = listed(std::move(reply));
  if (!entries.has_value()) {
    return unexpected(entries.error());
  }
  if (entries->empty()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::missing,
                       .delivery = DeliveryStatus::replied,
                       .exit_code = 0,
                       .diagnostic = "no value set for option " + std::string{name}});
  }
  OptionEntry entry = std::move(entries->front());
  // tmux accepts a user option whose name ends in an asterisk, and prints one
  // set here exactly as it prints the inheritance marker on a name that does
  // not. A listing cannot tell those apart; this can, because it knows which
  // name it asked for.
  if (entry.inherited && entry.name != name && entry.name + "*" == name) {
    entry.name = std::string{name};
    entry.inherited = false;
  }
  return entry;
}

// Build a scoped option command. `scope` is the tmux flag that selects the
// scope, empty for session options, which are the unflagged default.
std::vector<std::string> scoped(std::string_view verb, std::string_view scope,
                                std::string_view target,
                                std::initializer_list<std::string_view> rest) {
  std::vector<std::string> command{std::string{verb}};
  if (!scope.empty()) {
    command.emplace_back(scope);
  }
  command.emplace_back("-t");
  command.emplace_back(target);
  for (const std::string_view argument : rest) {
    command.emplace_back(argument);
  }
  return command;
}
#endif

std::string session_target(const Session& session) {
#if defined(_WIN32)
  static_cast<void>(session);
  return ":";
#else
  return std::string{session.id()};
#endif
}

detail::SessionRoute session_route(const Session& session) {
  return {.id = session.id(), .name = session.name()};
}

detail::SessionRoute session_route(const Window& window) {
  return {.id = window.session_id(), .name = window.session_name()};
}

detail::SessionRoute session_route(const Pane& pane) {
  return {.id = pane.session_id(), .name = pane.session_name()};
}

} // namespace

// --- Session ---------------------------------------------------------------

expected<std::vector<Window>, CommandFailure> Session::windows() const {
  return detail::list_entities<Window>(
      backend(), {"list-windows", "-t", session_target(*this)}, session_route(*this));
}

expected<std::vector<Pane>, CommandFailure> Session::panes() const {
  return detail::list_entities<Pane>(backend(),
                                     {"list-panes", "-s", "-t", session_target(*this)},
                                     session_route(*this));
}

expected<Window, CommandFailure> Session::active_window() const {
  return detail::describe<Window>(backend(), session_target(*this), {},
                                  session_route(*this));
}

expected<Pane, CommandFailure> Session::active_pane() const {
  return detail::describe<Pane>(backend(), session_target(*this), {},
                                session_route(*this));
}

namespace {

// Each of these selects, then reports where the selection ended up. The
// second call is what makes the result useful; tmux's own commands print
// nothing.
#if !defined(_WIN32)
expected<Window, CommandFailure> moved(const Session& session,
                                       expected<std::string, CommandFailure> ran) {
  if (!ran.has_value()) {
    return unexpected(ran.error());
  }
  return session.active_window();
}
#endif

} // namespace

expected<Window, CommandFailure> Session::select_next_window() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot atomically target session navigation"));
#else
  return moved(*this, run({"next-window", "-t", session_target(*this)}));
#endif
}

expected<Window, CommandFailure> Session::select_previous_window() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot atomically target session navigation"));
#else
  return moved(*this, run({"previous-window", "-t", session_target(*this)}));
#endif
}

expected<Window, CommandFailure> Session::select_last_window() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot atomically target session navigation"));
#else
  return moved(*this, run({"last-window", "-t", session_target(*this)}));
#endif
}

expected<Window, CommandFailure> Session::new_window(std::string_view name) const {
  return new_window(NewWindowOptions{.name = std::string{name}});
}

expected<Window, CommandFailure> Session::new_window(NewWindowOptions options) const {
  if (options.name.empty()) {
    return unexpected(rejected("window name is empty"));
  }
#if defined(_WIN32)
  static_cast<void>(options);
  return unexpected(
      unsupported("psmux can report a failed new-window as an existing window"));
#else
  std::string target = session_target(*this);
  if (options.index.has_value()) {
    target += ":" + std::to_string(*options.index);
  }
  CommandRequest command{"new-window", "-t", std::move(target),
                         "-P",         "-n", options.name};
  if (!options.focus) {
    command.emplace_back("-d");
  }
  if (options.after_current) {
    command.emplace_back("-a");
  }
  if (!options.start_directory.empty()) {
    command.emplace_back("-c");
    command.push_back(std::move(options.start_directory));
  }
  if (const auto env = detail::append_environment(command, options.environment);
      !env.has_value()) {
    return unexpected(env.error());
  }
  if (!options.shell_command.empty()) {
    // Positional, so it goes last and after `--`: a command beginning with a
    // dash is data here, not a flag of new-window.
    command.emplace_back("--");
    command.push_back(CommandArgument::sensitive(std::move(options.shell_command)));
  }
  return detail::one_entity<Window>(backend(), std::move(command), FormatArgument::flag,
                                    options.name, {}, session_route(*this));
#endif
}

expected<void, CommandFailure> Session::rename(std::string_view name) const {
  if (name.empty()) {
    return unexpected(rejected("session name is empty"));
  }
#if defined(_WIN32)
  if (auto invalid = libtmux_psmux::invalid_session_name(name); invalid.has_value()) {
    return unexpected(rejected(std::move(*invalid)));
  }
  return unexpected(unsupported(
      "psmux cannot atomically prevent rename-session from replacing a session"));
#else
  std::vector<std::string> command{"rename-session", "-t", session_target(*this)};
  command.emplace_back("--");
  command.emplace_back(name);
  return effect(run(command));
#endif
}

expected<void, CommandFailure> Session::kill() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot atomically kill a captured session"));
#else
  return effect(run({"kill-session", "-t", session_target(*this)}));
#endif
}

expected<Session, CommandFailure> Session::refresh() const {
  return detail::describe<Session>(backend(), session_target(*this), id(),
                                   session_route(*this));
}

expected<std::vector<OptionEntry>, CommandFailure> Session::options() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot atomically read session options"));
#else
  return listed(run(scoped("show-options", {}, session_target(*this), {"-A"})));
#endif
}

expected<OptionEntry, CommandFailure> Session::option(std::string_view name) const {
#if defined(_WIN32)
  static_cast<void>(name);
  return unexpected(unsupported("psmux cannot atomically read a session option"));
#else
  return named(run(scoped("show-options", {}, session_target(*this), {"-A", name})),
               name);
#endif
}

expected<void, CommandFailure> Session::set_option(std::string_view name,
                                                   std::string_view value) const {
#if defined(_WIN32)
  static_cast<void>(name);
  static_cast<void>(value);
  return unexpected(unsupported("psmux cannot atomically set a session option"));
#else
  CommandRequest command = scoped("set-option", {}, session_target(*this), {name});
  command.push_back(CommandArgument::sensitive(std::string{value}));
  return effect(run(command));
#endif
}

expected<void, CommandFailure> Session::unset_option(std::string_view name) const {
#if defined(_WIN32)
  static_cast<void>(name);
  return unexpected(unsupported("psmux cannot atomically unset a session option"));
#else
  return effect(run(scoped("set-option", {}, session_target(*this), {"-u", name})));
#endif
}

expected<std::string, CommandFailure> Session::expand(std::string_view format) const {
  return detail::expand_format<Session>(backend(), session_target(*this), format, id(),
                                        session_route(*this));
}

expected<void, CommandFailure> Session::show_message(std::string_view text) const {
#if defined(_WIN32)
  static_cast<void>(text);
  return unexpected(unsupported("psmux cannot safely preserve a session message"));
#else
  std::vector<std::string> command{"display-message", "-t", session_target(*this)};
  detail::append_display_message_text(command, std::string{text});
  return effect(run(command));
#endif
}

expected<AttachCommand, CommandFailure> Session::attach_command() const {
  if (backend() == nullptr) {
    return unexpected(detail::disconnected());
  }
  auto prepared = backend()->prepare_attach(id());
  if (!prepared.has_value()) {
    return unexpected(std::move(prepared.error()));
  }
  auto state = std::make_shared<const AttachCommand::State>(std::move(prepared->argv),
                                                            std::move(prepared->route));
  return AttachCommand{std::move(state)};
}

expected<void, CommandFailure> Session::detach_clients() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely route detach-client"));
#else
  return effect(run({"detach-client", "-s", session_target(*this)}));
#endif
}

expected<std::vector<OptionEntry>, CommandFailure> Session::hooks() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot atomically read session hooks"));
#else
  auto hooks = listed(run({"show-hooks", "-t", session_target(*this)}));
  return hooks;
#endif
}

expected<void, CommandFailure> Session::set_hook(std::string_view name,
                                                 std::string_view command) const {
#if defined(_WIN32)
  static_cast<void>(name);
  static_cast<void>(command);
  return unexpected(unsupported("psmux cannot atomically set a session hook"));
#else
  CommandRequest request{"set-hook", "-t", session_target(*this), std::string{name}};
  request.push_back(CommandArgument::sensitive(std::string{command}));
  return effect(run(request));
#endif
}

// --- Window ----------------------------------------------------------------

std::string Window::target() const {
  auto result = checked_target();
  return result.has_value() ? *std::move(result) : std::string{};
}

expected<std::string, CommandFailure> Window::checked_target() const {
#if defined(_WIN32)
  return unexpected(
      unsupported("psmux cannot bind a reusable target to a captured window safely"));
#else
  // A window read out of a recording may carry no session; a bare id is then
  // the only thing there is to say.
  if (session_id().empty()) {
    return std::string{id()};
  }
  return std::string{session_id()} + ":" + std::string{id()};
#endif
}

namespace {

std::string window_command_target(const Window& window) {
#if defined(_WIN32)
  return std::string{window.id()};
#else
  return window.target();
#endif
}

} // namespace

expected<Session, CommandFailure> Window::session() const {
#if defined(_WIN32)
  auto current = refresh();
  if (!current.has_value()) {
    return unexpected(current.error());
  }
  return detail::describe<Session>(current->backend(), ":", current->session_id(),
                                   session_route(*current));
#else
  return detail::describe<Session>(backend(), window_command_target(*this), {},
                                   session_route(*this));
#endif
}

expected<std::vector<Pane>, CommandFailure> Window::panes() const {
#if defined(_WIN32)
  auto all = detail::list_entities<Pane>(backend(), {"list-panes", "-s", "-t", ":"},
                                         session_route(*this));
  if (!all.has_value()) {
    return unexpected(all.error());
  }
  std::vector<Pane> owned;
  for (Pane& pane : *all) {
    if (pane.window_id() == id()) {
      owned.push_back(std::move(pane));
    }
  }
  if (owned.empty()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::missing,
                       .delivery = DeliveryStatus::replied,
                       .exit_code = 0,
                       .diagnostic = "tmux has no window " + std::string{id()}});
  }
  return owned;
#else
  return detail::list_entities<Pane>(backend(),
                                     {"list-panes", "-t", window_command_target(*this)},
                                     session_route(*this));
#endif
}

expected<Pane, CommandFailure> Window::active_pane() const {
#if defined(_WIN32)
  auto owned = panes();
  if (!owned.has_value()) {
    return unexpected(owned.error());
  }
  for (Pane& pane : *owned) {
    if (pane.active()) {
      return std::move(pane);
    }
  }
  return unexpected(
      CommandFailure{.kind = FailureKind::missing,
                     .delivery = DeliveryStatus::replied,
                     .exit_code = 0,
                     .diagnostic = "tmux has no active pane in " + std::string{id()}});
#else
  return detail::describe<Pane>(backend(), window_command_target(*this), {},
                                session_route(*this));
#endif
}

expected<Pane, CommandFailure> Window::split() const { return split(SplitOptions{}); }

expected<Pane, CommandFailure> Window::split(SplitOptions options) const {
  if (options.percentage.has_value() &&
      (*options.percentage < 1 || *options.percentage > 100)) {
    return unexpected(rejected("a percentage of the window is between 1 and 100"));
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target split-window"));
#else
  CommandRequest command{"split-window", "-t", window_command_target(*this), "-P"};
  if (!options.focus) {
    command.emplace_back("-d");
  }
  command.emplace_back(options.horizontal ? "-h" : "-v");
  if (options.before) {
    command.emplace_back("-b");
  }
  if (options.full_size) {
    command.emplace_back("-f");
  }
  if (options.percentage.has_value()) {
    // `-l 25%` rather than `-p 25`. They mean the same thing and tmux has
    // accepted the former since 3.1, but `-p` was removed in 3.4 — a version
    // in the middle of the supported range, which answers a split carrying it
    // with "size missing". Spelling it the way every supported tmux
    // understands costs nothing.
    command.emplace_back("-l");
    command.push_back(std::to_string(*options.percentage) + "%");
  }
  if (!options.start_directory.empty()) {
    command.emplace_back("-c");
    command.push_back(std::move(options.start_directory));
  }
  if (const auto env = detail::append_environment(command, options.environment);
      !env.has_value()) {
    return unexpected(env.error());
  }
  if (!options.shell_command.empty()) {
    command.emplace_back("--");
    command.push_back(CommandArgument::sensitive(std::move(options.shell_command)));
  }
  return detail::one_entity<Pane>(backend(), std::move(command), FormatArgument::flag,
                                  id(), {}, session_route(*this));
#endif
}

expected<void, CommandFailure> Window::rename(std::string_view name) const {
  if (name.empty()) {
    return unexpected(rejected("window name is empty"));
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target rename-window"));
#else
  std::vector<std::string> command{"rename-window", "-t", window_command_target(*this)};
  command.emplace_back("--");
  command.emplace_back(name);
  return effect(run(command));
#endif
}

expected<void, CommandFailure> Window::select() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely select a window by stable id"));
#else
  return effect(run({"select-window", "-t", window_command_target(*this)}));
#endif
}

// Both carry the session, like every other window operation here.
//
// A bare window id is not wrong so much as under-specified: a window linked
// into several sessions has several homes, and tmux picks one of them to
// supply the session-relative half of the format context. `#{session_name}`
// expanded against a bare id answers about whichever session tmux chose, which
// is the answer to a question nobody asked.
expected<void, CommandFailure> Window::show_message(std::string_view text) const {
#if defined(_WIN32)
  static_cast<void>(text);
  return unexpected(unsupported("psmux cannot safely target a window message"));
#else
  std::vector<std::string> command{"display-message", "-t",
                                   window_command_target(*this)};
  detail::append_display_message_text(command, std::string{text});
  return effect(run(command));
#endif
}

expected<std::string, CommandFailure> Window::expand(std::string_view format) const {
  return detail::expand_format<Window>(backend(), window_command_target(*this), format,
                                       id(), session_route(*this));
}

expected<void, CommandFailure> Window::kill() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux can kill the active window for a stale target"));
#else
  return effect(run({"kill-window", "-t", window_command_target(*this)}));
#endif
}

expected<void, CommandFailure> Window::select_layout(std::string_view layout) const {
  if (layout.empty()) {
    return unexpected(rejected("layout is empty"));
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target select-layout"));
#else
  return effect(
      run({"select-layout", "-t", window_command_target(*this), std::string{layout}}));
#endif
}

expected<void, CommandFailure> Window::resize(long long width, long long height) const {
  if (width <= 0 || height <= 0) {
    return unexpected(rejected("a window cannot be resized to nothing"));
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux treats resize-window as a no-op"));
#else
  return effect(run({"resize-window", "-t", window_command_target(*this), "-x",
                     std::to_string(width), "-y", std::to_string(height)}));
#endif
}

expected<void, CommandFailure> Window::next_layout() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target next-layout"));
#else
  return effect(run({"next-layout", "-t", window_command_target(*this)}));
#endif
}

expected<void, CommandFailure> Window::previous_layout() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target previous-layout"));
#else
  return effect(run({"previous-layout", "-t", window_command_target(*this)}));
#endif
}

expected<void, CommandFailure> Window::rotate() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target rotate-window"));
#else
  return effect(run({"rotate-window", "-t", window_command_target(*this)}));
#endif
}

expected<Pane, CommandFailure> Window::select_last_pane() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target the last pane"));
#else
  const auto moved = run({"select-pane", "-l", "-t", window_command_target(*this)});
  if (!moved.has_value()) {
    return unexpected(moved.error());
  }
  return active_pane();
#endif
}

expected<void, CommandFailure> Window::link_to(const Session& target) const {
  if (from_different_servers(*this, target)) {
    return unexpected(crossed_servers());
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot link a window between sessions"));
#else
  return effect(
      run({"link-window", "-s", this->target(), "-t", session_target(target) + ":"}));
#endif
}

expected<void, CommandFailure> Window::unlink() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux unlink-window destroys the active window"));
#else
  // The qualified target, because a window shown in several sessions has
  // several links and a bare id does not say which one to remove.
  return effect(run({"unlink-window", "-t", window_command_target(*this)}));
#endif
}

expected<void, CommandFailure> Window::swap_with(const Window& other) const {
  if (from_different_servers(*this, other)) {
    return unexpected(crossed_servers());
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target swap-window"));
#else
  return effect(run({"swap-window", "-s", window_command_target(*this), "-t",
                     window_command_target(other)}));
#endif
}

expected<void, CommandFailure> Window::move_to(long long index) const {
  if (index < 0) {
    return unexpected(rejected("a window index cannot be negative"));
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target move-window"));
#else
  // Both ends carry this window's session. A bare index would land in
  // whichever session tmux considers current, and a bare window id is worse:
  // a window linked into several sessions has several homes, and tmux picks
  // one of them, so the move lands in the wrong place and unlinks the window
  // from a session the caller never mentioned.
  //
  // `-d` because move-window otherwise selects what it moved, taking the
  // user's focus somewhere they did not ask to go.
  const std::string source = window_command_target(*this);
  const std::string owner{session_id()};
  const std::string target = owner + ":" + std::to_string(index);
  return effect(run({"move-window", "-d", "-s", source, "-t", target}));
#endif
}

expected<Window, CommandFailure> Window::refresh() const {
  return detail::describe<Window>(backend(), window_command_target(*this), id(),
                                  session_route(*this));
}

expected<std::vector<OptionEntry>, CommandFailure> Window::options() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux does not provide window-scoped options"));
#else
  return listed(
      run(scoped("show-options", "-w", window_command_target(*this), {"-A"})));
#endif
}

expected<OptionEntry, CommandFailure> Window::option(std::string_view name) const {
#if defined(_WIN32)
  static_cast<void>(name);
  return unexpected(unsupported("psmux does not provide window-scoped options"));
#else
  return named(
      run(scoped("show-options", "-w", window_command_target(*this), {"-A", name})),
      name);
#endif
}

expected<void, CommandFailure> Window::set_option(std::string_view name,
                                                  std::string_view value) const {
#if defined(_WIN32)
  static_cast<void>(name);
  static_cast<void>(value);
  return unexpected(unsupported("psmux does not provide window-scoped options"));
#else
  CommandRequest command =
      scoped("set-option", "-w", window_command_target(*this), {name});
  command.push_back(CommandArgument::sensitive(std::string{value}));
  return effect(run(command));
#endif
}

expected<void, CommandFailure> Window::unset_option(std::string_view name) const {
#if defined(_WIN32)
  static_cast<void>(name);
  return unexpected(unsupported("psmux does not provide window-scoped options"));
#else
  return effect(
      run(scoped("set-option", "-w", window_command_target(*this), {"-u", name})));
#endif
}

// --- Pane ------------------------------------------------------------------

namespace {

std::string pane_target(const Pane& pane) {
#if defined(_WIN32)
  static_cast<void>(pane);
#endif
  return std::string{pane.id()};
}

#if !defined(_WIN32)
ExecutionPolicy
remaining_execution_policy(const ExecutionPolicy& policy,
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

bool is_canonical_tmux_id(std::string_view id, char prefix) {
  if (id.size() < 2U || id.front() != prefix || (id.size() > 2U && id[1] == '0')) {
    return false;
  }
  std::uint32_t value = 0U;
  for (const char byte : id.substr(1)) {
    if (byte < '0' || byte > '9') {
      return false;
    }
    const auto digit = static_cast<std::uint32_t>(byte - '0');
    if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  return true;
}

expected<std::string, CommandFailure> tmux_command_word(std::string_view value) {
  if (value.find('\0') != std::string_view::npos) {
    return unexpected(rejected("a window name cannot contain NUL"));
  }

  // if-shell reparses its branch as tmux syntax. Fixed-width octal keeps every
  // caller byte inside one word without relying on shell quoting.
  std::string quoted;
  quoted.reserve(value.size() * 4U + 2U);
  quoted.push_back('"');
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    quoted.push_back('\\');
    quoted.push_back(static_cast<char>('0' + ((byte >> 6U) & 7U)));
    quoted.push_back(static_cast<char>('0' + ((byte >> 3U) & 7U)));
    quoted.push_back(static_cast<char>('0' + (byte & 7U)));
  }
  quoted.push_back('"');
  return quoted;
}

constexpr auto kNamedBreakFields = [] {
  std::array<std::string_view, Window::kFields.size() + 2U> fields{};
  for (std::size_t index = 0U; index < Window::kFields.size(); ++index) {
    fields[index] = Window::kFields[index];
  }
  fields[Window::kFields.size()] = "version";
  fields[Window::kFields.size() + 1U] = "automatic-rename";
  return fields;
}();

struct NamedBreakReport {
  Window window;
  Version version;
  bool automatic_rename;
};

expected<NamedBreakReport, CommandFailure>
named_break_report(const std::shared_ptr<const detail::Backend>& backend,
                   std::string output, std::string_view source,
                   std::string_view expected_window = {},
                   std::string_view expected_session = {}) {
  auto snapshot = detail::SnapshotFactory::from_output(backend, kNamedBreakFields,
                                                       std::move(output));
  if (!snapshot.has_value()) {
    CommandFailure failure = snapshot.error();
    failure.diagnostic =
        "break-pane completed for pane " + std::string{source} +
        ", but its window report was malformed: " + std::move(failure.diagnostic);
    return unexpected(std::move(failure));
  }
  const auto& rows = (*snapshot)->rows();
  if (rows.empty() || rows.front().front().empty()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::missing,
                       .delivery = DeliveryStatus::replied,
                       .exit_code = 0,
                       .diagnostic = "tmux has no window " + std::string{source}});
  }
  if (rows.size() != 1U) {
    return unexpected(CommandFailure{
        .kind = FailureKind::refused,
        .delivery = DeliveryStatus::replied,
        .exit_code = 0,
        .diagnostic = "break-pane completed for pane " + std::string{source} +
                      ", but did not report one exact connected window row"});
  }

  Window window{*snapshot, 0U};
  if (!is_canonical_tmux_id(window.id(), '@') ||
      !is_canonical_tmux_id(window.session_id(), '$') ||
      (!expected_window.empty() && window.id() != expected_window) ||
      (!expected_session.empty() && window.session_id() != expected_session)) {
    return unexpected(CommandFailure{
        .kind = FailureKind::refused,
        .delivery = DeliveryStatus::replied,
        .exit_code = 0,
        .diagnostic = "break-pane completed for pane " + std::string{source} +
                      ", but did not report the exact connected window"});
  }

  const std::string_view reported_version = rows.front()[Window::kFields.size()];
  const auto version = parse_version("tmux " + std::string{reported_version});
  const std::string_view automatic_rename = rows.front()[Window::kFields.size() + 1U];
  if (!version.has_value() || (automatic_rename != "0" && automatic_rename != "1")) {
    return unexpected(CommandFailure{
        .kind = FailureKind::refused,
        .delivery = DeliveryStatus::replied,
        .exit_code = 0,
        .diagnostic = "break-pane completed for pane " + std::string{source} +
                      ", but its version or automatic-rename report was invalid"});
  }
  return NamedBreakReport{.window = std::move(window),
                          .version = *version,
                          .automatic_rename = automatic_rename == "1"};
}

std::string named_repair_guard(std::string_view window_id,
                               std::string_view session_id) {
  const auto differs = [](std::string_view field, std::string_view value) {
    return "#{!=:#{" + std::string{field} + "}," + std::string{value} + "}";
  };
  std::array terms{
      differs("version", "3.7"),         differs("window_id", window_id),
      differs("session_id", session_id), differs("after-rename-window", ""),
      differs("window-renamed", ""),     differs("after-display-message", ""),
  };
  std::string condition = std::move(terms.back());
  for (std::size_t index = terms.size() - 1U; index > 0U; --index) {
    condition = "#{||:" + terms[index - 1U] + ',' + std::move(condition) + '}';
  }
  return condition;
}
#endif

} // namespace

expected<Window, CommandFailure> Pane::window() const {
#if defined(_WIN32)
  auto current_window = expand("#{window_id}");
  if (!current_window.has_value()) {
    return unexpected(current_window.error());
  }
  auto all = detail::list_entities<Window>(backend(), {"list-windows", "-t", ":"},
                                           session_route(*this));
  if (!all.has_value()) {
    return unexpected(all.error());
  }
  for (Window& window : *all) {
    if (window.id() == *current_window) {
      return std::move(window);
    }
  }
  return unexpected(
      CommandFailure{.kind = FailureKind::missing,
                     .delivery = DeliveryStatus::replied,
                     .exit_code = 0,
                     .diagnostic = "tmux has no window " + *std::move(current_window)});
#else
  return detail::describe<Window>(backend(), pane_target(*this), {},
                                  session_route(*this));
#endif
}

expected<Session, CommandFailure> Pane::session() const {
#if defined(_WIN32)
  auto current = refresh();
  if (!current.has_value()) {
    return unexpected(current.error());
  }
  return detail::describe<Session>(current->backend(), ":", current->session_id(),
                                   session_route(*current));
#else
  return detail::describe<Session>(backend(), pane_target(*this), {},
                                   session_route(*this));
#endif
}

expected<void, CommandFailure> Pane::send_text(std::string_view text) const {
  const auto arguments = literal_arguments(text);
  if (!arguments.has_value()) {
    return unexpected(rejected("text to send is empty"));
  }
#if defined(_WIN32)
  return unexpected(
      unsupported("psmux can send text to the active pane for a stale target"));
#else
  std::vector<std::string> command{"send-keys", "-t", pane_target(*this)};
  command.insert(command.end(), arguments->begin(), arguments->end());
  return effect(run(command));
#endif
}

expected<void, CommandFailure> Pane::send_key(std::string_view key) const {
  // tmux accepts an unknown key name silently, so a typo would arrive as
  // input that never happened. Reject it here, where the caller can be told.
  if (!is_key_name(key)) {
    return unexpected(rejected("unknown key name: " + std::string{key}));
  }
#if defined(_WIN32)
  return unexpected(
      unsupported("psmux can send a key to the active pane for a stale target"));
#else
  return effect(run({"send-keys", "-t", pane_target(*this), std::string{key}}));
#endif
}

expected<std::string, CommandFailure> Pane::capture() const {
  return capture(CaptureOptions{});
}

expected<std::string, CommandFailure> Pane::capture(CaptureOptions options) const {
#if defined(_WIN32)
  static_cast<void>(options);
  return unexpected(
      unsupported("psmux can capture the active pane for a stale target"));
#else
  std::vector<std::string> command{"capture-pane", "-p", "-t", pane_target(*this)};
  if (options.whole_history) {
    // tmux spells the top of the scrollback as a bare dash.
    command.emplace_back("-S");
    command.emplace_back("-");
  } else if (options.start_line.has_value()) {
    command.emplace_back("-S");
    command.push_back(std::to_string(*options.start_line));
  }
  if (options.end_line.has_value()) {
    command.emplace_back("-E");
    command.push_back(std::to_string(*options.end_line));
  }
  if (options.join_wrapped) {
    command.emplace_back("-J");
  }
  if (options.with_escape_sequences) {
    command.emplace_back("-e");
  }
  if (options.keep_trailing_spaces) {
    command.emplace_back("-N");
  }
  return run(command, options.output_limit);
#endif
}

expected<void, CommandFailure> Pane::set_width(long long width) const {
  if (width <= 0) {
    return unexpected(rejected("a pane cannot be resized to nothing"));
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target resize-pane"));
#else
  return effect(
      run({"resize-pane", "-t", pane_target(*this), "-x", std::to_string(width)}));
#endif
}

expected<void, CommandFailure> Pane::set_height(long long height) const {
  if (height <= 0) {
    return unexpected(rejected("a pane cannot be resized to nothing"));
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target resize-pane"));
#else
  return effect(
      run({"resize-pane", "-t", pane_target(*this), "-y", std::to_string(height)}));
#endif
}

expected<void, CommandFailure> Pane::swap_with(const Pane& other) const {
  if (from_different_servers(*this, other)) {
    return unexpected(crossed_servers());
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target swap-pane"));
#else
  return effect(
      run({"swap-pane", "-d", "-s", pane_target(*this), "-t", pane_target(other)}));
#endif
}

expected<Window, CommandFailure> Pane::break_out(std::string_view name) const {
  if (backend() == nullptr) {
    return unexpected(detail::disconnected());
  }
#if defined(_WIN32)
  static_cast<void>(name);
  return unexpected(
      unsupported("psmux cannot report the window created by break-pane"));
#else
  const auto started = std::chrono::steady_clock::now();
  const ExecutionPolicy policy = backend()->policy();
  if (name.empty()) {
    const std::string target = pane_target(*this);
    const std::string owner{session_id()};
    const std::string home{window_id()};
    if (!is_canonical_tmux_id(target, '%')) {
      return unexpected(rejected("break-pane requires a stable numeric pane id"));
    }
    if (!is_canonical_tmux_id(owner, '$')) {
      return unexpected(rejected("break-pane requires a stable numeric session id"));
    }
    if (!is_canonical_tmux_id(home, '@')) {
      return unexpected(rejected("break-pane requires a stable numeric window id"));
    }

    // A one-pane window is already broken out. Report it in place rather than
    // letting tmux choose an unrelated destination session. Raw tmux 3.7 also
    // needs -n for a multi-pane break; both decisions stay server-side.
    const std::string native = "break-pane -d -s " + target + " -t " + owner +
                               ": -P -F '" + format_request(Window::kFields) + "'";
    const std::string current = "display-message -p -t " + owner + ":" + home + " '" +
                                format_request(Window::kFields) + "'";
    const std::string guarded = "if-shell -F -t " + target +
                                " '#{==:#{version},3.7}' { " + native +
                                " -n libtmux } { " + native + " }";
    const std::vector<std::string> command{
        "if-shell", "-F", "-t", target, "#{==:#{window_panes},1}", current, guarded};
    const auto break_policy = remaining_execution_policy(policy, started);
    auto moved =
        backend()->run(command, break_policy.timeout, break_policy.output_limit);
    if (!moved.has_value()) {
      return unexpected(moved.error());
    }

    auto snapshot = detail::SnapshotFactory::from_output(backend(), Window::kFields,
                                                         *std::move(moved));
    if (!snapshot.has_value()) {
      CommandFailure failure = snapshot.error();
      failure.diagnostic =
          "break-pane completed for pane " + target +
          ", but its window row was malformed: " + std::move(failure.diagnostic);
      return unexpected(std::move(failure));
    }
    const auto& rows = (*snapshot)->rows();
    if (rows.empty() || rows.front().front().empty()) {
      return unexpected(CommandFailure{.kind = FailureKind::missing,
                                       .delivery = DeliveryStatus::replied,
                                       .exit_code = 0,
                                       .diagnostic = "tmux has no window " + target});
    }
    if (rows.size() != 1U) {
      return unexpected(CommandFailure{
          .kind = FailureKind::refused,
          .delivery = DeliveryStatus::replied,
          .exit_code = 0,
          .diagnostic = "break-pane completed for pane " + target +
                        ", but did not report one exact connected window row"});
    }
    Window created{*snapshot, 0U};
    if (!is_canonical_tmux_id(created.id(), '@') ||
        !is_canonical_tmux_id(created.session_id(), '$') ||
        created.session_id() != owner) {
      return unexpected(CommandFailure{
          .kind = FailureKind::refused,
          .delivery = DeliveryStatus::replied,
          .exit_code = 0,
          .diagnostic = "break-pane completed for pane " + target +
                        ", but did not report one exact connected window row"});
    }
    return created;
  }

  const std::string target = pane_target(*this);
  const std::string owner{session_id()};
  const std::string home{window_id()};
  if (!is_canonical_tmux_id(target, '%')) {
    return unexpected(rejected("break-pane requires a stable numeric pane id"));
  }
  if (!is_canonical_tmux_id(owner, '$')) {
    return unexpected(rejected("break-pane requires a stable numeric session id"));
  }
  if (!is_canonical_tmux_id(home, '@')) {
    return unexpected(rejected("break-pane requires a stable numeric window id"));
  }
  auto break_name = tmux_command_word(name);
  if (!break_name.has_value()) {
    return unexpected(break_name.error());
  }
  // rename-window expands formats in its name; break-pane -n does not.
  auto rename_name = tmux_command_word(escape_literal(name));
  if (!rename_name.has_value()) {
    return unexpected(rename_name.error());
  }

  const std::string fields = format_request(kNamedBreakFields);
  const std::string source_window = owner + ":" + home;
  const std::string report_command =
      "display-message -p -t " + source_window + " '" + fields + "'";
  const std::string retained = "rename-window -t " + source_window + " -- " +
                               *rename_name + " ; " + report_command;
  const std::string native = "break-pane -d -s " + target + " -t " + owner +
                             ": -P -n " + *break_name + " -F '" + fields + "'";
  const std::vector<std::string> command{
      "if-shell", "-F", "-t", target, "#{==:#{window_panes},1}", retained, native};
  const auto break_policy = remaining_execution_policy(policy, started);
  auto broken =
      backend()->run(command, break_policy.timeout, break_policy.output_limit);
  if (!broken.has_value()) {
    return unexpected(broken.error());
  }
  auto created = named_break_report(backend(), *std::move(broken), id(), {}, owner);
  if (!created.has_value()) {
    return unexpected(created.error());
  }

  const bool raw_tmux_37 = created->version == Version{.major = 3, .minor = 7};
  const bool retained_window = created->window.id() == window_id();
  if (!raw_tmux_37 || (retained_window && !created->automatic_rename)) {
    return std::move(created->window);
  }

  const std::string created_window{created->window.id()};
  const std::string created_session{created->window.session_id()};
  const std::string repair_target = window_command_target(created->window);
  CommandBatch repair;
  static_cast<void>(
      repair.add({"if-shell", "-F", "-t", repair_target,
                  named_repair_guard(created_window, created_session), "{"}));
  static_cast<void>(
      repair.add({"rename-window", "-t", repair_target, "--", escape_literal(name)}));
  std::vector<std::string> final_report{"display-message", "-p", "-t", repair_target};
  detail::append_display_message_text(final_report, format_request(kNamedBreakFields));
  static_cast<void>(repair.add(std::move(final_report)));

  const auto repair_policy = remaining_execution_policy(policy, started);
  auto repaired =
      backend()->run_batch(repair, repair_policy.timeout, repair_policy.output_limit);
  if (!repaired.has_value()) {
    CommandFailure failure = repaired.error();
    if (failure.delivery == DeliveryStatus::not_started) {
      // The repair did not start, but break-pane itself already replied and
      // moved the pane. Retrying the whole operation is therefore unsafe.
      failure.delivery = DeliveryStatus::replied;
    }
    failure.diagnostic =
        "break-pane moved pane " + std::string{id()} + " into window " +
        created_window +
        ", but its raw tmux 3.7 name repair failed: " + std::move(failure.diagnostic);
    return unexpected(std::move(failure));
  }

  auto final = named_break_report(backend(), *std::move(repaired), id(), created_window,
                                  created_session);
  if (!final.has_value()) {
    return unexpected(final.error());
  }
  if (final->version != Version{.major = 3, .minor = 7} || final->automatic_rename) {
    return unexpected(CommandFailure{
        .kind = FailureKind::refused,
        .delivery = DeliveryStatus::replied,
        .exit_code = 0,
        .diagnostic = "break-pane moved pane " + std::string{id()} + " into window " +
                      created_window +
                      ", but its raw tmux 3.7 name repair was not durable"});
  }
  return std::move(final->window);
#endif
}

expected<void, CommandFailure> Pane::join(const Window& target) const {
  if (from_different_servers(*this, target)) {
    return unexpected(crossed_servers());
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target join-pane"));
#else
  return effect(run(
      {"join-pane", "-s", pane_target(*this), "-t", window_command_target(target)}));
#endif
}

expected<void, CommandFailure> Pane::enter_copy_mode() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target copy-mode"));
#else
  return effect(run({"copy-mode", "-t", pane_target(*this)}));
#endif
}

expected<void, CommandFailure> Pane::leave_mode() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely leave mode on a targeted pane"));
#else
  // `-X cancel` is how a mode is left: there is no leave-mode command, and
  // the key that would do it depends on the caller's bindings.
  return effect(run({"send-keys", "-t", pane_target(*this), "-X", "cancel"}));
#endif
}

expected<void, CommandFailure> Pane::pipe_to(std::string_view command) const {
  if (command.empty()) {
    // An empty command is how tmux spells "stop", and a caller who reached
    // for pipe_to did not mean to stop. Say so rather than doing the
    // opposite of what was asked.
    return unexpected(rejected("a pipe needs a command; use stop_piping"));
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target pipe-pane"));
#else
  CommandRequest argv{"pipe-pane", "-t", pane_target(*this)};
  argv.emplace_back("--");
  argv.push_back(CommandArgument::sensitive(std::string{command}));
  return effect(run(argv));
#endif
}

expected<void, CommandFailure> Pane::stop_piping() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target pipe-pane"));
#else
  return effect(run({"pipe-pane", "-t", pane_target(*this)}));
#endif
}

expected<void, CommandFailure> Pane::set_title(std::string_view title) const {
#if defined(_WIN32)
  static_cast<void>(title);
  return unexpected(unsupported("psmux cannot safely target a pane title"));
#else
  // No `--` here, though a title beginning with a dash is data: `-T`
  // consumes the next argument whatever it looks like, and adding the guard
  // makes tmux read the separator as the value and the title as a target —
  // "can't find pane: itled". The guard belongs on positional arguments,
  // like the text of set-buffer, not on the value of a flag.
  return effect(
      run({"select-pane", "-t", pane_target(*this), "-T", std::string{title}}));
#endif
}

expected<void, CommandFailure> Pane::respawn(bool replace_running) const {
#if defined(_WIN32)
  static_cast<void>(replace_running);
  return unexpected(unsupported("psmux respawn-pane can terminate the session server"));
#else
  std::vector<std::string> command{"respawn-pane", "-t", pane_target(*this)};
  if (replace_running) {
    command.emplace_back("-k");
  }
  return effect(run(command));
#endif
}

expected<void, CommandFailure> Pane::clear_history() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot safely target clear-history"));
#else
  return effect(run({"clear-history", "-t", pane_target(*this)}));
#endif
}

expected<void, CommandFailure> Pane::select() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot atomically select a captured pane"));
#else
  return effect(run({"select-pane", "-t", pane_target(*this)}));
#endif
}

expected<void, CommandFailure> Pane::show_message(std::string_view text) const {
#if defined(_WIN32)
  static_cast<void>(text);
  return unexpected(unsupported("psmux cannot safely target a pane message"));
#else
  std::vector<std::string> command{"display-message", "-t", pane_target(*this)};
  detail::append_display_message_text(command, std::string{text});
  return effect(run(command));
#endif
}

expected<std::string, CommandFailure> Pane::expand(std::string_view format) const {
  return detail::expand_format<Pane>(backend(), pane_target(*this), format, id(),
                                     session_route(*this));
}

expected<void, CommandFailure> Pane::kill() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux cannot atomically kill a captured pane"));
#else
  return effect(run({"kill-pane", "-t", pane_target(*this)}));
#endif
}

expected<Pane, CommandFailure> Pane::refresh() const {
  return detail::describe<Pane>(backend(), pane_target(*this), id(),
                                session_route(*this));
}

expected<std::vector<OptionEntry>, CommandFailure> Pane::options() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux does not implement pane options"));
#else
  return listed(run(scoped("show-options", "-p", pane_target(*this), {"-A"})));
#endif
}

expected<OptionEntry, CommandFailure> Pane::option(std::string_view name) const {
#if defined(_WIN32)
  static_cast<void>(name);
  return unexpected(unsupported("psmux does not implement pane options"));
#else
  return named(run(scoped("show-options", "-p", pane_target(*this), {"-A", name})),
               name);
#endif
}

expected<void, CommandFailure> Pane::set_option(std::string_view name,
                                                std::string_view value) const {
#if defined(_WIN32)
  static_cast<void>(name);
  static_cast<void>(value);
  return unexpected(unsupported("psmux does not implement pane options"));
#else
  CommandRequest command = scoped("set-option", "-p", pane_target(*this), {name});
  command.push_back(CommandArgument::sensitive(std::string{value}));
  return effect(run(command));
#endif
}

expected<void, CommandFailure> Pane::unset_option(std::string_view name) const {
#if defined(_WIN32)
  static_cast<void>(name);
  return unexpected(unsupported("psmux does not implement pane options"));
#else
  return effect(run(scoped("set-option", "-p", pane_target(*this), {"-u", name})));
#endif
}

// --- Client ----------------------------------------------------------------

expected<void, CommandFailure> Pane::paste(const Buffer& buffer, bool consume) const {
  if (from_different_servers(*this, buffer)) {
    return unexpected(crossed_servers());
  }
#if defined(_WIN32)
  static_cast<void>(consume);
  return unexpected(unsupported("psmux buffers are separate in each session"));
#else
  std::vector<std::string> command{"paste-buffer", "-t", pane_target(*this), "-b",
                                   std::string{buffer.name()}};
  if (consume) {
    command.emplace_back("-d");
  }
  return effect(run(command));
#endif
}

expected<std::string, CommandFailure> Buffer::contents() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux buffers are separate in each session"));
#else
  return run({"show-buffer", "-b", std::string{name()}});
#endif
}

expected<void, CommandFailure> Buffer::remove() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux buffers are separate in each session"));
#else
  return effect(run({"delete-buffer", "-b", std::string{name()}}));
#endif
}

expected<Session, CommandFailure> Client::session() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux client handles are not session-routed"));
#else
  // A client is targeted by name with -c, and the session it is looking at is
  // the one a format query in its context reports.
  return detail::one_entity<Session>(
      backend(), {"display-message", "-p", "-c", std::string{name()}},
      FormatArgument::message, name());
#endif
}

expected<void, CommandFailure> Client::switch_to(const Session& session) const {
  if (from_different_servers(*this, session)) {
    return unexpected(crossed_servers());
  }
#if defined(_WIN32)
  return unexpected(unsupported("psmux client handles are not session-routed"));
#else
  return effect(
      run({"switch-client", "-c", std::string{name()}, "-t", session_target(session)}));
#endif
}

expected<void, CommandFailure> Client::detach() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux client handles are not session-routed"));
#else
  return effect(run({"detach-client", "-t", std::string{name()}}));
#endif
}

expected<void, CommandFailure> Client::refresh() const {
#if defined(_WIN32)
  return unexpected(unsupported("psmux client handles are not session-routed"));
#else
  return effect(run({"refresh-client", "-t", std::string{name()}}));
#endif
}

// --- Printing and hashing --------------------------------------------------

std::ostream& operator<<(std::ostream& stream, const Session& session) {
  return stream << "Session(" << session.id() << ' ' << session.name() << ')';
}

std::ostream& operator<<(std::ostream& stream, const Window& window) {
  return stream << "Window(" << window.id() << ' ' << window.index() << ':'
                << window.name() << ')';
}

std::ostream& operator<<(std::ostream& stream, const Pane& pane) {
  return stream << "Pane(" << pane.id() << ' ' << pane.command() << ')';
}

std::ostream& operator<<(std::ostream& stream, const Client& client) {
  return stream << "Client(" << client.name() << ' ' << client.session_name() << ')';
}

LIBTMUX_NAMESPACE_END

namespace {

std::size_t hash_mix(std::size_t left, std::size_t right) noexcept {
  return left ^ (right + 0x9e3779b9U + (left << 6U) + (left >> 2U));
}

// Hash the identity fields equality compares, in their comparison order.
template <typename Entity>
std::size_t hash_identity(std::string_view connection,
                          std::string_view identity) noexcept {
  const std::size_t left = std::hash<std::string_view>{}(connection);
  const std::size_t right = std::hash<std::string_view>{}(identity);
  // The mix libstdc++ and libc++ both use for pairs.
  return hash_mix(left, right);
}

} // namespace

std::size_t
std::hash<libtmux::Session>::operator()(const libtmux::Session& value) const noexcept {
  return hash_identity<libtmux::Session>(value.connection_identity(), value.id());
}

std::size_t
std::hash<libtmux::Window>::operator()(const libtmux::Window& value) const noexcept {
#if defined(_WIN32)
  return hash_mix(
      hash_identity<libtmux::Window>(value.connection_identity(), value.session_id()),
      std::hash<std::string_view>{}(value.id()));
#else
  return hash_identity<libtmux::Window>(value.connection_identity(), value.id());
#endif
}

std::size_t
std::hash<libtmux::Pane>::operator()(const libtmux::Pane& value) const noexcept {
#if defined(_WIN32)
  return hash_mix(
      hash_identity<libtmux::Pane>(value.connection_identity(), value.session_id()),
      std::hash<std::string_view>{}(value.id()));
#else
  return hash_identity<libtmux::Pane>(value.connection_identity(), value.id());
#endif
}

std::size_t
std::hash<libtmux::Client>::operator()(const libtmux::Client& value) const noexcept {
  return hash_identity<libtmux::Client>(value.connection_identity(), value.name());
}
