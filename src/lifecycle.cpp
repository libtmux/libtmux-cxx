#include "libtmux/lifecycle.hpp"
#include "acquire.hpp"
#include "backend.hpp"
#include "libtmux/format.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
struct Ownership {
  Server server;
  OwnershipReceipt receipt;
  std::string kind;
  std::chrono::milliseconds timeout;
  CleanupReport report;
  mutable std::mutex mutex;
  std::condition_variable finished;
  std::thread::id cleaning_thread{};
};
struct LifecycleAccess {
  static const std::shared_ptr<const Backend>& backend(const Server& server) {
    return server.backend_;
  }
  static CleanupHandle cleanup(std::shared_ptr<Ownership> state) {
    return CleanupHandle{std::move(state)};
  }
  template <typename T>
  static Owned<T> owned(T value, std::shared_ptr<Ownership> state) {
    return Owned<T>{std::move(value), CleanupHandle{std::move(state)}};
  }
};
} // namespace detail
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view marker{"__libtmux_owned__|"};
constexpr std::string_view mismatch{"__libtmux_generation_changed__"};
constexpr std::string_view generation_key{"@libtmux_owner_generation"};
std::mutex coordination_mutex;
std::condition_variable coordination_changed;
std::thread::id coordinator;
class Reservation final {
public:
  Reservation() = default;
  Reservation(const Reservation&) = delete;
  Reservation& operator=(const Reservation&) = delete;
  ~Reservation() {
    {
      const std::lock_guard lock{coordination_mutex};
      coordinator = {};
    }
    coordination_changed.notify_all();
  }
};

CommandFailure failure(std::string diagnostic,
                       FailureKind kind = FailureKind::validation,
                       DeliveryStatus delivery = DeliveryStatus::not_started) {
  return {.kind = kind, .delivery = delivery, .diagnostic = std::move(diagnostic)};
}
LifecycleFailure failed(CommandFailure error, std::exception_ptr exception = {}) {
  return {.primary = std::move(error), .exception = std::move(exception)};
}
CommandFailure cancellation_failure() {
  return failure("acquisition cancelled", FailureKind::cancelled);
}

const detail::SubprocessBackend* subprocess(const Server& server) {
#if defined(_WIN32)
  static_cast<void>(server);
  return nullptr;
#else
  return dynamic_cast<const detail::SubprocessBackend*>(
      detail::LifecycleAccess::backend(server).get());
#endif
}
CommandFailure unsupported() {
  return failure("ownership requires a POSIX subprocess tmux Server",
                 FailureKind::unsupported);
}

bool decimal(std::string_view value, bool positive = true) {
  if (value.empty() ||
      !std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; })) {
    return false;
  }
  unsigned long long number{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), number);
  return parsed.ec == std::errc{} && (!positive || number != 0);
}
bool hex_token(std::string_view value) {
  return value.size() == 32 && std::ranges::all_of(value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
         });
}
bool exact_id(std::string_view value, std::string_view kind) {
  const char prefix = kind == "session" ? '$' : kind == "window" ? '@' : '%';
  return kind == "server" ? value.empty()
                          : value.size() > 1 && value.front() == prefix &&
                                decimal(value.substr(1), false);
}
expected<std::string, CommandFailure> nonce() {
  std::array<unsigned char, 16> bytes{};
  std::ifstream random{"/dev/urandom", std::ios::binary};
  if (!random.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
    return unexpected(failure("cannot read random ownership token", FailureKind::pipe));
  }
  constexpr char hex[]{"0123456789abcdef"};
  std::string result;
  result.reserve(32);
  for (const auto byte : bytes) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}

