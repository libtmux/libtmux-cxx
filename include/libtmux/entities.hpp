#pragma once

// The tmux object hierarchy.
//
// A Session, Window, Pane or Client is one row of a snapshot: a shared pointer
// to the listing it came from plus the index of its row. That representation
// is what lets an entity be copied, stored in a container and returned from a
// function with nothing to keep alive alongside it, while still costing no
// per-row allocation and no tmux call to read.
//
// Reading a field is local and cannot fail. Every method returning `expected`
// runs tmux, and a returned entity describes the moment that command ran:
// entities do not update themselves, `refresh` takes a new snapshot.
//
// Psmux numbers windows and panes per session, so Windows snapshots retain
// their owning session identity alongside tmux's `$0`, `@0`, and `%0` IDs.
//
// Every field below is a format token tmux 3.2a already registers, which is
// the oldest version this library supports. That is a hard constraint rather
// than a preference: tmux expands a token it does not know to the empty
// string, so requesting a newer one would read as a present-but-empty value on
// an older server instead of failing.

#include "libtmux/abi.hpp"
#include "libtmux/command.hpp"
#include "libtmux/expected.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "libtmux/filter_expr.hpp"
#include "libtmux/options.hpp"
#include "libtmux/snapshot.hpp"

LIBTMUX_NAMESPACE_BEGIN

class Server;
class Session;
class Window;
class Pane;
class Buffer;
class Client;
class Command;

// What a creation command can be told.
//
// Every field maps to a flag tmux 3.2a already has, and every default is what
// tmux does when the flag is absent — except focus, which defaults to leaving
// it where the user put it. A library that moves someone's cursor because it
// created something is a library people stop calling.
//
// Aggregates rather than parameters: a caller writes only the fields it cares
// about, and a field added later does not renumber anything.

struct SplitOptions {
  // Side by side. tmux stacks by default.
  bool horizontal{false};
  // Before the target rather than after it.
  bool before{false};
  // Spanning the full width or height of the window rather than of the pane.
  bool full_size{false};
  // Where the new pane starts. Empty inherits from the window.
  std::string start_directory{};
  // Run this instead of the default shell.
  std::string shell_command{};
  // Size as a percentage of the space being divided.
  std::optional<int> percentage{};
  // Make the new pane the active one.
  bool focus{false};
  // Variables the new process starts with, on top of what tmux passes down.
  //
  // Pairs rather than `NAME=value` strings: tmux takes a `-e` without an `=`
  // without complaint and creates nothing, so the pair is joined here and a
  // name carrying an `=` is refused where the command is built.
  //
  // An empty value sets the variable to empty. It does not remove it.
  std::vector<std::pair<std::string, std::string>> environment{};
};

struct NewWindowOptions {
  std::string name{};
  std::string start_directory{};
  std::string shell_command{};
  // Immediately after the current window rather than at the end.
  bool after_current{false};
  // Put the window at this index rather than at the next free one.
  //
  // tmux refuses an index already in use rather than shifting anything, and
  // that refusal is kept: a workspace rebuilt over a running one should say
  // so rather than quietly land somewhere else.
  std::optional<long long> index{};
  bool focus{false};
  // Variables the new process starts with, on top of what tmux passes down.
  //
  // Pairs rather than `NAME=value` strings: tmux takes a `-e` without an `=`
  // without complaint and creates nothing, so the pair is joined here and a
  // name carrying an `=` is refused where the command is built.
  //
  // An empty value sets the variable to empty. It does not remove it.
  std::vector<std::pair<std::string, std::string>> environment{};
};

struct NewSessionOptions {
  std::string name{};
  std::string start_directory{};
  // The name of the window the session starts with.
  std::string first_window_name{};
  std::string shell_command{};
  // The size tmux gives the session while no client is attached. Without it a
  // detached session is 80x24, and a pane that reports its size to a program
  // reports that.
  std::optional<int> width{};
  std::optional<int> height{};
  // Variables the new process starts with, on top of what tmux passes down.
  //
  // Pairs rather than `NAME=value` strings: tmux takes a `-e` without an `=`
  // without complaint and creates nothing, so the pair is joined here and a
  // name carrying an `=` is refused where the command is built.
  //
  // An empty value sets the variable to empty. It does not remove it.
  std::vector<std::pair<std::string, std::string>> environment{};
};

struct CaptureOptions {
  // Where to start, counting back into the scrollback. Absent starts at the
  // top of the visible pane.
  std::optional<int> start_line{};
  std::optional<int> end_line{};
  // Everything tmux still remembers, which is what a caller reading history
  // usually means.
  bool whole_history{false};
  // Join a line tmux wrapped back into the one line it was.
  bool join_wrapped{false};
  // Keep the escape sequences rather than the text they produced.
  bool with_escape_sequences{false};
  // Keep the spaces at the end of a line, which tmux otherwise trims.
  bool keep_trailing_spaces{false};
  // How much of the answer this call is prepared to hold. A scrollback can be
  // far larger than the default, and one that does not fit is reported.
  std::optional<std::size_t> output_limit{};
};

