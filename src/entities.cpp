#include "libtmux/entities.hpp"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <ostream>

#include "libtmux/keys.hpp"
#include "libtmux/options.hpp"
#include "libtmux/server.hpp"

#include "acquire.hpp"

LIBTMUX_NAMESPACE_BEGIN

expected<std::string, CommandFailure>
detail::Row::run(const std::vector<std::string>& command,
                 std::optional<std::size_t> output_limit) const {
  if (backend() == nullptr) {
    return unexpected(detail::disconnected());
  }
  return backend()->run(command, std::nullopt, output_limit);
}

expected<Server, CommandFailure> detail::Row::server() const {
  if (backend() == nullptr) {
    return unexpected(detail::disconnected());
  }
  return detail::server_over(backend());
}

namespace {

// A command run for its effect. The output of `kill-window` is not a result.
expected<void, CommandFailure> effect(expected<std::string, CommandFailure> reply) {
  if (!reply.has_value()) {
    return unexpected(reply.error());
  }
  return {};
}

CommandFailure rejected(std::string diagnostic) {
  return CommandFailure{.kind = FailureKind::validation,
                        .dispatched = false,
                        .exit_code = 0,
                        .diagnostic = std::move(diagnostic)};
}

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
                       .dispatched = true,
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

} // namespace

// --- Session ---------------------------------------------------------------

expected<std::vector<Window>, CommandFailure> Session::windows() const {
  return detail::list_entities<Window>(backend(),
                                       {"list-windows", "-t", std::string{id()}});
}

expected<std::vector<Pane>, CommandFailure> Session::panes() const {
  return detail::list_entities<Pane>(backend(),
                                     {"list-panes", "-s", "-t", std::string{id()}});
}

expected<Window, CommandFailure> Session::active_window() const {
  return detail::describe<Window>(backend(), id());
}

expected<Pane, CommandFailure> Session::active_pane() const {
  return detail::describe<Pane>(backend(), id());
}

namespace {

// Each of these selects, then reports where the selection ended up. The
// second call is what makes the result useful; tmux's own commands print
// nothing.
expected<Window, CommandFailure> moved(const Session& session,
                                       expected<std::string, CommandFailure> ran) {
  if (!ran.has_value()) {
    return unexpected(ran.error());
  }
  return session.active_window();
}

} // namespace

expected<Window, CommandFailure> Session::select_next_window() const {
  return moved(*this, run({"next-window", "-t", std::string{id()}}));
}

expected<Window, CommandFailure> Session::select_previous_window() const {
  return moved(*this, run({"previous-window", "-t", std::string{id()}}));
}

expected<Window, CommandFailure> Session::select_last_window() const {
  return moved(*this, run({"last-window", "-t", std::string{id()}}));
}

expected<Window, CommandFailure> Session::new_window(std::string_view name) const {
  return new_window(NewWindowOptions{.name = std::string{name}});
}

expected<Window, CommandFailure> Session::new_window(NewWindowOptions options) const {
  if (options.name.empty()) {
    return unexpected(rejected("window name is empty"));
  }
  std::string target{id()};
  if (options.index.has_value()) {
    target += ":" + std::to_string(*options.index);
  }
  std::vector<std::string> command{"new-window", "-t", std::move(target),
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
    command.push_back(std::move(options.shell_command));
  }
  return detail::one_entity<Window>(backend(), std::move(command), FormatArgument::flag,
                                    options.name);
}

expected<void, CommandFailure> Session::rename(std::string_view name) const {
  if (name.empty()) {
    return unexpected(rejected("session name is empty"));
  }
  // `--` because a name is data: without it tmux reads a leading dash as a
  // flag and refuses the rename.
  return effect(
      run({"rename-session", "-t", std::string{id()}, "--", std::string{name}}));
}

expected<void, CommandFailure> Session::kill() const {
  return effect(run({"kill-session", "-t", std::string{id()}}));
}

expected<Session, CommandFailure> Session::refresh() const {
  return detail::describe<Session>(backend(), id(), id());
}

expected<std::vector<OptionEntry>, CommandFailure> Session::options() const {
  return listed(run(scoped("show-options", {}, id(), {"-A"})));
}