// tmux parses if-shell branches as tmux commands, not a shell command. Octal
// escapes in double quotes preserve dollars, quotes, semicolons and newlines.
std::string quote(std::string_view value) {
  std::string result{"\""};
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    result.push_back('\\');
    result.push_back(static_cast<char>('0' + ((byte >> 6) & 7)));
    result.push_back(static_cast<char>('0' + ((byte >> 3) & 7)));
    result.push_back(static_cast<char>('0' + (byte & 7)));
  }
  result.push_back('"');
  return result;
}
std::string serialise(const CommandRequest& command) {
  std::string result;
  for (const auto& argument : command.arguments()) {
    if (!result.empty())
      result.push_back(' ');
    result += quote(argument.value());
  }
  return result;
}
std::string condition(const OwnershipReceipt& receipt) {
  auto token =
      receipt.generation.empty()
          ? "#{==:#{LIBTMUX_OWNER_STARTUP_NONCE}," + receipt.startup_nonce + "}"
          : "#{==:#{@libtmux_owner_generation}," + receipt.generation + "}";
  return "#{&&:#{&&:#{==:#{pid}," + receipt.pid + "},#{==:#{start_time}," +
         receipt.start_time + "}}," + token + "}";
}
CommandRequest guarded(const OwnershipReceipt& receipt, const std::string& action) {
  CommandRequest request{"if-shell", "-F", condition(receipt)};
  request.push_back(CommandArgument::sensitive(action));
  request.emplace_back("display-message -p " + std::string{mismatch});
  return request;
}
std::string receipt_format(std::string_view kind, bool startup = false) {
  return std::string{marker} + "#{pid}|#{start_time}|#{@libtmux_owner_generation}|" +
         (kind == "server" ? "" : "#{" + std::string{kind} + "_id}") + "|" +
         (startup ? "#{LIBTMUX_OWNER_STARTUP_NONCE}" : "");
}
expected<OwnershipReceipt, CommandFailure>
parse_receipt(std::string_view output, std::string_view kind,
              std::string_view startup_nonce = {}) {
  std::optional<OwnershipReceipt> accepted;
  while (!output.empty()) {
    const auto end = output.find('\n');
    const auto line = output.substr(0, end);
    if (line.starts_with(marker)) {
      auto fields = line.substr(marker.size());
      std::array<std::string, 5> parts;
      for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto split = fields.find('|');
        parts[i] = fields.substr(0, split);
        fields = split == std::string_view::npos ? std::string_view{}
                                                 : fields.substr(split + 1);
        if ((i < 4 && split == std::string_view::npos) ||
            (i == 4 && split != std::string_view::npos)) {
          return unexpected(failure("malformed ownership receipt", FailureKind::pipe,
                                    DeliveryStatus::replied));
        }
      }
      OwnershipReceipt receipt{parts[0], parts[1], parts[2], parts[3], parts[4]};
      const bool provisional =
          !startup_nonce.empty() && receipt.startup_nonce == startup_nonce;
      if (!startup_nonce.empty() && !provisional && receipt.generation.empty() &&
          decimal(receipt.pid) && decimal(receipt.start_time, false) &&
          exact_id(receipt.id, kind)) {
        if (end == std::string_view::npos)
          break;
        output.remove_prefix(end + 1);
        continue;
      }
      if (!decimal(receipt.pid) || !decimal(receipt.start_time, false) ||
          !exact_id(receipt.id, kind) ||
          (!hex_token(receipt.generation) && !provisional)) {
        return unexpected(failure("invalid ownership id or @libtmux_owner_generation; "
                                  "expected 32 ASCII hex characters",
                                  FailureKind::pipe, DeliveryStatus::replied));
      }
      if (provisional && !hex_token(receipt.generation))
        receipt.generation.clear();
      if (accepted && (accepted->id != receipt.id || accepted->pid != receipt.pid ||
                       accepted->start_time != receipt.start_time)) {
        return unexpected(failure("conflicting ownership receipts", FailureKind::pipe,
                                  DeliveryStatus::replied));
      }
      accepted = std::move(receipt);
    }
    if (end == std::string_view::npos)
      break;
    output.remove_prefix(end + 1);
  }
  if (!accepted)
    return unexpected(failure("tmux returned no complete ownership receipt",
                              FailureKind::pipe, DeliveryStatus::indeterminate));
  return std::move(*accepted);
}