namespace detail {

// tmux renders every value as text. These read the three shapes it uses, and
// answer zero, false or the epoch for a value it did not render — which within
// the supported version range means tmux had nothing to say, not that the
// token was unknown.
[[nodiscard]] inline long long to_number(std::string_view text) noexcept {
  long long value = 0;
  const char* const end = text.data() + text.size();
  const auto [stopped, code] = std::from_chars(text.data(), end, value);
  return code == std::errc{} && stopped == end ? value : 0;
}

// tmux renders flag formats as "1" or "0"; anything else is not the flag.
[[nodiscard]] inline bool to_flag(std::string_view text) noexcept {
  return text == "1";
}

[[nodiscard]] inline std::chrono::sys_seconds to_time(std::string_view text) noexcept {
  return std::chrono::sys_seconds{std::chrono::seconds{to_number(text)}};
}

[[nodiscard]] inline bool same_entity_id(std::string_view left_id,
                                         std::string_view left_session_id,
                                         std::string_view right_id,
                                         std::string_view right_session_id) noexcept {
#if defined(_WIN32)
  return left_id == right_id && left_session_id == right_session_id;
#else
  static_cast<void>(left_session_id);
  static_cast<void>(right_session_id);
  return left_id == right_id;
#endif
}

// The storage every entity has, in one place: the snapshot that owns the bytes
// and which of its rows this entity is. Inherited privately — an entity is
// implemented in terms of a row, it is not a kind of row.
class Row {
public:
  Row(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept
      : snapshot_{std::move(snapshot)}, row_{row} {}

protected:
  [[nodiscard]] std::string_view value(std::size_t index) const noexcept {
    return snapshot_->rows()[row_][index];
  }

  [[nodiscard]] std::string_view value(std::string_view field) const noexcept {
    const std::size_t index = snapshot_->index_of(field);
    const auto& row = snapshot_->rows()[row_];
    return index < row.size() ? row[index] : std::string_view{};
  }

  [[nodiscard]] const std::shared_ptr<const Backend>& backend() const noexcept {
    return snapshot_->backend();
  }

  // Run a command for this entity. An entity read out of a recording has no
  // server to run it against, and says so rather than doing nothing.
  [[nodiscard]] expected<std::string, CommandFailure>
  run(const CommandRequest& command,
      std::optional<std::size_t> output_limit = {}) const;

public:
  // The server this entity came from, for a command the typed surface does not
  // cover. This rather than a public `run`: a command built there would lose
  // the qualified target the entity knows to use, and a caller reaching for
  // the escape hatch should be able to see that they are back to raw argv.
  [[nodiscard]] expected<Server, CommandFailure> server() const;

  // Which tmux server incarnation this came from. Equality and hashing are
  // defined in terms of it, so two handles opened on one live socket agree but
  // a handle kept across a restart does not become a handle to the replacement.
  //
  // Empty for a value read out of a recording, which is on no server at all.
  [[nodiscard]] std::string_view connection_identity() const noexcept;

protected:
  // Whether another value came from the same tmux server. Out of line because
  // answering it needs the connection type, which no installed header sees.
  [[nodiscard]] bool same_connection(const Row& other) const noexcept;

private:
  std::shared_ptr<const Snapshot> snapshot_;
  std::size_t row_;
};

} // namespace detail

// An attach argv and the private route selecting this server incarnation.
// Keep it alive until the client exits; same-process `exec` leaves the route on disk.
class AttachCommand final {
public:
  AttachCommand(const AttachCommand&) noexcept;
  AttachCommand(AttachCommand&&) noexcept;
  AttachCommand& operator=(const AttachCommand&) noexcept;
  AttachCommand& operator=(AttachCommand&&) noexcept;
  ~AttachCommand();

  // Exec-order arguments; empty after this value is moved from.
  [[nodiscard]] const std::vector<std::string>& argv() const noexcept;

private:
  struct State;
  explicit AttachCommand(std::shared_ptr<const State> state) noexcept;

  friend class Session;
  std::shared_ptr<const State> state_;
};

class Session : private detail::Row {
public:
  static constexpr std::string_view kNoun{"session"};
  static constexpr std::array kFields{
      std::string_view{"session_id"},       std::string_view{"session_name"},
      std::string_view{"session_attached"}, std::string_view{"session_windows"},
      std::string_view{"session_path"},     std::string_view{"session_created"},
      std::string_view{"session_group"},    std::string_view{"session_grouped"}};

