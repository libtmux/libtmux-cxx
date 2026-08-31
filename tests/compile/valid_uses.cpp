// The positive control. Everything the compile-fail probes reject has a legal
// form, and this is it: if this stops compiling, those probes prove nothing.

#include <chrono>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <libtmux/libtmux.hpp>

static_assert(std::is_nothrow_copy_constructible_v<libtmux::AttachCommand>);
static_assert(std::is_nothrow_move_constructible_v<libtmux::AttachCommand>);
static_assert(std::is_nothrow_copy_assignable_v<libtmux::AttachCommand>);
static_assert(std::is_nothrow_move_assignable_v<libtmux::AttachCommand>);
static_assert(
    std::is_same_v<decltype(&libtmux::Session::attach_command),
                   libtmux::expected<libtmux::AttachCommand, libtmux::CommandFailure> (
                       libtmux::Session::*)() const>);
static_assert(std::is_same_v<decltype(&libtmux::Window::target),
                             std::string (libtmux::Window::*)() const>);
static_assert(static_cast<int>(libtmux::FailureKind::truncated) == 7);
static_assert(static_cast<int>(libtmux::FailureKind::unsupported) == 8);
static_assert(static_cast<int>(libtmux::SocketError::empty) == 0);
static_assert(static_cast<int>(libtmux::SocketError::name_has_separator) == 1);
static_assert(static_cast<int>(libtmux::SocketError::path_too_long) == 2);
static_assert(static_cast<int>(libtmux::SocketError::path_unsupported) == 3);
static_assert(libtmux::Window::kFields.size() == 13U);
static_assert(libtmux::Pane::kFields.size() == 19U);

std::vector<libtmux::Window> listed();

void uses() {
  const std::vector<libtmux::Window> rows = listed();
  const auto windows_of = libtmux::children_of<libtmux::Session, libtmux::Window>(
      rows, libtmux::window::session_id, libtmux::session::id);
  (void)windows_of;

  auto named = rows | libtmux::matching(libtmux::window::name == "editor");
  (void)libtmux::exactly_one(named);
  (void)libtmux::first(named);

  // A flag field is a predicate on its own, and compares against a bool.
  (void)libtmux::matching(libtmux::window::active);

  // The query vocabulary answers to its own namespace as well, and to the
  // same adaptor: a caller who imported only that must get what the design
  // note describes rather than a name that no longer exists.
  auto qualified = rows | libtmux::tmuxq::matching(libtmux::window::active);
  static_assert(
      std::is_same_v<decltype(qualified),
                     decltype(rows | libtmux::matching(libtmux::window::active))>);
  (void)(libtmux::window::active == false);
  // A string field offers string operations.
  (void)libtmux::window::name.starts_with("ed");
  // Framing a named string is fine; only a value that dies at the semicolon
  // is refused.
  const std::string text = "one\ntwo\n";
  (void)libtmux::capture_lines(text);
  (void)libtmux::capture_lines("one\ntwo\n");

  // A numeric field offers comparisons.
  (void)(libtmux::window::width > 80);
  (void)(libtmux::window::index <= 9);
}

// Going back to the pane selected before this one.
void the_last_pane_is_reselected(const libtmux::Window& window) {
  const libtmux::expected<libtmux::Pane, libtmux::CommandFailure> back =
      window.select_last_pane();
  (void)back;
}

// Showing one window in two sessions, and stopping.
void windows_are_linked_and_unlinked(const libtmux::Window& window,
                                     const libtmux::Session& other) {
  const libtmux::expected<void, libtmux::CommandFailure> shown = window.link_to(other);
  const libtmux::expected<void, libtmux::CommandFailure> hidden = window.unlink();
  (void)shown;
  (void)hidden;
}

// Entering the mode where a pane is read rather than typed into.
void a_pane_enters_and_leaves_copy_mode(const libtmux::Pane& pane) {
  const libtmux::expected<void, libtmux::CommandFailure> entered =
      pane.enter_copy_mode();
  const libtmux::expected<void, libtmux::CommandFailure> left = pane.leave_mode();
  (void)entered;
  (void)left;
}

// Copying a pane's output somewhere, and stopping.
void a_pane_is_piped(const libtmux::Pane& pane) {
  const libtmux::expected<void, libtmux::CommandFailure> started =
      pane.pipe_to("cat > /dev/null");
  const libtmux::expected<void, libtmux::CommandFailure> stopped = pane.stop_piping();
  (void)started;
  (void)stopped;
}