expected<void, CommandFailure> check_options(const LifecycleOptions& options) {
  if (options.timeout <= std::chrono::milliseconds::zero() ||
      options.cleanup_timeout <= std::chrono::milliseconds::zero()) {
    return unexpected(failure("acquisition and cleanup timeouts must be positive"));
  }
  if ((options.cancelled && options.cancelled()))
    return unexpected(cancellation_failure());
  return {};
}
expected<OwnershipReceipt, LifecycleFailure> accept(const Server& server,
                                                    std::string_view kind,
                                                    std::string_view id,
                                                    const LifecycleOptions& options) {
  if (auto checked = check_options(options); !checked)
    return unexpected(failed(checked.error()));
  const auto* backend = subprocess(server);
  if (!backend)
    return unexpected(failed(unsupported()));
  if (!exact_id(id, kind))
    return unexpected(failed(failure("adoption requires an exact tmux id")));
  auto token = nonce();
  if (!token)
    return unexpected(failed(token.error()));
  CommandRequest request{"set-option",      "-soq", generation_key, *token, ";",
                         "display-message", "-p"};
  if (!id.empty()) {
    request.emplace_back("-t");
    request.emplace_back(id);
  }
  request.emplace_back(receipt_format(kind));
  const auto answer =
      backend->run_receipted(request, options.timeout, options.cancelled);
  if (answer.failure)
    return unexpected(LifecycleFailure{.primary = *answer.failure,
                                       .exception = answer.observer_exception});
  auto receipt = parse_receipt(answer.output, kind);
  if (!receipt)
    return unexpected(failed(receipt.error()));
  if (receipt->id != id)
    return unexpected(failed(failure("adoption target no longer has the accepted id")));
  return *receipt;
}
std::shared_ptr<detail::Ownership> state(const Server& server, OwnershipReceipt receipt,
                                         std::string kind,
                                         std::chrono::milliseconds timeout) {
  // Allocate ownership state before sending a creation command.
  return std::shared_ptr<detail::Ownership>{new detail::Ownership{
      server, std::move(receipt), std::move(kind), timeout, {}, {}, {}, {}}};
}

expected<void, LifecycleFailure> destroy(detail::Ownership& owner) {
  const auto* backend = subprocess(owner.server);
  if (!backend)
    return unexpected(failed(unsupported()));
  CommandRequest kill{"kill-" + owner.kind};
  if (!owner.receipt.id.empty()) {
    kill.emplace_back("-t");
    kill.emplace_back(owner.receipt.id);
  }
  auto reply =
      backend->run_receipted(guarded(owner.receipt, serialise(kill)), owner.timeout);
  if (reply.failure)
    return unexpected(failed(*reply.failure, reply.observer_exception));
  if (reply.output.find(mismatch) != std::string::npos) {
    return unexpected(
        failed(failure("cleanup refused: daemon ownership generation changed",
                       FailureKind::refused, DeliveryStatus::replied)));
  }
  return {};
}
LifecycleFailure rollback(std::shared_ptr<detail::Ownership> owner,
                          CommandFailure primary, std::exception_ptr exception = {}) {
  LifecycleFailure error{.primary = std::move(primary),
                         .receipt = owner->receipt,
                         .exception = std::move(exception),
                         .cleanup = detail::LifecycleAccess::cleanup(owner)};
  auto cleaned = error.cleanup->close();
  if (!cleaned)
    error.rollback = std::move(cleaned.error());
  return error;
}