expected<OptionEntry, CommandFailure> Session::option(std::string_view name) const {
  return named(run(scoped("show-options", {}, id(), {"-A", name})), name);
}

expected<void, CommandFailure> Session::set_option(std::string_view name,
                                                   std::string_view value) const {
  return effect(run(scoped("set-option", {}, id(), {name, value})));
}

expected<void, CommandFailure> Session::unset_option(std::string_view name) const {
  return effect(run(scoped("set-option", {}, id(), {"-u", name})));
}

expected<std::string, CommandFailure> Session::expand(std::string_view format) const {
  return detail::expand_format<Session>(backend(), id(), format);
}

expected<void, CommandFailure> Session::show_message(std::string_view text) const {
  return effect(
      run({"display-message", "-t", std::string{id()}, "--", std::string{text}}));
}

std::vector<std::string> Session::attach_command() const {
  std::vector<std::string> command{"tmux"};
  if (backend() != nullptr) {
    const std::vector<std::string>& connection = backend()->connection();
    command.insert(command.end(), connection.begin(), connection.end());
  }
  command.emplace_back("attach-session");
  command.emplace_back("-t");
  command.emplace_back(id());
  return command;
}

expected<void, CommandFailure> Session::detach_clients() const {
  return effect(run({"detach-client", "-s", std::string{id()}}));
}

expected<std::vector<OptionEntry>, CommandFailure> Session::hooks() const {
  return listed(run({"show-hooks", "-t", std::string{id()}}));
}

expected<void, CommandFailure> Session::set_hook(std::string_view name,
                                                 std::string_view command) const {
  return effect(run(
      {"set-hook", "-t", std::string{id()}, std::string{name}, std::string{command}}));
}

// --- Window ----------------------------------------------------------------

std::string Window::target() const {
  // A window read out of a recording may carry no session; a bare id is then
  // the only thing there is to say.
  if (session_id().empty()) {
    return std::string{id()};
  }
  return std::string{session_id()} + ":" + std::string{id()};
}

expected<Session, CommandFailure> Window::session() const {
  return detail::describe<Session>(backend(), target());
}

expected<std::vector<Pane>, CommandFailure> Window::panes() const {
  return detail::list_entities<Pane>(backend(), {"list-panes", "-t", target()});
}

expected<Pane, CommandFailure> Window::active_pane() const {
  return detail::describe<Pane>(backend(), target());
}

expected<Pane, CommandFailure> Window::split() const { return split(SplitOptions{}); }

expected<Pane, CommandFailure> Window::split(SplitOptions options) const {
  if (options.percentage.has_value() &&
      (*options.percentage < 1 || *options.percentage > 100)) {
    return unexpected(rejected("a percentage of the window is between 1 and 100"));
  }
  std::vector<std::string> command{"split-window", "-t", target(), "-P"};
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
    command.push_back(std::move(options.shell_command));
  }
  return detail::one_entity<Pane>(backend(), std::move(command), FormatArgument::flag,
                                  id());
}

expected<void, CommandFailure> Window::rename(std::string_view name) const {
  if (name.empty()) {
    return unexpected(rejected("window name is empty"));
  }
  return effect(run({"rename-window", "-t", target(), "--", std::string{name}}));
}

expected<void, CommandFailure> Window::select() const {
  return effect(run({"select-window", "-t", target()}));
}

// Both carry the session, like every other window operation here.
//
// A bare window id is not wrong so much as under-specified: a window linked
// into several sessions has several homes, and tmux picks one of them to
// supply the session-relative half of the format context. `#{session_name}`
// expanded against a bare id answers about whichever session tmux chose, which
// is the answer to a question nobody asked.
expected<void, CommandFailure> Window::show_message(std::string_view text) const {
  return effect(run({"display-message", "-t", target(), "--", std::string{text}}));
}

expected<std::string, CommandFailure> Window::expand(std::string_view format) const {
  return detail::expand_format<Window>(backend(), target(), format, id());
}

expected<void, CommandFailure> Window::kill() const {
  return effect(run({"kill-window", "-t", target()}));
}