// Naming a pane and restarting what runs in it.
void panes_are_named_and_restarted(const libtmux::Pane& pane) {
  const libtmux::expected<void, libtmux::CommandFailure> named =
      pane.set_title("title");
  const libtmux::expected<void, libtmux::CommandFailure> restarted = pane.respawn(true);
  (void)named;
  (void)restarted;
}

// Taking the pane tree apart and putting it back.
void panes_move_between_windows(const libtmux::Pane& pane,
                                const libtmux::Window& target) {
  const libtmux::expected<void, libtmux::CommandFailure> rejoined = pane.join(target);
  (void)rejoined;
}

// Waiting on a channel, as a caller writes it.
void a_channel_is_waited_on(const libtmux::Server& server) {
  const libtmux::expected<void, libtmux::CommandFailure> released =
      server.wait_for("channel", std::chrono::seconds{5});
  const libtmux::expected<void, libtmux::CommandFailure> sent =
      server.signal("channel");
  (void)released;
  (void)sent;
}

// Opening a control stream with defaults or caller-selected policy, while the
// Server remains the only owner of its socket route.
void control_streams_are_opened(const libtmux::Server& server) {
  const auto control_pointer = &libtmux::Server::control;
  libtmux::ConnectionOptions options{.pane_output = true,
                                     .pause_after = std::chrono::seconds{2}};
  const libtmux::expected<libtmux::Connection, libtmux::ProtocolError> direct =
      server.control_with_options("work", options);
  const libtmux::expected<libtmux::Connection, libtmux::ProtocolError> defaults =
      server.control("work");
  (void)direct;
  (void)defaults;
  (void)control_pointer;
}

// Asking tmux what it understands.
void commands_are_listed(const libtmux::Server& server) {
  const libtmux::expected<std::vector<libtmux::Command>, libtmux::CommandFailure>
      known = server.commands();
  (void)known;
}

// Asking tmux to expand a format, and telling a client something, at every
// level that accepts a target.
void formats_expand_and_messages_show(const libtmux::Server& server,
                                      const libtmux::Session& session,
                                      const libtmux::Window& window,
                                      const libtmux::Pane& pane) {
  const libtmux::expected<std::string, libtmux::CommandFailure> answers[] = {
      server.expand("#{socket_path}"), session.expand("#{session_windows}"),
      window.expand("#{window_name}"), pane.expand("#{pane_current_command}")};
  const libtmux::expected<void, libtmux::CommandFailure> told[] = {
      server.show_message("done"), session.show_message("done"),
      window.show_message("done"), pane.show_message("done")};
  (void)answers;
  (void)told;
}

// Starting something with variables of its own, at each level that takes
// them.
void creation_carries_an_environment(const libtmux::Server& server,
                                     const libtmux::Session& session,
                                     const libtmux::Window& window) {
  libtmux::NewSessionOptions session_options;
  session_options.name = "built";
  session_options.environment = {{"EDITOR", "vi"}};
  libtmux::NewWindowOptions window_options;
  window_options.name = "built";
  window_options.environment = {{"EDITOR", "vi"}};
  libtmux::SplitOptions split_options;
  split_options.environment = {{"EDITOR", "vi"}};
  const auto made = server.new_session(session_options);
  const auto added = session.new_window(window_options);
  const auto split = window.split(split_options);
  (void)made;
  (void)added;
  (void)split;
}

// Attaching is a command line rather than a call, and sending clients away
// is a call.
void clients_come_and_go(const libtmux::Session& session) {
  const libtmux::expected<libtmux::AttachCommand, libtmux::CommandFailure> attach =
      session.attach_command();
  const libtmux::expected<void, libtmux::CommandFailure> sent =
      session.detach_clients();
  (void)attach;
  (void)sent;
}

void targets_can_report_unsupported(const libtmux::Window& window) {
  const libtmux::expected<std::string, libtmux::CommandFailure> target =
      window.checked_target();
  (void)target;
}

void capabilities_are_available_from_the_umbrella(const libtmux::Server& server) {
  const libtmux::ServerCapabilities capabilities = server.capabilities();
  const bool can_inspect =
      capabilities.supports(libtmux::ServerFeature::exact_inspection);
  (void)can_inspect;
}