expected<void, CommandFailure>
add_environment(CommandRequest& command,
                const std::vector<std::pair<std::string, std::string>>& environment) {
  for (const auto& [key, value] : environment) {
    if (key.empty() || key.find_first_of("=\0", 0, 2) != std::string::npos ||
        value.find('\0') != std::string::npos || key == "LIBTMUX_OWNER_STARTUP_NONCE") {
      return unexpected(failure("invalid or reserved creation environment key"));
    }
    command.emplace_back("-e");
    command.push_back(CommandArgument::sensitive(key + "=" + value));
  }
  return {};
}
expected<CommandRequest, CommandFailure>
session_command(const NewSessionOptions& options, bool startup = false) {
  if (options.name.empty())
    return unexpected(failure("session name is empty"));
  CommandRequest command{"new-session",
                         "-d",
                         "-P",
                         "-F",
                         receipt_format("session", startup),
                         "-s",
                         escape_literal(options.name)};
  if (!options.first_window_name.empty()) {
    command.emplace_back("-n");
    command.push_back(escape_literal(options.first_window_name));
  }
  if (!options.start_directory.empty()) {
    command.emplace_back("-c");
    command.push_back(escape_literal(options.start_directory));
  }
  if (options.width) {
    command.emplace_back("-x");
    command.emplace_back(std::to_string(*options.width));
  }
  if (options.height) {
    command.emplace_back("-y");
    command.emplace_back(std::to_string(*options.height));
  }
  if (auto env = add_environment(command, options.environment); !env)
    return unexpected(env.error());
  if (!options.shell_command.empty()) {
    command.emplace_back("--");
    command.push_back(CommandArgument::sensitive(options.shell_command));
  }
  return command;
}
expected<CommandRequest, CommandFailure>
window_command(const Session& session, const NewWindowOptions& options) {
  if (options.name.empty())
    return unexpected(failure("window name is empty"));
  std::string target{session.id().value()};
  if (options.index)
    target += ":" + std::to_string(*options.index);
  CommandRequest command{"new-window", "-P",   "-F", receipt_format("window"),
                         "-t",         target, "-n", escape_literal(options.name)};
  if (!options.focus)
    command.emplace_back("-d");
  if (options.after_current)
    command.emplace_back("-a");
  if (!options.start_directory.empty()) {
    command.emplace_back("-c");
    command.push_back(escape_literal(options.start_directory));
  }
  if (auto env = add_environment(command, options.environment); !env)
    return unexpected(env.error());
  if (!options.shell_command.empty()) {
    command.emplace_back("--");
    command.push_back(CommandArgument::sensitive(options.shell_command));
  }
  return command;
}
expected<CommandRequest, CommandFailure> pane_command(const Pane& target,
                                                      const SplitOptions& options) {
  if (options.percentage && (*options.percentage < 1 || *options.percentage > 100)) {
    return unexpected(failure("a percentage of the window is between 1 and 100"));
  }
  CommandRequest command{"split-window",         "-P", "-F",
                         receipt_format("pane"), "-t", target.id().value()};
  if (!options.focus)
    command.emplace_back("-d");
  command.emplace_back(options.horizontal ? "-h" : "-v");
  if (options.before)
    command.emplace_back("-b");
  if (options.full_size)
    command.emplace_back("-f");
  if (options.percentage) {
    command.emplace_back("-l");
    command.emplace_back(std::to_string(*options.percentage) + "%");
  }
  if (!options.start_directory.empty()) {
    command.emplace_back("-c");
    command.push_back(escape_literal(options.start_directory));
  }
  if (auto env = add_environment(command, options.environment); !env)
    return unexpected(env.error());
  if (!options.shell_command.empty()) {
    command.emplace_back("--");
    command.push_back(CommandArgument::sensitive(options.shell_command));
  }
  return command;
}

expected<void, CommandFailure> spend(LifecycleOptions& options,
                                     Clock::time_point deadline) {
  if (options.cancelled && options.cancelled())
    return unexpected(cancellation_failure());
  options.timeout =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  if (options.timeout <= std::chrono::milliseconds::zero()) {
    return unexpected(failure("acquisition timed out", FailureKind::timeout));
  }
  return {};
}