expected<void, CommandFailure> Window::select_layout(std::string_view layout) const {
  if (layout.empty()) {
    return unexpected(rejected("layout is empty"));
  }
  return effect(run({"select-layout", "-t", target(), std::string{layout}}));
}

expected<void, CommandFailure> Window::resize(long long width, long long height) const {
  if (width <= 0 || height <= 0) {
    return unexpected(rejected("a window cannot be resized to nothing"));
  }
  return effect(run({"resize-window", "-t", target(), "-x", std::to_string(width), "-y",
                     std::to_string(height)}));
}

expected<void, CommandFailure> Window::next_layout() const {
  return effect(run({"next-layout", "-t", target()}));
}

expected<void, CommandFailure> Window::previous_layout() const {
  return effect(run({"previous-layout", "-t", target()}));
}

expected<void, CommandFailure> Window::rotate() const {
  return effect(run({"rotate-window", "-t", target()}));
}

expected<Pane, CommandFailure> Window::select_last_pane() const {
  const auto moved = run({"select-pane", "-l", "-t", target()});
  if (!moved.has_value()) {
    return unexpected(moved.error());
  }
  return active_pane();
}

expected<void, CommandFailure> Window::link_to(const Session& target) const {
  return effect(run(
      {"link-window", "-s", std::string{id()}, "-t", std::string{target.id()} + ":"}));
}

expected<void, CommandFailure> Window::unlink() const {
  // The qualified target, because a window shown in several sessions has
  // several links and a bare id does not say which one to remove.
  return effect(run({"unlink-window", "-t", target()}));
}

expected<void, CommandFailure> Window::swap_with(const Window& other) const {
  return effect(run({"swap-window", "-s", target(), "-t", other.target()}));
}

expected<void, CommandFailure> Window::move_to(long long index) const {
  if (index < 0) {
    return unexpected(rejected("a window index cannot be negative"));
  }
  // Both ends carry this window's session. A bare index would land in
  // whichever session tmux considers current, and a bare window id is worse:
  // a window linked into several sessions has several homes, and tmux picks
  // one of them, so the move lands in the wrong place and unlinks the window
  // from a session the caller never mentioned.
  //
  // `-d` because move-window otherwise selects what it moved, taking the
  // user's focus somewhere they did not ask to go.
  const std::string source = target();
  const std::string target = std::string{session_id()} + ":" + std::to_string(index);
  return effect(run({"move-window", "-d", "-s", source, "-t", target}));
}

expected<Window, CommandFailure> Window::refresh() const {
  return detail::describe<Window>(backend(), target(), id());
}

expected<std::vector<OptionEntry>, CommandFailure> Window::options() const {
  return listed(run(scoped("show-options", "-w", target(), {"-A"})));
}

expected<OptionEntry, CommandFailure> Window::option(std::string_view name) const {
  return named(run(scoped("show-options", "-w", target(), {"-A", name})), name);
}

expected<void, CommandFailure> Window::set_option(std::string_view name,
                                                  std::string_view value) const {
  return effect(run(scoped("set-option", "-w", target(), {name, value})));
}

expected<void, CommandFailure> Window::unset_option(std::string_view name) const {
  return effect(run(scoped("set-option", "-w", target(), {"-u", name})));
}

// --- Pane ------------------------------------------------------------------

expected<Window, CommandFailure> Pane::window() const {
  return detail::describe<Window>(backend(), id());
}

expected<Session, CommandFailure> Pane::session() const {
  return detail::describe<Session>(backend(), id());
}

expected<void, CommandFailure> Pane::send_text(std::string_view text) const {
  const auto arguments = literal_arguments(text);
  if (!arguments.has_value()) {
    return unexpected(rejected("text to send is empty"));
  }
  std::vector<std::string> command{"send-keys", "-t", std::string{id()}};
  command.insert(command.end(), arguments->begin(), arguments->end());
  return effect(run(command));
}

expected<void, CommandFailure> Pane::send_key(std::string_view key) const {
  // tmux accepts an unknown key name silently, so a typo would arrive as
  // input that never happened. Reject it here, where the caller can be told.
  if (!is_key_name(key)) {
    return unexpected(rejected("unknown key name: " + std::string{key}));
  }
  return effect(run({"send-keys", "-t", std::string{id()}, std::string{key}}));
}