  Session(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept
      : Row{std::move(snapshot), row} {}

  using Row::connection_identity;
  using Row::server;

  [[nodiscard]] std::string_view id() const noexcept { return value(0); }
  [[nodiscard]] std::string_view name() const noexcept { return value(1); }
  // `session_attached` counts clients rather than rendering a flag, so any
  // count other than zero means attached.
  [[nodiscard]] bool attached() const noexcept { return client_count() != 0; }
  [[nodiscard]] long long client_count() const noexcept {
    return detail::to_number(value(2));
  }
  [[nodiscard]] long long window_count() const noexcept {
    return detail::to_number(value(3));
  }
  // The directory a new window starts in, not the shell's current directory.
  [[nodiscard]] std::string_view path() const noexcept { return value(4); }
  [[nodiscard]] std::chrono::sys_seconds created() const noexcept {
    return detail::to_time(value(5));
  }
  // Empty unless the session belongs to a group sharing its windows.
  [[nodiscard]] std::string_view group() const noexcept { return value(6); }
  [[nodiscard]] bool grouped() const noexcept { return detail::to_flag(value(7)); }

  // Two values are the same session when they name the same tmux object on the
  // same connection — not when they were listed at the same moment. A
  // session refreshed after a rename equals the one it was refreshed from.
  [[nodiscard]] bool operator==(const Session& other) const noexcept {
    return same_connection(other) && id() == other.id();
  }

  [[nodiscard]] expected<std::vector<Window>, CommandFailure> windows() const;
  [[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
  [[nodiscard]] expected<Window, CommandFailure> active_window() const;
  [[nodiscard]] expected<Pane, CommandFailure> active_pane() const;

  // Move the selection, and answer with the window it landed on.
  //
  // Named for what they do rather than for what they return: `next_window()`
  // would read as a question, and these change which window is active.
  //
  // Relative navigation is tmux's to perform, not a caller's to compute.
  // Next and previous wrap around the window list, and "last" means the
  // previously selected window — state only the server holds, which a caller
  // listing windows has no way to reconstruct.
  //
  // Each fails when there is nowhere to go, as tmux does: a session with one
  // window refuses all three rather than selecting the window already active.
  [[nodiscard]] expected<Window, CommandFailure> select_next_window() const;
  [[nodiscard]] expected<Window, CommandFailure> select_previous_window() const;
  [[nodiscard]] expected<Window, CommandFailure> select_last_window() const;

  // Created detached: a library call that stole the terminal would be a
  // surprise, and attaching is a separate decision.
  [[nodiscard]] expected<Window, CommandFailure>
  new_window(std::string_view name) const;
  [[nodiscard]] expected<Window, CommandFailure>
  new_window(NewWindowOptions options) const;

  [[nodiscard]] expected<void, CommandFailure> rename(std::string_view name) const;
  [[nodiscard]] expected<void, CommandFailure> kill() const;
  [[nodiscard]] expected<Session, CommandFailure> refresh() const;

  // Session options. Reading reports the value tmux would use, marking one
  // that comes from a wider scope as inherited rather than hiding it.
  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
  [[nodiscard]] expected<OptionEntry, CommandFailure>
  option(std::string_view name) const;
  [[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name,
                                                          std::string_view value) const;
  // Remove the value set here, so the wider scope shows through again.
  [[nodiscard]] expected<void, CommandFailure>
  unset_option(std::string_view name) const;

  // A command line that attaches a terminal to this session.
  //
  // Not a method that attaches: a tmux client needs a terminal, and every
  // command this library runs talks to it through pipes, so an attach it
  // performed itself could only ever fail. Spawn the returned argv and retain
  // the value until that client exits.
  [[nodiscard]] expected<AttachCommand, CommandFailure> attach_command() const;

  // Send every client here away, leaving the session running.
  [[nodiscard]] expected<void, CommandFailure> detach_clients() const;

  // Ask tmux to expand a format against this session. The fields above are
  // what a session is; this reaches the rest of tmux's vocabulary without a
  // method per variable.
  //
  // A target tmux cannot find is not an error to tmux. It expands the fields
  // it cannot resolve to nothing, prints the literals around them, and exits
  // zero — so a session that has been killed answers with a blank that reads
  // like a value. This asks for the session's own id alongside the caller's
  // format and reports `missing` when the answer is not this session.
  [[nodiscard]] expected<std::string, CommandFailure>
  expand(std::string_view format) const;

  // Put a message on the status line of every client attached here, and send
  // it to a control client as `%message`.
  //
  // tmux expands the text as a format, so a `#{...}` in it is substituted
  // rather than shown. Text built from data belongs in `escape_literal` first.
  [[nodiscard]] expected<void, CommandFailure>
  show_message(std::string_view text) const;

  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> hooks() const;
  [[nodiscard]] expected<void, CommandFailure> set_hook(std::string_view name,
                                                        std::string_view command) const;
};

class Window : private detail::Row {
public:
  static constexpr std::string_view kNoun{"window"};
  static constexpr std::string_view kSessionNameField{"session_name"};
  static constexpr std::array kFields{std::string_view{"window_id"},
                                      std::string_view{"window_name"},
                                      std::string_view{"window_active"},
                                      std::string_view{"session_id"},
                                      std::string_view{"window_index"},
                                      std::string_view{"window_panes"},
                                      std::string_view{"window_width"},
                                      std::string_view{"window_height"},
                                      std::string_view{"window_layout"},
                                      std::string_view{"window_zoomed_flag"},
                                      std::string_view{"window_bell_flag"},
                                      std::string_view{"window_activity_flag"},
                                      std::string_view{"window_linked_sessions"}};

  Window(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept
      : Row{std::move(snapshot), row} {}

  using Row::connection_identity;
  using Row::server;

  [[nodiscard]] std::string_view id() const noexcept { return value(0); }
  [[nodiscard]] std::string_view name() const noexcept { return value(1); }
  [[nodiscard]] bool active() const noexcept { return detail::to_flag(value(2)); }
  // The link to the parent, carried in the row so traversal upward costs
  // nothing until the parent itself is wanted.
  [[nodiscard]] std::string_view session_id() const noexcept { return value(3); }
  // Position within its session, which `base-index` is free to start anywhere.
  [[nodiscard]] long long index() const noexcept { return detail::to_number(value(4)); }
  [[nodiscard]] long long pane_count() const noexcept {
    return detail::to_number(value(5));
  }
  [[nodiscard]] long long width() const noexcept { return detail::to_number(value(6)); }
  [[nodiscard]] long long height() const noexcept {
    return detail::to_number(value(7));
  }
  // tmux's own layout description, which `select-layout` accepts back.
  [[nodiscard]] std::string_view layout() const noexcept { return value(8); }
  [[nodiscard]] bool zoomed() const noexcept { return detail::to_flag(value(9)); }
  [[nodiscard]] bool bell() const noexcept { return detail::to_flag(value(10)); }
  [[nodiscard]] bool activity() const noexcept { return detail::to_flag(value(11)); }
  // How many sessions hold this window. More than one means the same window
  // is shown in several places, and a command aimed at a bare id could land
  // on any of them — which is why targets here are qualified.
  [[nodiscard]] long long linked_sessions() const noexcept {
    return detail::to_number(value(12));
  }
  // The owning psmux route carried by Windows live snapshots. Empty on POSIX
  // and in recordings made from the backward-compatible `kFields` schema.
  [[nodiscard]] std::string_view session_name() const noexcept {
    return value(kSessionNameField);
  }

  // Two values are the same window when they name the same tmux object on the
  // same connection — not when they were listed at the same moment. A
  // window refreshed after a rename equals the one it was refreshed from.
  [[nodiscard]] bool operator==(const Window& other) const noexcept {
    return same_connection(other) &&
           detail::same_entity_id(id(), session_id(), other.id(), other.session_id());
  }

  // How to address this window, and the reason a window id alone will not do.
  //
  // The same window can be linked into several sessions, and a bare `@id`
  // leaves tmux to pick one of those homes: the index it reports, the session
  // it names, and the link a move or a kill lands on then depend on a choice
  // the caller did not make. Qualifying by the session this window was listed
  // from names one link.
  //
  // Prefer `checked_target()`; this source-compatible form returns an empty
  // string when psmux cannot bind a reusable target without a stale race.
  [[nodiscard]] std::string target() const;
  [[nodiscard]] expected<std::string, CommandFailure> checked_target() const;

  [[nodiscard]] expected<Session, CommandFailure> session() const;
  [[nodiscard]] expected<std::vector<Pane>, CommandFailure> panes() const;
  [[nodiscard]] expected<Pane, CommandFailure> active_pane() const;

  [[nodiscard]] expected<Pane, CommandFailure> split() const;
  [[nodiscard]] expected<Pane, CommandFailure> split(SplitOptions options) const;
  [[nodiscard]] expected<void, CommandFailure> rename(std::string_view name) const;

  // Rearrange the panes. tmux names five layouts and also accepts the layout
  // description `layout()` returns, which is how a saved arrangement is
  // restored exactly.
  [[nodiscard]] expected<void, CommandFailure>
  select_layout(std::string_view layout) const;
  [[nodiscard]] expected<void, CommandFailure> resize(long long width,
                                                      long long height) const;
  // Exchange positions with another window, keeping both ids.
  // Step through tmux's preset arrangements, and turn the panes within the
  // one in use.
  //
  // The layouts are tmux's list, not a caller's: asking for "the next one"
  // is the only way to reach them without naming each. Rotating is a
  // different act — it moves which pane occupies which cell and leaves the
  // cells where they are.
  //
  // None of the three refuses a window holding a single pane. tmux accepts
  // all of them there and changes nothing, which is worth knowing before
  // treating success as evidence that something moved.
  [[nodiscard]] expected<void, CommandFailure> next_layout() const;
  [[nodiscard]] expected<void, CommandFailure> previous_layout() const;
  [[nodiscard]] expected<void, CommandFailure> rotate() const;
  // Go back to the pane that was selected before the current one, and
  // answer with it.
  //
  // Server state, like the window equivalent: nothing in a listing says
  // which pane that was. A window holding one pane is refused rather than
  // reselecting it, which is what tmux does.
  [[nodiscard]] expected<Pane, CommandFailure> select_last_pane() const;

  // Show this window in another session as well. The same window, not a
  // copy: what runs in it is running in one place and shown in two.
  [[nodiscard]] expected<void, CommandFailure> link_to(const Session& target) const;

  // Stop showing it in the session this value came from.
  //
  // tmux refuses to remove the last link rather than leaving a window no
  // session holds, and that refusal is kept: a caller who meant to be rid
  // of the window wants `kill`, which says so.
  [[nodiscard]] expected<void, CommandFailure> unlink() const;

  [[nodiscard]] expected<void, CommandFailure> swap_with(const Window& other) const;
  // Move to another index within the same session.
  [[nodiscard]] expected<void, CommandFailure> move_to(long long index) const;

  // Ask tmux to expand a format against this window: the same reach the
  // session form gives, guarded the same way. A window that has gone reports
  // `missing` rather than answering with a blank.
  [[nodiscard]] expected<std::string, CommandFailure>
  expand(std::string_view format) const;

  // Show a message to the clients watching this window's session.
  //
  // The window is the context the text expands in, not just who sees it:
  // `#{window_name}` in a message sent from here names this window even
  // while another is the active one.
  [[nodiscard]] expected<void, CommandFailure>
  show_message(std::string_view text) const;

  [[nodiscard]] expected<void, CommandFailure> select() const;
  [[nodiscard]] expected<void, CommandFailure> kill() const;
  [[nodiscard]] expected<Window, CommandFailure> refresh() const;

  // Window options. tmux looks a named option up in the table it belongs to,
  // so the scope here selects which window provides the context and the
  // inheritance chain, not which names are legal.
  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
  [[nodiscard]] expected<OptionEntry, CommandFailure>
  option(std::string_view name) const;
  [[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name,
                                                          std::string_view value) const;
  [[nodiscard]] expected<void, CommandFailure>
  unset_option(std::string_view name) const;
};

class Pane : private detail::Row {
public:
  static constexpr std::string_view kNoun{"pane"};
  static constexpr std::string_view kSessionNameField{"session_name"};
  static constexpr std::array kFields{
      std::string_view{"pane_id"},      std::string_view{"pane_current_command"},
      std::string_view{"pane_active"},  std::string_view{"window_id"},
      std::string_view{"session_id"},   std::string_view{"pane_index"},
      std::string_view{"pane_title"},   std::string_view{"pane_pid"},
      std::string_view{"pane_tty"},     std::string_view{"pane_current_path"},
      std::string_view{"pane_width"},   std::string_view{"pane_height"},
      std::string_view{"pane_dead"},    std::string_view{"pane_in_mode"},
      std::string_view{"pane_at_top"},  std::string_view{"pane_at_bottom"},
      std::string_view{"pane_at_left"}, std::string_view{"pane_at_right"},
      std::string_view{"pane_pipe"}};

  Pane(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept
      : Row{std::move(snapshot), row} {}

  using Row::connection_identity;
  using Row::server;

  [[nodiscard]] std::string_view id() const noexcept { return value(0); }
  // What is running in the pane now, which is not what started it.
  [[nodiscard]] std::string_view command() const noexcept { return value(1); }
  [[nodiscard]] bool active() const noexcept { return detail::to_flag(value(2)); }
  [[nodiscard]] std::string_view window_id() const noexcept { return value(3); }
  [[nodiscard]] std::string_view session_id() const noexcept { return value(4); }
  [[nodiscard]] long long index() const noexcept { return detail::to_number(value(5)); }
  [[nodiscard]] std::string_view title() const noexcept { return value(6); }
  [[nodiscard]] long long pid() const noexcept { return detail::to_number(value(7)); }
  [[nodiscard]] std::string_view tty() const noexcept { return value(8); }
  [[nodiscard]] std::string_view path() const noexcept { return value(9); }
  [[nodiscard]] long long width() const noexcept {
    return detail::to_number(value(10));
  }
  [[nodiscard]] long long height() const noexcept {
    return detail::to_number(value(11));
  }
  // A pane whose program exited while `remain-on-exit` kept it on screen.
  [[nodiscard]] bool dead() const noexcept { return detail::to_flag(value(12)); }
  // Copy mode and its relatives, in which sent keys move the cursor rather
  // than reaching the program.
  [[nodiscard]] bool in_mode() const noexcept { return detail::to_flag(value(13)); }
  [[nodiscard]] bool at_top() const noexcept { return detail::to_flag(value(14)); }
  [[nodiscard]] bool at_bottom() const noexcept { return detail::to_flag(value(15)); }
  [[nodiscard]] bool at_left() const noexcept { return detail::to_flag(value(16)); }
  [[nodiscard]] bool at_right() const noexcept { return detail::to_flag(value(17)); }
  // Whether this pane's output is currently being copied to a command.
  [[nodiscard]] bool piping() const noexcept { return detail::to_flag(value(18)); }
  // The owning psmux route carried by Windows live snapshots. Empty on POSIX
  // and in recordings made from the backward-compatible `kFields` schema.
  [[nodiscard]] std::string_view session_name() const noexcept {
    return value(kSessionNameField);
  }

  // Two values are the same pane when they name the same tmux object on the
  // same connection — not when they were listed at the same moment. A
  // pane refreshed after a rename equals the one it was refreshed from.
  [[nodiscard]] bool operator==(const Pane& other) const noexcept {
    return same_connection(other) &&
           detail::same_entity_id(id(), session_id(), other.id(), other.session_id());
  }

  [[nodiscard]] expected<Window, CommandFailure> window() const;
  [[nodiscard]] expected<Session, CommandFailure> session() const;

  // Literal text, never interpreted as key names or formats, and never
  // followed by a newline the caller did not ask for.
  [[nodiscard]] expected<void, CommandFailure> send_text(std::string_view text) const;
  [[nodiscard]] expected<void, CommandFailure> send_key(std::string_view key) const;

  // The visible contents, as tmux printed them. `capture_lines` frames it into
  // lines, and takes a named string: the lines are views into it, so framing
  // this return value directly is a compile error rather than a dangling read.
  //
  // A pane's scrollback can be far larger than the default bound, and a
  // capture that does not fit is reported rather than cut, so a caller reading
  // history passes the size it is prepared to hold.
  [[nodiscard]] expected<std::string, CommandFailure> capture() const;
  [[nodiscard]] expected<std::string, CommandFailure>
  capture(CaptureOptions options) const;

  [[nodiscard]] expected<void, CommandFailure> set_width(long long width) const;
  [[nodiscard]] expected<void, CommandFailure> set_height(long long height) const;
  [[nodiscard]] expected<void, CommandFailure> swap_with(const Pane& other) const;

  // Take this pane out into a window of its own, which is returned. If it is
  // already the window's only pane, return that window without moving it.
  // An empty name leaves tmux to name the window after what is running.
  [[nodiscard]] expected<Window, CommandFailure>
  break_out(std::string_view name = {}) const;

  // Move this pane into another window, splitting it. The other half of
  // `break_out`: that takes the tree apart, this puts it back.
  //
  // The pane keeps its id, so a value held across the move still names it.
  // The window it came from disappears if it held nothing else, which is
  // why the target is named rather than inferred from where this pane is.
  [[nodiscard]] expected<void, CommandFailure> join(const Window& target) const;

  // Forget the scrollback, which is the only way to bound a pane's memory
  // without restarting what is running in it.
  // Put this pane into copy mode, where its contents can be scrolled and
  // selected rather than typed into.
  //
  // Needs no attached client: the mode is pane state, which `in_mode`
  // reports. Entering twice is harmless.
  [[nodiscard]] expected<void, CommandFailure> enter_copy_mode() const;

  // Leave whatever mode the pane is in.
  //
  // A pane in no mode is refused, with tmux's "not in a mode". That is kept
  // rather than smoothed into success: checking first would cost a round
  // trip and still race, and a caller who cares can read `in_mode` or
  // ignore the failure.
  [[nodiscard]] expected<void, CommandFailure> leave_mode() const;

  // Copy everything this pane prints to a shell command, until told to
  // stop. The command runs on the tmux server's machine with the pane's
  // output on its standard input.
  //
  // Starting a second pipe replaces the first: tmux keeps one per pane, so
  // there is nothing to close and nothing to leak.
  [[nodiscard]] expected<void, CommandFailure> pipe_to(std::string_view command) const;

  // Stop copying. Harmless on a pane that was not piping.
  [[nodiscard]] expected<void, CommandFailure> stop_piping() const;

  // Name this pane. The title is what `#{pane_title}` reports and what a
  // status line can show; it survives the process being replaced.
  [[nodiscard]] expected<void, CommandFailure> set_title(std::string_view title) const;

  // Start the pane's command again.
  //
  // tmux refuses a pane whose process is still running unless told to kill
  // it, and that refusal is kept rather than smoothed over: replacing a
  // live process is a decision, so `replace_running` has to be asked for.
  [[nodiscard]] expected<void, CommandFailure>
  respawn(bool replace_running = false) const;

  [[nodiscard]] expected<void, CommandFailure> clear_history() const;

  // Ask tmux to expand a format against this pane. `#{pane_current_command}`
  // and `#{pane_current_path}` are the two most callers reach for, and
  // neither is a field this class carries: both change under a value that
  // stays still.
  //
  // Guarded like the session and window forms, because tmux answers a pane
  // that has gone with a blank and a zero exit status.
  [[nodiscard]] expected<std::string, CommandFailure>
  expand(std::string_view format) const;

  // Show a message to the clients watching this pane's session, expanded
  // against this pane.
  [[nodiscard]] expected<void, CommandFailure>
  show_message(std::string_view text) const;

  // Deliver a buffer's text to this pane, as if it had been typed. The text
  // arrives on the command line and is not run: a caller wanting it executed
  // sends Enter afterwards, which is the same distinction `send_text` draws.
  //
  // `consume` is tmux's `-d`, deleting the buffer once it has been pasted,
  // which is what a caller treating it as a one-shot transfer wants.
  [[nodiscard]] expected<void, CommandFailure> paste(const Buffer& buffer,
                                                     bool consume = false) const;

  [[nodiscard]] expected<void, CommandFailure> select() const;
  [[nodiscard]] expected<void, CommandFailure> kill() const;
  [[nodiscard]] expected<Pane, CommandFailure> refresh() const;

  // Pane options, the narrowest scope tmux has, and the end of an inheritance
  // chain that runs pane, window, session, global.
  [[nodiscard]] expected<std::vector<OptionEntry>, CommandFailure> options() const;
  [[nodiscard]] expected<OptionEntry, CommandFailure>
  option(std::string_view name) const;
  [[nodiscard]] expected<void, CommandFailure> set_option(std::string_view name,
                                                          std::string_view value) const;
  [[nodiscard]] expected<void, CommandFailure>
  unset_option(std::string_view name) const;
};

// One command this tmux understands.
//
// The list is how a caller asks what the server can do rather than deducing
// it from a version string. A build with commands compiled out, or a
// version between releases, answers for itself.
class Command : private detail::Row {
public:
  static constexpr std::string_view kNoun{"command"};
  static constexpr std::array kFields{std::string_view{"command_list_name"},
                                      std::string_view{"command_list_alias"},
                                      std::string_view{"command_list_usage"}};

  Command(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept
      : Row{std::move(snapshot), row} {}

  using Row::connection_identity;
  using Row::server;

  [[nodiscard]] std::string_view name() const noexcept { return value(0); }
  // tmux's short form, such as `lscm` for `list-commands`. Empty when the
  // command has none.
  [[nodiscard]] std::string_view alias() const noexcept { return value(1); }
  // The flags and arguments, as tmux prints them in its own help.
  [[nodiscard]] std::string_view usage() const noexcept { return value(2); }

  [[nodiscard]] bool operator==(const Command& other) const noexcept {
    return same_connection(other) && name() == other.name();
  }
};

// A named piece of text the server holds, outliving the pane it came from.
//
// tmux's cut buffers are the clipboard between panes: a pane's selection
// lands in one, and pasting reads one back. They belong to the server, not
// to any pane, which is why they are listed from it.
class Buffer : private detail::Row {
public:
  static constexpr std::string_view kNoun{"buffer"};
  static constexpr std::array kFields{
      std::string_view{"buffer_name"}, std::string_view{"buffer_size"},
      std::string_view{"buffer_sample"}, std::string_view{"buffer_created"}};

  Buffer(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept
      : Row{std::move(snapshot), row} {}

  using Row::connection_identity;
  using Row::server;

  // Named by the caller, or by tmux as `buffer0` and upward when it is not.
  [[nodiscard]] std::string_view name() const noexcept { return value(0); }
  [[nodiscard]] long long size() const noexcept { return detail::to_number(value(1)); }
  // The opening of the contents, as tmux prints it in a listing. Truncated,
  // and with control characters rendered — `contents()` reads the bytes.
  [[nodiscard]] std::string_view sample() const noexcept { return value(2); }
  [[nodiscard]] std::chrono::sys_seconds created() const noexcept {
    return detail::to_time(value(3));
  }

  [[nodiscard]] bool operator==(const Buffer& other) const noexcept {
    return same_connection(other) && name() == other.name();
  }

  // The whole contents. tmux prints them with no trailing newline, so what
  // comes back is exactly what was put in.
  [[nodiscard]] expected<std::string, CommandFailure> contents() const;
  [[nodiscard]] expected<void, CommandFailure> remove() const;

private:
  friend class Server;
};

class Client : private detail::Row {
public:
  static constexpr std::string_view kNoun{"client"};
  static constexpr std::array kFields{
      std::string_view{"client_name"},     std::string_view{"client_session"},
      std::string_view{"client_readonly"}, std::string_view{"client_tty"},
      std::string_view{"client_width"},    std::string_view{"client_height"},
      std::string_view{"client_created"},  std::string_view{"client_activity"},
      std::string_view{"client_termname"}, std::string_view{"client_control_mode"}};

  Client(std::shared_ptr<const Snapshot> snapshot, std::size_t row) noexcept
      : Row{std::move(snapshot), row} {}

  using Row::connection_identity;
  using Row::server;

  // A client is named by its terminal path, which is the only stable handle
  // tmux gives; there is no client id format.
  [[nodiscard]] std::string_view name() const noexcept { return value(0); }
  [[nodiscard]] std::string_view session_name() const noexcept { return value(1); }
  [[nodiscard]] bool read_only() const noexcept { return detail::to_flag(value(2)); }
  [[nodiscard]] std::string_view tty() const noexcept { return value(3); }
  [[nodiscard]] long long width() const noexcept { return detail::to_number(value(4)); }
  [[nodiscard]] long long height() const noexcept {
    return detail::to_number(value(5));
  }
  [[nodiscard]] std::chrono::sys_seconds created() const noexcept {
    return detail::to_time(value(6));
  }
  [[nodiscard]] std::chrono::sys_seconds last_activity() const noexcept {
    return detail::to_time(value(7));
  }
  [[nodiscard]] std::string_view terminal() const noexcept { return value(8); }
  // A control-mode client is a program driving tmux, not a terminal.
  [[nodiscard]] bool control_mode() const noexcept { return detail::to_flag(value(9)); }

  // Two values are the same client when they name the same terminal on the
  // same connection.
  [[nodiscard]] bool operator==(const Client& other) const noexcept {
    return same_connection(other) && name() == other.name();
  }

  [[nodiscard]] expected<Session, CommandFailure> session() const;

  // Point this client at another session, leaving it attached.
  [[nodiscard]] expected<void, CommandFailure> switch_to(const Session& session) const;
  [[nodiscard]] expected<void, CommandFailure> detach() const;
  // Redraw, and tell tmux the size this client is now, which matters for a
  // control-mode client whose size tmux cannot otherwise observe.
  [[nodiscard]] expected<void, CommandFailure> refresh() const;
};

// Typed field handles. A handle exists for every field a filter can compare —
// the strings and the flags — and names the tmux token beside the accessor, so
// an expression can be evaluated here and translated to a tmux `-f` filter
// later without a second table to keep in step.

// Written as tmux would name it, with the detail that identifies it: an id
// and the thing a reader recognises it by. Declared against a forward-declared
// stream so no consumer pays for <ostream> to include an entity.
std::ostream& operator<<(std::ostream& stream, const Session& session);
std::ostream& operator<<(std::ostream& stream, const Window& window);
std::ostream& operator<<(std::ostream& stream, const Pane& pane);
std::ostream& operator<<(std::ostream& stream, const Client& client);

namespace session {

inline constexpr StringFieldHandle<Session> id{
    {Session::kFields[0], [](const Session& row) { return row.id(); }}};
inline constexpr StringFieldHandle<Session> name{
    {Session::kFields[1], [](const Session& row) { return row.name(); }}};
inline constexpr BoolFieldHandle<Session> attached{
    {Session::kFields[2], [](const Session& row) { return row.attached(); }}};
inline constexpr StringFieldHandle<Session> path{
    {Session::kFields[4], [](const Session& row) { return row.path(); }}};
inline constexpr StringFieldHandle<Session> group{
    {Session::kFields[6], [](const Session& row) { return row.group(); }}};
inline constexpr BoolFieldHandle<Session> grouped{
    {Session::kFields[7], [](const Session& row) { return row.grouped(); }}};
inline constexpr NumberFieldHandle<Session> client_count{
    {Session::kFields[2], [](const Session& row) { return row.client_count(); }}};
inline constexpr NumberFieldHandle<Session> window_count{
    {Session::kFields[3], [](const Session& row) { return row.window_count(); }}};

} // namespace session

namespace window {

inline constexpr StringFieldHandle<Window> id{
    {Window::kFields[0], [](const Window& row) { return row.id(); }}};
inline constexpr StringFieldHandle<Window> name{
    {Window::kFields[1], [](const Window& row) { return row.name(); }}};
inline constexpr BoolFieldHandle<Window> active{
    {Window::kFields[2], [](const Window& row) { return row.active(); }}};
inline constexpr StringFieldHandle<Window> session_id{
    {Window::kFields[3], [](const Window& row) { return row.session_id(); }}};
inline constexpr StringFieldHandle<Window> session_name{
    {Window::kSessionNameField, [](const Window& row) { return row.session_name(); }}};
inline constexpr StringFieldHandle<Window> layout{
    {Window::kFields[8], [](const Window& row) { return row.layout(); }}};
inline constexpr BoolFieldHandle<Window> zoomed{
    {Window::kFields[9], [](const Window& row) { return row.zoomed(); }}};
inline constexpr BoolFieldHandle<Window> bell{
    {Window::kFields[10], [](const Window& row) { return row.bell(); }}};
inline constexpr BoolFieldHandle<Window> activity{
    {Window::kFields[11], [](const Window& row) { return row.activity(); }}};
inline constexpr NumberFieldHandle<Window> index{
    {Window::kFields[4], [](const Window& row) { return row.index(); }}};
inline constexpr NumberFieldHandle<Window> pane_count{
    {Window::kFields[5], [](const Window& row) { return row.pane_count(); }}};
inline constexpr NumberFieldHandle<Window> width{
    {Window::kFields[6], [](const Window& row) { return row.width(); }}};
inline constexpr NumberFieldHandle<Window> height{
    {Window::kFields[7], [](const Window& row) { return row.height(); }}};

} // namespace window

namespace pane {

inline constexpr StringFieldHandle<Pane> id{
    {Pane::kFields[0], [](const Pane& row) { return row.id(); }}};
inline constexpr StringFieldHandle<Pane> command{
    {Pane::kFields[1], [](const Pane& row) { return row.command(); }}};
inline constexpr BoolFieldHandle<Pane> active{
    {Pane::kFields[2], [](const Pane& row) { return row.active(); }}};
inline constexpr StringFieldHandle<Pane> window_id{
    {Pane::kFields[3], [](const Pane& row) { return row.window_id(); }}};
inline constexpr StringFieldHandle<Pane> session_id{
    {Pane::kFields[4], [](const Pane& row) { return row.session_id(); }}};
inline constexpr StringFieldHandle<Pane> session_name{
    {Pane::kSessionNameField, [](const Pane& row) { return row.session_name(); }}};
inline constexpr StringFieldHandle<Pane> title{
    {Pane::kFields[6], [](const Pane& row) { return row.title(); }}};
inline constexpr StringFieldHandle<Pane> tty{
    {Pane::kFields[8], [](const Pane& row) { return row.tty(); }}};
inline constexpr StringFieldHandle<Pane> path{
    {Pane::kFields[9], [](const Pane& row) { return row.path(); }}};
inline constexpr BoolFieldHandle<Pane> dead{
    {Pane::kFields[12], [](const Pane& row) { return row.dead(); }}};
inline constexpr BoolFieldHandle<Pane> in_mode{
    {Pane::kFields[13], [](const Pane& row) { return row.in_mode(); }}};
inline constexpr NumberFieldHandle<Pane> index{
    {Pane::kFields[5], [](const Pane& row) { return row.index(); }}};
inline constexpr NumberFieldHandle<Pane> pid{
    {Pane::kFields[7], [](const Pane& row) { return row.pid(); }}};
inline constexpr NumberFieldHandle<Pane> width{
    {Pane::kFields[10], [](const Pane& row) { return row.width(); }}};
inline constexpr NumberFieldHandle<Pane> height{
    {Pane::kFields[11], [](const Pane& row) { return row.height(); }}};

} // namespace pane

namespace client {

inline constexpr StringFieldHandle<Client> name{
    {Client::kFields[0], [](const Client& row) { return row.name(); }}};
inline constexpr StringFieldHandle<Client> session_name{
    {Client::kFields[1], [](const Client& row) { return row.session_name(); }}};
inline constexpr BoolFieldHandle<Client> read_only{
    {Client::kFields[2], [](const Client& row) { return row.read_only(); }}};
inline constexpr StringFieldHandle<Client> tty{
    {Client::kFields[3], [](const Client& row) { return row.tty(); }}};
inline constexpr StringFieldHandle<Client> terminal{
    {Client::kFields[8], [](const Client& row) { return row.terminal(); }}};
inline constexpr BoolFieldHandle<Client> control_mode{
    {Client::kFields[9], [](const Client& row) { return row.control_mode(); }}};
inline constexpr NumberFieldHandle<Client> width{
    {Client::kFields[4], [](const Client& row) { return row.width(); }}};
inline constexpr NumberFieldHandle<Client> height{
    {Client::kFields[5], [](const Client& row) { return row.height(); }}};

} // namespace client

LIBTMUX_NAMESPACE_END

// Hashing an entity uses exactly what its equality compares, so values from
// separate listings key an unordered container consistently.
template <> struct std::hash<libtmux::Session> {
  [[nodiscard]] std::size_t operator()(const libtmux::Session& value) const noexcept;
};
template <> struct std::hash<libtmux::Window> {
  [[nodiscard]] std::size_t operator()(const libtmux::Window& value) const noexcept;
};
template <> struct std::hash<libtmux::Pane> {
  [[nodiscard]] std::size_t operator()(const libtmux::Pane& value) const noexcept;
};
template <> struct std::hash<libtmux::Client> {
  [[nodiscard]] std::size_t operator()(const libtmux::Client& value) const noexcept;
};