template <typename T>
expected<std::vector<T>, LifecycleFailure>
read_entities(const Server& server, CommandRequest command,
              const LifecycleOptions& options, bool one = false) {
  const auto* backend = subprocess(server);
  if (!backend)
    return unexpected(failed(unsupported()));
  if (!one)
    command.emplace_back("-F");
  command.emplace_back(format_request(T::kFields));
  auto answer = backend->run_receipted(command, options.timeout, options.cancelled);
  if (answer.failure)
    return unexpected(failed(*answer.failure, answer.observer_exception));
  auto snapshot = detail::SnapshotFactory::from_output(
      detail::LifecycleAccess::backend(server), T::kFields, std::move(answer.output));
  if (!snapshot)
    return unexpected(failed(snapshot.error()));
  if (auto malformed = detail::malformed_number<T>(**snapshot))
    return unexpected(failed(*malformed));
  std::vector<T> entities;
  for (std::size_t index = 0; index < (*snapshot)->rows().size(); ++index) {
    entities.emplace_back(*snapshot, index);
  }
  return entities;
}
template <typename T>
expected<T, LifecycleFailure> lookup(const Server& server, std::string_view id,
                                     const LifecycleOptions& options) {
  auto entities =
      read_entities<T>(server, {"display-message", "-p", "-t", id}, options, true);
  if (!entities)
    return unexpected(entities.error());
  if (entities->size() != 1 || entities->front().id().value() != id) {
    return unexpected(
        failed(failure("created object disappeared before ownership handoff",
                       FailureKind::missing, DeliveryStatus::replied)));
  }
  return entities->front();
}
template <typename T>
expected<Owned<T>, LifecycleFailure>
create(const Server& server, expected<CommandRequest, CommandFailure> command,
       std::string_view kind, LifecycleOptions options) {
  const auto deadline = Clock::now() + options.timeout;
  if (!command)
    return unexpected(failed(command.error()));
  auto identity = accept(server, "server", "", options);
  if (!identity)
    return unexpected(identity.error());
  if (auto left = spend(options, deadline); !left)
    return unexpected(failed(left.error()));
  auto owner = state(server, *identity, std::string{kind}, options.cleanup_timeout);
  auto answer = subprocess(server)->run_receipted(
      guarded(*identity, serialise(*command)), options.timeout, options.cancelled);
  auto receipt = parse_receipt(answer.output, kind);
  if (!receipt) {
    auto error = failed(answer.failure.value_or(receipt.error()));
    error.uncertain = error.primary.delivery != DeliveryStatus::not_started;
    error.exception = answer.observer_exception;
    return unexpected(std::move(error));
  }
  owner->receipt = *receipt;
  if (receipt->pid != identity->pid || receipt->start_time != identity->start_time ||
      receipt->generation != identity->generation) {
    return unexpected(LifecycleFailure{
        .primary = failure("creation receipt changed daemon identity",
                           FailureKind::pipe, DeliveryStatus::indeterminate),
        .receipt = *receipt,
        .uncertain = true,
        .exception = answer.observer_exception,
        .command_failure = std::move(answer.failure)});
  }
  if (answer.failure)
    return unexpected(rollback(owner, *answer.failure, answer.observer_exception));
  if ((options.cancelled && options.cancelled()))
    return unexpected(rollback(owner, cancellation_failure()));
  try {
    if (auto left = spend(options, deadline); !left)
      return unexpected(rollback(owner, left.error()));
    auto entity = lookup<T>(server, receipt->id, options);
    if (!entity)
      return unexpected(
          rollback(owner, entity.error().primary, entity.error().exception));
    if ((options.cancelled && options.cancelled()))
      return unexpected(rollback(owner, cancellation_failure()));
    return detail::LifecycleAccess::owned(std::move(*entity), std::move(owner));
  } catch (...) {
    return unexpected(
        rollback(owner,
                 failure("snapshot or ownership handoff threw after creation",
                         FailureKind::pipe, DeliveryStatus::replied),
                 std::current_exception()));
  }
}
template <typename T>
expected<Owned<T>, LifecycleFailure>
adopt_entity(const T& entity, std::string_view kind, const LifecycleOptions& options) {
  auto server = entity.server();
  if (!server)
    return unexpected(failed(server.error()));
  auto receipt = accept(*server, kind, entity.id().value(), options);
  if (!receipt)
    return unexpected(receipt.error());
  return detail::LifecycleAccess::owned(
      entity, state(*server, *receipt, std::string{kind}, options.cleanup_timeout));
}
expected<std::unique_ptr<Reservation>, LifecycleFailure>
coordinate(LifecycleOptions& options) {
  if (auto checked = check_options(options); !checked)
    return unexpected(failed(checked.error()));
  const auto deadline = Clock::now() + options.timeout;
  std::unique_lock lock{coordination_mutex};
  if (coordinator == std::this_thread::get_id()) {
    return unexpected(failed(failure(
        "find-or-create cannot reenter its active observer", FailureKind::overloaded)));
  }
  while (coordinator != std::thread::id{}) {
    coordination_changed.wait_for(lock, std::chrono::milliseconds{5});
    if (auto left = spend(options, deadline); !left)
      return unexpected(failed(left.error()));
  }
  if (auto left = spend(options, deadline); !left)
    return unexpected(failed(left.error()));
  auto reservation = std::make_unique<Reservation>();
  coordinator = std::this_thread::get_id();
  return reservation;
}
expected<Server, CommandFailure> reopened(const Server& server) {
  const auto& backend = detail::LifecycleAccess::backend(server);
  return Server::at_socket_path(server.socket_path(),
                                backend->command_observer().value_or(CommandObserver{}),
                                backend->policy());
}
template <typename T> FoundOrCreated<T> created_result(Owned<T> owner) {
  T value = owner.get();
  return {std::move(value), std::move(owner)};
}
} // namespace