expected<std::string, CommandFailure> Pane::capture() const {
  return capture(CaptureOptions{});
}

expected<std::string, CommandFailure> Pane::capture(CaptureOptions options) const {
  std::vector<std::string> command{"capture-pane", "-p", "-t", std::string{id()}};
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
}

expected<void, CommandFailure> Pane::set_width(long long width) const {
  if (width <= 0) {
    return unexpected(rejected("a pane cannot be resized to nothing"));
  }
  return effect(
      run({"resize-pane", "-t", std::string{id()}, "-x", std::to_string(width)}));
}

expected<void, CommandFailure> Pane::set_height(long long height) const {
  if (height <= 0) {
    return unexpected(rejected("a pane cannot be resized to nothing"));
  }
  return effect(
      run({"resize-pane", "-t", std::string{id()}, "-y", std::to_string(height)}));
}

expected<void, CommandFailure> Pane::swap_with(const Pane& other) const {
  return effect(
      run({"swap-pane", "-d", "-s", std::string{id()}, "-t", std::string{other.id()}}));
}

expected<Window, CommandFailure> Pane::break_out(std::string_view name) const {
  if (backend() == nullptr) {
    return unexpected(detail::disconnected());
  }
  std::vector<std::string> command{"break-pane", "-d", "-s", std::string{id()}, "-P"};
  if (!name.empty()) {
    command.emplace_back("-n");
    command.emplace_back(name);
  } else {
    // tmux 3.7 dereferences a null window name here and takes the whole
    // server down with it; 3.7a reverted the change. Naming the window when
    // the caller did not is the cheaper of the two outcomes, and only that
    // one release needs it — which is why the version is read exactly, not
    // compared numerically.
    const auto version = backend()->version();
    if (!version.has_value()) {
      return unexpected(version.error());
    }
    if (*version == Version{.major = 3, .minor = 7}) {
      command.emplace_back("-n");
      command.emplace_back("libtmux");
    }
  }
  return detail::one_entity<Window>(backend(), std::move(command), FormatArgument::flag,
                                    id());
}

expected<void, CommandFailure> Pane::join(const Window& target) const {
  return effect(
      run({"join-pane", "-s", std::string{id()}, "-t", std::string{target.id()}}));
}

expected<void, CommandFailure> Pane::enter_copy_mode() const {
  return effect(run({"copy-mode", "-t", std::string{id()}}));
}

expected<void, CommandFailure> Pane::leave_mode() const {
  // `-X cancel` is how a mode is left: there is no leave-mode command, and
  // the key that would do it depends on the caller's bindings.
  return effect(run({"send-keys", "-t", std::string{id()}, "-X", "cancel"}));
}

expected<void, CommandFailure> Pane::pipe_to(std::string_view command) const {
  if (command.empty()) {
    // An empty command is how tmux spells "stop", and a caller who reached
    // for pipe_to did not mean to stop. Say so rather than doing the
    // opposite of what was asked.
    return unexpected(rejected("a pipe needs a command; use stop_piping"));
  }
  return effect(
      run({"pipe-pane", "-t", std::string{id()}, "--", std::string{command}}));
}

expected<void, CommandFailure> Pane::stop_piping() const {
  return effect(run({"pipe-pane", "-t", std::string{id()}}));
}

expected<void, CommandFailure> Pane::set_title(std::string_view title) const {
  // No `--` here, though a title beginning with a dash is data: `-T`
  // consumes the next argument whatever it looks like, and adding the guard
  // makes tmux read the separator as the value and the title as a target —
  // "can't find pane: itled". The guard belongs on positional arguments,
  // like the text of set-buffer, not on the value of a flag.
  return effect(
      run({"select-pane", "-t", std::string{id()}, "-T", std::string{title}}));
}

expected<void, CommandFailure> Pane::respawn(bool replace_running) const {
  std::vector<std::string> command{"respawn-pane", "-t", std::string{id()}};
  if (replace_running) {
    command.emplace_back("-k");
  }
  return effect(run(command));
}