void asynchronous_commands_name_their_runtime(const libtmux::Server& server) {
  auto started =
      libtmux::CommandRuntime::start(libtmux::CommandRuntimeConfig{.capacity = 4U});
  if (!started.has_value()) {
    return;
  }
  auto runtime = *std::move(started);
  auto submitted =
      server.try_submit(runtime, {"display-message", "-p", "explicit runtime"});
  if (submitted.has_value()) {
    std::move(*submitted).detach();
  }
  const libtmux::CommandRuntimeSnapshot snapshot = runtime.snapshot();
  const std::size_t dispatched = runtime.dispatch_ready();
  const std::size_t discarded = runtime.discard_ready();
  runtime.request_stop();
  const libtmux::CommandRuntimeShutdown shutdown = runtime.close();
  (void)snapshot;
  (void)dispatched;
  (void)discarded;
  (void)shutdown;
}

// Binding a key, with the command as argv rather than a quoted string.
void keys_bind_and_unbind(const libtmux::Server& server) {
  const libtmux::expected<void, libtmux::CommandFailure> bound =
      server.bind_key("mytable", "X", {"display-message", "hello"});
  const libtmux::expected<void, libtmux::CommandFailure> repeating =
      server.bind_key("mytable", "Y", {"resize-pane", "-L"}, /*repeatable=*/true);
  const libtmux::expected<void, libtmux::CommandFailure> gone =
      server.unbind_key("mytable", "X");
  (void)bound;
  (void)repeating;
  (void)gone;
}

// Scripting the server: a shell command, and a file of tmux commands.
void the_server_runs_scripts(const libtmux::Server& server) {
  const libtmux::expected<void, libtmux::CommandFailure> results[] = {
      server.run_shell("true"), server.run_shell("true", /*background=*/true),
      server.source_file("/tmp/conf"), server.check_file("/tmp/conf")};
  (void)results;
}

// Buffers read from and written to files.
void buffers_move_through_files(const libtmux::Server& server) {
  const libtmux::expected<void, libtmux::CommandFailure> loaded =
      server.load_buffer("name", "/tmp/in.txt");
  const libtmux::expected<void, libtmux::CommandFailure> saved =
      server.save_buffer("name", "/tmp/out.txt");
  (void)loaded;
  (void)saved;
}

// The server's cut buffers, as a caller writes them.
void buffers_are_set_read_and_removed(const libtmux::Server& server) {
  const libtmux::expected<void, libtmux::CommandFailure> put =
      server.set_buffer("name", "text");
  const libtmux::expected<std::vector<libtmux::Buffer>, libtmux::CommandFailure> held =
      server.buffers();
  (void)put;
  (void)held;
}

// Stepping through arrangements and turning the panes in one.
void a_window_changes_its_arrangement(const libtmux::Window& window) {
  const libtmux::expected<void, libtmux::CommandFailure> stepped = window.next_layout();
  const libtmux::expected<void, libtmux::CommandFailure> back =
      window.previous_layout();
  const libtmux::expected<void, libtmux::CommandFailure> turned = window.rotate();
  (void)stepped;
  (void)back;
  (void)turned;
}

// Dropping what a pane remembers, as a caller writes it.
void a_pane_forgets_its_scrollback(const libtmux::Pane& pane) {
  const libtmux::expected<void, libtmux::CommandFailure> dropped = pane.clear_history();
  (void)dropped;
}

// Choosing the active window, as a caller writes it.
void a_window_is_selected(const libtmux::Window& window) {
  const libtmux::expected<void, libtmux::CommandFailure> chosen = window.select();
  (void)chosen;
}

// A client is a value read from a listing, like every other entity.
void a_client_is_a_value_read_from_a_listing(const libtmux::Server& server) {
  const libtmux::expected<std::vector<libtmux::Client>, libtmux::CommandFailure>
      clients = server.clients();
  (void)clients;
}

// Making a session and ending the server, as a caller writes them.
void server_sessions_are_made_and_ended(const libtmux::Server& server) {
  const libtmux::expected<libtmux::Session, libtmux::CommandFailure> made =
      server.new_session("probe");
  const libtmux::expected<void, libtmux::CommandFailure> ended = server.kill();
  (void)made;
  (void)ended;
}

// Relative window navigation, as a caller writes it. Each answers with the
// window the selection landed on, so the result composes with the entity
// surface rather than needing a second listing to find out where it went.
void navigation_answers_with_a_window(const libtmux::Session& session) {
  const libtmux::expected<libtmux::Window, libtmux::CommandFailure> next =
      session.select_next_window();
  const libtmux::expected<libtmux::Window, libtmux::CommandFailure> previous =
      session.select_previous_window();
  const libtmux::expected<libtmux::Window, libtmux::CommandFailure> last =
      session.select_last_window();
  (void)next;
  (void)previous;
  (void)last;
}