CleanupHandle::CleanupHandle(std::shared_ptr<detail::Ownership> state) noexcept
    : state_{std::move(state)} {}
expected<void, CommandFailure> CleanupHandle::close() const {
  if (!state_)
    return {};
  std::unique_lock lock{state_->mutex};
  if (state_->cleaning_thread == std::this_thread::get_id()) {
    return unexpected(
        failure("cleanup cannot reenter its active observer", FailureKind::overloaded));
  }
  state_->finished.wait(lock, [&] { return !state_->report.in_progress; });
  if (state_->report.complete)
    return {};
  ++state_->report.attempts;
  state_->report.exception = {};
  state_->report.in_progress = true;
  state_->cleaning_thread = std::this_thread::get_id();
  lock.unlock();
  expected<void, CommandFailure> result;
  std::exception_ptr exception;
  std::exception_ptr raised;
  try {
    auto destroyed = destroy(*state_);
    if (!destroyed) {
      exception = destroyed.error().exception;
      result = unexpected(std::move(destroyed.error().primary));
    }
  } catch (...) {
    exception = std::current_exception();
    raised = exception;
  }
  lock.lock();
  state_->report.exception = exception;
  state_->report.in_progress = false;
  state_->cleaning_thread = {};
  if (!result)
    state_->report.failure = result.error();
  else if (!exception) {
    state_->report.failure.reset();
    state_->report.complete = true;
  }
  lock.unlock();
  state_->finished.notify_all();
  if (raised)
    std::rethrow_exception(raised);
  return result;
}
void CleanupHandle::close_noexcept() const noexcept {
  try {
    static_cast<void>(close());
  } catch (...) {
  }
}
CleanupReport CleanupHandle::report() const {
  if (!state_)
    return {.complete = true};
  const std::lock_guard lock{state_->mutex};
  return state_->report;
}
OwnershipReceipt CleanupHandle::receipt() const {
  if (!state_)
    return {};
  const std::lock_guard lock{state_->mutex};
  return state_->receipt;
}
expected<Owned<Server>, LifecycleFailure> adopt(const Server& server,
                                                LifecycleOptions options) {
  auto receipt = accept(server, "server", "", options);
  if (!receipt)
    return unexpected(receipt.error());
  return detail::LifecycleAccess::owned(
      server, state(server, *receipt, "server", options.cleanup_timeout));
}
expected<Owned<Session>, LifecycleFailure> adopt(const Session& session,
                                                 LifecycleOptions options) {
  return adopt_entity(session, "session", options);
}
expected<Owned<Window>, LifecycleFailure> adopt(const Window& window,
                                                LifecycleOptions options) {
  return adopt_entity(window, "window", options);
}
expected<Owned<Pane>, LifecycleFailure> adopt(const Pane& pane,
                                              LifecycleOptions options) {
  return adopt_entity(pane, "pane", options);
}
expected<Owned<Session>, LifecycleFailure> own_session(const Server& server,
                                                       NewSessionOptions creation,
                                                       LifecycleOptions options) {
  return create<Session>(server, session_command(creation), "session", options);
}
expected<Owned<Window>, LifecycleFailure> own_window(const Session& session,
                                                     NewWindowOptions creation,
                                                     LifecycleOptions options) {
  auto server = session.server();
  if (!server)
    return unexpected(failed(server.error()));
  return create<Window>(*server, window_command(session, creation), "window", options);
}
expected<Owned<Pane>, LifecycleFailure>
own_pane(const Pane& target, SplitOptions creation, LifecycleOptions options) {
  auto server = target.server();
  if (!server)
    return unexpected(failed(server.error()));
  return create<Pane>(*server, pane_command(target, creation), "pane", options);
}

