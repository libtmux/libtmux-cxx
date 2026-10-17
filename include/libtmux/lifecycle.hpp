#pragma once

#include "libtmux/abi.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/server.hpp"
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

LIBTMUX_NAMESPACE_BEGIN

namespace detail {
struct Ownership;
struct LifecycleAccess;
} // namespace detail

/// The daemon and exact object accepted by an owner. A server has an empty id.
/// The reserved server option `@libtmux_owner_generation` contains 32 ASCII hex
/// characters. Adoption initializes an absent option, retains a valid one and
/// refuses an empty or malformed value. Do not shadow or change this option.
struct OwnershipReceipt {
  std::string pid;
  std::string start_time;
  std::string generation;
  std::string id;
  /// A newly started daemon can be rolled back before its option is installed.
  /// This nonce comes from its inherited `LIBTMUX_OWNER_STARTUP_NONCE`.
  std::string startup_nonce{};
};

/// Timeout and cancellation apply to acquisition, including waiting for the
/// find-or-create lock. Cleanup ignores the cancellation predicate and uses its own
/// timeout. It runs synchronously on the thread leaving the scope.
struct LifecycleOptions {
  std::chrono::milliseconds timeout{30000};
  std::chrono::milliseconds cleanup_timeout{5000};
  /// Nonthrowing, thread-safe predicate, polled during subprocess execution.
  /// Bind an atomic flag or a stop_token where the standard library supplies it.
  std::function<bool()> cancelled{};
};

/// The latest cleanup attempt. A failed attempt leaves `complete` false.
struct CleanupReport {
  bool complete{false};
  bool in_progress{false};
  std::size_t attempts{};
  std::optional<CommandFailure> failure{};
  std::exception_ptr exception{};
};

/// Retains cleanup responsibility and its result beyond an Owned scope.
/// Copies share one result. `close` is serialized and retryable after failure;
/// success is idempotent. Observers may inspect report(); recursive close from
/// the active observer returns overloaded. Destroying this handle alone performs
/// no cleanup.
class CleanupHandle final {
public:
  [[nodiscard]] expected<void, CommandFailure> close() const;
  [[nodiscard]] CleanupReport report() const;
  [[nodiscard]] OwnershipReceipt receipt() const;

private:
  explicit CleanupHandle(std::shared_ptr<detail::Ownership> state) noexcept;
  void close_noexcept() const noexcept;
  std::shared_ptr<detail::Ownership> state_;
  friend struct detail::LifecycleAccess;
  template <typename T> friend class Owned;
};

/// Creation can fail after tmux has created an object. `receipt` identifies
/// that object; `rollback` describes failed cleanup. Retain `cleanup` to
/// inspect or retry the rollback through the original daemon-bound transport.
/// With `uncertain` and no receipt, no object was guessed or destroyed.
struct LifecycleFailure {
  CommandFailure primary;
  std::optional<CommandFailure> rollback{};
  std::optional<OwnershipReceipt> receipt{};
  bool uncertain{false};
  std::exception_ptr exception{};
  std::optional<CleanupHandle> cleanup{};
  /// The captured command failure when `primary` reports a separate receipt
  /// identity or server ownership refusal. Retains the command's original kind,
  /// delivery, exit code and diagnostic; `exception` retains its observer error.
  std::optional<CommandFailure> command_failure{};
};

/// A move-only RAII owner. Lookup values remain borrowed. Destruction attempts
/// remote cleanup without throwing; retain `cleanup()` or call `close()` to
/// inspect the result. A window owner kills the window, all links and panes.
/// A pane may move and a session may be renamed without redirecting cleanup.
/// Use one Owned value from one thread; its CleanupHandle supports concurrent
/// close/report calls. Allocation and caller callbacks may still throw.
template <typename T> class Owned final {
public:
  Owned(Owned&&) noexcept = default;
  Owned& operator=(Owned&&) = delete;
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;
  ~Owned() noexcept { cleanup_.close_noexcept(); }

  [[nodiscard]] const T& get() const noexcept { return value_; }
  [[nodiscard]] const T* operator->() const noexcept { return &value_; }
  [[nodiscard]] CleanupHandle cleanup() const { return cleanup_; }
  [[nodiscard]] expected<void, CommandFailure> close() const {
    return cleanup_.close();
  }

private:
  Owned(T value, CleanupHandle cleanup)
      : value_{std::move(value)}, cleanup_{std::move(cleanup)} {}
  T value_;
  CleanupHandle cleanup_;
  friend struct detail::LifecycleAccess;
};

/// Both errors remain accessible when a body and cleanup fail. Exception
/// bodies appear in `body_exception`; expected-style bodies in `body_failure`.
struct OwnedScopeResult {
  std::optional<CommandFailure> body_failure{};
  std::exception_ptr body_exception{};
  CleanupReport cleanup;
  [[nodiscard]] bool ok() const noexcept {
    return !body_failure && !body_exception && cleanup.complete;
  }
};