expected<void, CommandFailure> Pane::clear_history() const {
  return effect(run({"clear-history", "-t", std::string{id()}}));
}

expected<void, CommandFailure> Pane::select() const {
  return effect(run({"select-pane", "-t", std::string{id()}}));
}

expected<void, CommandFailure> Pane::show_message(std::string_view text) const {
  return effect(
      run({"display-message", "-t", std::string{id()}, "--", std::string{text}}));
}

expected<std::string, CommandFailure> Pane::expand(std::string_view format) const {
  return detail::expand_format<Pane>(backend(), id(), format);
}

expected<void, CommandFailure> Pane::kill() const {
  return effect(run({"kill-pane", "-t", std::string{id()}}));
}

expected<Pane, CommandFailure> Pane::refresh() const {
  return detail::describe<Pane>(backend(), id(), id());
}

expected<std::vector<OptionEntry>, CommandFailure> Pane::options() const {
  return listed(run(scoped("show-options", "-p", id(), {"-A"})));
}

expected<OptionEntry, CommandFailure> Pane::option(std::string_view name) const {
  return named(run(scoped("show-options", "-p", id(), {"-A", name})), name);
}

expected<void, CommandFailure> Pane::set_option(std::string_view name,
                                                std::string_view value) const {
  return effect(run(scoped("set-option", "-p", id(), {name, value})));
}

expected<void, CommandFailure> Pane::unset_option(std::string_view name) const {
  return effect(run(scoped("set-option", "-p", id(), {"-u", name})));
}

// --- Client ----------------------------------------------------------------

expected<void, CommandFailure> Pane::paste(const Buffer& buffer, bool consume) const {
  std::vector<std::string> command{"paste-buffer", "-t", std::string{id()}, "-b",
                                   std::string{buffer.name()}};
  if (consume) {
    command.emplace_back("-d");
  }
  return effect(run(command));
}

expected<std::string, CommandFailure> Buffer::contents() const {
  return run({"show-buffer", "-b", std::string{name()}});
}

expected<void, CommandFailure> Buffer::remove() const {
  return effect(run({"delete-buffer", "-b", std::string{name()}}));
}

expected<Session, CommandFailure> Client::session() const {
  // A client is targeted by name with -c, and the session it is looking at is
  // the one a format query in its context reports.
  return detail::one_entity<Session>(
      backend(), {"display-message", "-p", "-c", std::string{name()}},
      FormatArgument::message, name());
}

expected<void, CommandFailure> Client::switch_to(const Session& session) const {
  return effect(run(
      {"switch-client", "-c", std::string{name()}, "-t", std::string{session.id()}}));
}

expected<void, CommandFailure> Client::detach() const {
  return effect(run({"detach-client", "-t", std::string{name()}}));
}

expected<void, CommandFailure> Client::refresh() const {
  return effect(run({"refresh-client", "-t", std::string{name()}}));
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

// The connection and the id, which is what equality compares.
template <typename Entity>
std::size_t hash_identity(const void* connection, std::string_view identity) noexcept {
  const std::size_t left = std::hash<const void*>{}(connection);
  const std::size_t right = std::hash<std::string_view>{}(identity);
  // The mix libstdc++ and libc++ both use for pairs.
  return left ^ (right + 0x9e3779b9U + (left << 6U) + (left >> 2U));
}

} // namespace

std::size_t
std::hash<libtmux::Session>::operator()(const libtmux::Session& value) const noexcept {
  return hash_identity<libtmux::Session>(value.connection_identity(), value.id());
}

std::size_t
std::hash<libtmux::Window>::operator()(const libtmux::Window& value) const noexcept {
  return hash_identity<libtmux::Window>(value.connection_identity(), value.id());
}

std::size_t
std::hash<libtmux::Pane>::operator()(const libtmux::Pane& value) const noexcept {
  return hash_identity<libtmux::Pane>(value.connection_identity(), value.id());
}

std::size_t
std::hash<libtmux::Client>::operator()(const libtmux::Client& value) const noexcept {
  return hash_identity<libtmux::Client>(value.connection_identity(), value.name());
}