expected<Owned<Server>, LifecycleFailure> own_server(const Server& startable,
                                                     NewSessionOptions first_session,
                                                     LifecycleOptions options) {
  if (auto checked = check_options(options); !checked)
    return unexpected(failed(checked.error()));
  const auto* backend = subprocess(startable);
  if (!backend)
    return unexpected(failed(unsupported()));
  auto fresh = reopened(startable);
  if (!fresh)
    return unexpected(failed(fresh.error()));
  if (!detail::LifecycleAccess::backend(*fresh)
           ->allows_cold_socket_version_fallback()) {
    return unexpected(
        failed(failure("owned server startup requires an absent endpoint")));
  }
  auto token = nonce();
  if (!token)
    return unexpected(failed(token.error()));
  auto command = session_command(first_session, true);
  if (!command)
    return unexpected(failed(command.error()));
  command->emplace_back(";");
  command->emplace_back("set-option");
  command->emplace_back("-soq");
  command->emplace_back(generation_key);
  command->emplace_back(*token);
  command->emplace_back(";");
  command->emplace_back("display-message");
  command->emplace_back("-p");
  command->emplace_back(receipt_format("session", true));
  auto owner = state(startable, {}, "server", options.cleanup_timeout);
  auto answer =
      backend->run_receipted(*command, options.timeout, options.cancelled, *token);
  auto receipt = parse_receipt(answer.output, "session", *token);
  if (!receipt) {
    auto error = failed(answer.failure.value_or(receipt.error()));
    error.uncertain = error.primary.delivery != DeliveryStatus::not_started;
    error.exception = answer.observer_exception;
    return unexpected(std::move(error));
  }
  owner->receipt = *receipt;
  auto retained = reopened(startable);
  if (retained)
    owner->server = *retained;
  const bool started = receipt->startup_nonce == *token;
  if (!started) {
    owner->kind = "session";
    auto error = rollback(
        owner,
        failure(
            "another client started the server; rolled back only this call's session",
            FailureKind::refused, DeliveryStatus::replied),
        answer.observer_exception);
    error.command_failure = std::move(answer.failure);
    return unexpected(std::move(error));
  }
  owner->receipt.id.clear();
  if (answer.failure)
    return unexpected(rollback(owner, *answer.failure, answer.observer_exception));
  if (!hex_token(receipt->generation))
    return unexpected(
        rollback(owner, failure("startup did not install a valid ownership generation",
                                FailureKind::pipe, DeliveryStatus::replied)));
  if (!retained)
    return unexpected(rollback(owner, retained.error()));
  if ((options.cancelled && options.cancelled()))
    return unexpected(rollback(owner, cancellation_failure()));
  Server value = owner->server;
  return detail::LifecycleAccess::owned(std::move(value), std::move(owner));
}