/// Run a body returning expected<void, CommandFailure>, then checked cleanup.
/// Inspect this result without losing the body error to a cleanup error.
template <typename T, typename Body>
[[nodiscard]] OwnedScopeResult with_owned(Owned<T>& owner, Body&& body) {
  OwnedScopeResult result;
  try {
    auto ran = std::invoke(std::forward<Body>(body), owner.get());
    if (!ran) {
      result.body_failure = std::move(ran.error());
    }
  } catch (...) {
    result.body_exception = std::current_exception();
  }
  try {
    static_cast<void>(owner.close());
  } catch (...) {
    result.cleanup.exception = std::current_exception();
    return result;
  }
  result.cleanup = owner.cleanup().report();
  return result;
}

/// Explicitly accept destruction responsibility for an existing object.
/// POSIX subprocess tmux only; unsupported transports fail before dispatch.
[[nodiscard]] expected<Owned<Server>, LifecycleFailure>
adopt(const Server& server, LifecycleOptions options = {});
[[nodiscard]] expected<Owned<Session>, LifecycleFailure>
adopt(const Session& session, LifecycleOptions options = {});
[[nodiscard]] expected<Owned<Window>, LifecycleFailure>
adopt(const Window& window, LifecycleOptions options = {});
[[nodiscard]] expected<Owned<Pane>, LifecycleFailure>
adopt(const Pane& pane, LifecycleOptions options = {});

/// Create an owned object. A valid receipt survives command failure, timeout
/// and cancellation so rollback can target the creating daemon and exact id.
[[nodiscard]] expected<Owned<Session>, LifecycleFailure>
own_session(const Server& server, NewSessionOptions creation,
            LifecycleOptions options = {});
[[nodiscard]] expected<Owned<Window>, LifecycleFailure>
own_window(const Session& session, NewWindowOptions creation,
           LifecycleOptions options = {});
[[nodiscard]] expected<Owned<Pane>, LifecycleFailure>
own_pane(const Pane& target, SplitOptions creation = {}, LifecycleOptions options = {});
/// Start through a startable Server handle, creating the named first session.
/// Refuses an existing daemon. A random inherited startup nonce proves this
/// call started the daemon; the generation option alone cannot prove that.
[[nodiscard]] expected<Owned<Server>, LifecycleFailure>
own_server(const Server& startable, NewSessionOptions first_session,
           LifecycleOptions options = {});

/// A reused value is borrowed and has no owner. A created value carries its
/// owner; retain this result while using it or move the owner into your scope.
template <typename T> struct FoundOrCreated {
  T value;
  std::optional<Owned<T>> owner;
  [[nodiscard]] bool created() const noexcept { return owner.has_value(); }
};

/// These calls serialize lookup and creation within this process, including
/// calls made through separate handles. Other tmux clients can still change
/// objects. Reentry from a command observer returns overloaded; it cannot wait
/// for its own active acquisition. Sessions match an exact name; windows match a name
/// in one session; panes match `@libtmux_pane_key` in one window. Multiple matches fail
/// rather than selecting an arbitrary object. The supplied pane key must be nonempty.
[[nodiscard]] expected<FoundOrCreated<Server>, LifecycleFailure>
find_or_create_server(const Server& startable, NewSessionOptions first_session,
                      LifecycleOptions options = {});
[[nodiscard]] expected<FoundOrCreated<Session>, LifecycleFailure>
find_or_create_session(const Server& server, NewSessionOptions creation,
                       LifecycleOptions options = {});
[[nodiscard]] expected<FoundOrCreated<Window>, LifecycleFailure>
find_or_create_window(const Session& session, NewWindowOptions creation,
                      LifecycleOptions options = {});
[[nodiscard]] expected<FoundOrCreated<Pane>, LifecycleFailure>
find_or_create_pane(const Window& window, std::string_view key,
                    SplitOptions creation = {}, LifecycleOptions options = {});

/// Roots are directories containing sockets, scanned without recursion.
/// The configured roots add `$TMUX_TMPDIR/tmux-<uid>` and `/tmp/tmux-<uid>`.
/// Symlinks to sockets and roots are followed through the filesystem, never
/// lexically normalized. Duplicate roots consume the root bound; duplicate
/// socket inodes consume the entry bound but are probed only once.
struct DiscoveryOptions {
  std::vector<std::filesystem::path> roots{};
  bool configured_roots{true};
  std::size_t max_roots{16};
  std::size_t max_entries{256};
  std::size_t max_probes{64};
  std::chrono::milliseconds timeout{2000};
  std::chrono::milliseconds probe_timeout{100};
  ExecutionPolicy policy{};
  std::function<bool()> cancelled{};
};
struct DiscoveredServer {
  std::filesystem::path socket;
  std::string pid;
  std::string start_time;
};
/// Includes successful probes, duplicates, non-sockets and failed roots.
struct DiscoveryDiagnostic {
  std::filesystem::path path;
  std::string outcome;
  std::optional<CommandFailure> failure{};
};
struct DiscoveryResult {
  std::vector<DiscoveredServer> servers;
  std::vector<DiscoveryDiagnostic> diagnostics;
  bool truncated{false};
  std::size_t roots_examined{};
  std::size_t entries_examined{};
  std::size_t probes{};
};
/// No-start, bounded discovery; it cannot inventory arbitrary system paths.
/// Metadata inspection is synchronous, so a blocked filesystem call can exceed
/// the elapsed-time bound. No probe initializes ownership metadata.
[[nodiscard]] expected<DiscoveryResult, CommandFailure>
discover_servers(DiscoveryOptions options = {});

LIBTMUX_NAMESPACE_END