expected<FoundOrCreated<Server>, LifecycleFailure>
find_or_create_server(const Server& startable, NewSessionOptions first_session,
                      LifecycleOptions options) {
  auto lock = coordinate(options);
  if (!lock)
    return unexpected(lock.error());
  if (!subprocess(startable))
    return unexpected(failed(unsupported()));
  auto fresh = reopened(startable);
  if (!fresh)
    return unexpected(failed(fresh.error()));
  const auto& backend = detail::LifecycleAccess::backend(*fresh);
  if (!backend->allows_cold_socket_version_fallback()) {
    auto checked = subprocess(*fresh)->run_receipted(
        {"display-message", "-p", "#{pid}"}, options.timeout, options.cancelled);
    if (checked.failure)
      return unexpected(failed(*checked.failure, checked.observer_exception));
    return FoundOrCreated<Server>{*fresh, std::nullopt};
  }
  auto owner = own_server(startable, std::move(first_session), options);
  if (!owner)
    return unexpected(owner.error());
  return created_result(std::move(*owner));
}
expected<FoundOrCreated<Session>, LifecycleFailure>
find_or_create_session(const Server& server, NewSessionOptions creation,
                       LifecycleOptions options) {
  auto lock = coordinate(options);
  if (!lock)
    return unexpected(lock.error());
  const auto deadline = Clock::now() + options.timeout;
  auto sessions = read_entities<Session>(server, {"list-sessions"}, options);
  if (!sessions)
    return unexpected(sessions.error());
  std::optional<Session> match;
  for (const auto& session : *sessions) {
    if (session.name() != creation.name)
      continue;
    if (match)
      return unexpected(failed(failure("session name matched more than one session")));
    match = session;
  }
  if (match)
    return FoundOrCreated<Session>{*match, std::nullopt};
  if (auto left = spend(options, deadline); !left)
    return unexpected(failed(left.error()));
  auto owner = own_session(server, std::move(creation), options);
  if (!owner)
    return unexpected(owner.error());
  return created_result(std::move(*owner));
}
expected<FoundOrCreated<Window>, LifecycleFailure>
find_or_create_window(const Session& session, NewWindowOptions creation,
                      LifecycleOptions options) {
  auto lock = coordinate(options);
  if (!lock)
    return unexpected(lock.error());
  const auto deadline = Clock::now() + options.timeout;
  auto server = session.server();
  if (!server)
    return unexpected(failed(server.error()));
  auto windows = read_entities<Window>(
      *server, {"list-windows", "-t", session.id().value()}, options);
  if (!windows)
    return unexpected(windows.error());
  std::optional<Window> match;
  for (const auto& window : *windows) {
    if (window.name() != creation.name)
      continue;
    if (match)
      return unexpected(failed(failure("window name matched more than one window")));
    match = window;
  }
  if (match)
    return FoundOrCreated<Window>{*match, std::nullopt};
  if (auto left = spend(options, deadline); !left)
    return unexpected(failed(left.error()));
  auto owner = own_window(session, std::move(creation), options);
  if (!owner)
    return unexpected(owner.error());
  return created_result(std::move(*owner));
}
expected<FoundOrCreated<Pane>, LifecycleFailure>
find_or_create_pane(const Window& window, std::string_view key, SplitOptions creation,
                    LifecycleOptions options) {
  if (key.empty() || key.find('\0') != std::string_view::npos)
    return unexpected(failed(failure("pane key must be nonempty and contain no NUL")));
  auto lock = coordinate(options);
  if (!lock)
    return unexpected(lock.error());
  const auto deadline = Clock::now() + options.timeout;
  auto server = window.server();
  if (!server)
    return unexpected(failed(server.error()));
  if (!subprocess(*server))
    return unexpected(failed(unsupported()));
  auto panes =
      read_entities<Pane>(*server, {"list-panes", "-t", window.id().value()}, options);
  if (!panes)
    return unexpected(panes.error());
  std::optional<Pane> match;
  for (const auto& pane : *panes) {
    if (auto left = spend(options, deadline); !left)
      return unexpected(failed(left.error()));
    auto value = subprocess(*server)->run_receipted(
        {"display-message", "-p", "-t", pane.id().value(), "#{@libtmux_pane_key}"},
        options.timeout, options.cancelled);
    if (value.failure)
      return unexpected(failed(*value.failure, value.observer_exception));
    if (detail::without_trailing_newline(value.output) != key)
      continue;
    if (match)
      return unexpected(failed(failure("pane key matched more than one pane")));
    match = pane;
  }
  if (match)
    return FoundOrCreated<Pane>{*match, std::nullopt};
  if (panes->empty())
    return unexpected(failed(failure("cannot split a window without a pane")));
  if (auto left = spend(options, deadline); !left)
    return unexpected(failed(left.error()));
  auto owner = own_pane(panes->front(), std::move(creation), options);
  if (!owner)
    return unexpected(owner.error());
  if (auto left = spend(options, deadline); !left) {
    LifecycleFailure error{.primary = left.error(),
                           .receipt = owner->cleanup().receipt(),
                           .cleanup = owner->cleanup()};
    if (auto closed = owner->close(); !closed)
      error.rollback = closed.error();
    return unexpected(std::move(error));
  }
  CommandRequest mark{"set-option",        "-p", "-t", (*owner)->id().value(),
                      "@libtmux_pane_key", key};
  auto answer = subprocess(*server)->run_receipted(
      guarded(owner->cleanup().receipt(), serialise(mark)), options.timeout,
      options.cancelled);
  if (answer.failure || answer.output.find(mismatch) != std::string::npos ||
      (options.cancelled && options.cancelled())) {
    LifecycleFailure error{.primary = answer.failure.value_or(
                               (options.cancelled && options.cancelled())
                                   ? cancellation_failure()
                                   : failure("pane identity changed before marking")),
                           .receipt = owner->cleanup().receipt(),
                           .exception = answer.observer_exception,
                           .cleanup = owner->cleanup()};
    auto closed = owner->close();
    if (!closed)
      error.rollback = closed.error();
    return unexpected(std::move(error));
  }
  return created_result(std::move(*owner));
}
LIBTMUX_NAMESPACE_END
