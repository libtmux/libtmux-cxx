#include "libtmux/control.hpp"

#include "libtmux/expected.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

LIBTMUX_NAMESPACE_BEGIN
namespace {

using Clock = std::chrono::steady_clock;

// What the waiter asks `waitid` for.
//
// Only exits, which is all it wants. Some platforms report a stop anyway, and
// `LIBTMUX_SIMULATE_STOP_REPORTING_WAITID` asks for stops explicitly so that
// behaviour can be reproduced on a platform that does not have it. The deadlock
// this guards against was found that way and stays testable that way.
#if defined(LIBTMUX_SIMULATE_STOP_REPORTING_WAITID)
inline constexpr int kChildWaitOptions = WEXITED | WSTOPPED | WNOWAIT;
#else
inline constexpr int kChildWaitOptions = WEXITED | WNOWAIT;
#endif

void close_fd(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

ProtocolError system_error(std::string_view operation, int error_number) {
  return ProtocolError{std::string{operation} + ": " + std::strerror(error_number)};
}

expected<std::array<int, 2>, ProtocolError> make_pipe() {
  std::array<int, 2> descriptors{-1, -1};
#if defined(__linux__) && !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  if (::pipe2(descriptors.data(), O_CLOEXEC) != 0) {
    return unexpected(system_error("pipe2", errno));
  }
#else
  if (::pipe(descriptors.data()) != 0) {
    return unexpected(system_error("pipe", errno));
  }
  for (const auto descriptor : descriptors) {
    const auto flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
      const auto saved_errno = errno;
      auto read_descriptor = descriptors[0];
      auto write_descriptor = descriptors[1];
      close_fd(read_descriptor);
      close_fd(write_descriptor);
      return unexpected(system_error("fcntl(FD_CLOEXEC)", saved_errno));
    }
  }
#endif
  return descriptors;
}

expected<void, ProtocolError> make_nonblocking(int descriptor) {
  const auto flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    return unexpected(system_error("fcntl(O_NONBLOCK)", errno));
  }
  return {};
}

bool contains_nul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

std::string encode_token(std::string_view value) {
  if (value.empty()) {
    return "''";
  }
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

expected<std::string, ProtocolError> render_request(ControlRequest&& request) {
  if (request.group.empty()) {
    return unexpected(ProtocolError{"control request group is empty"});
  }
  std::string line;
  for (std::size_t operation_index = 0; operation_index < request.group.size();
       ++operation_index) {
    const auto& operation = request.group[operation_index];
    if (operation.argv.empty()) {
      return unexpected(ProtocolError{"control request operation has no command"});
    }
    if (operation_index != 0U) {
      line.append(" ; ");
    }
    for (std::size_t argument_index = 0; argument_index < operation.argv.size();
         ++argument_index) {
      const auto& argument = operation.argv[argument_index];
      if (contains_nul(argument)) {
        return unexpected(ProtocolError{"control request argument contains NUL"});
      }
      if (argument_index != 0U) {
        line.push_back(' ');
      }
      line.append(encode_token(argument));
    }
  }
  line.push_back('\n');
  return line;
}

std::vector<std::string> sanitized_environment() {
  std::vector<std::string> values;
  for (auto** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view value{*entry};
    if (value.starts_with("TMUX=") || value.starts_with("TMUX_PANE=")) {
      continue;
    }
    values.emplace_back(value);
  }
  return values;
}

std::vector<char*> writable_pointers(std::vector<std::string>& values) {
  std::vector<char*> pointers;
  pointers.reserve(values.size() + 1U);
  for (auto& value : values) {
    pointers.push_back(value.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

struct SpawnedClient {
  pid_t pid{-1};
  int input{-1};
  int output{-1};
  int error{-1};
};

expected<SpawnedClient, ProtocolError> spawn_client(const ConnectionOptions& options) {
  auto input_pipe = make_pipe();
  if (!input_pipe) {
    return unexpected(input_pipe.error());
  }
  auto output_pipe = make_pipe();
  if (!output_pipe) {
    close_fd((*input_pipe)[0]);
    close_fd((*input_pipe)[1]);
    return unexpected(output_pipe.error());
  }
  auto error_pipe = make_pipe();
  if (!error_pipe) {
    close_fd((*input_pipe)[0]);
    close_fd((*input_pipe)[1]);
    close_fd((*output_pipe)[0]);
    close_fd((*output_pipe)[1]);
    return unexpected(error_pipe.error());
  }

  const auto close_all = [&] {
    close_fd((*input_pipe)[0]);
    close_fd((*input_pipe)[1]);
    close_fd((*output_pipe)[0]);
    close_fd((*output_pipe)[1]);
    close_fd((*error_pipe)[0]);
    close_fd((*error_pipe)[1]);
  };

  posix_spawn_file_actions_t actions;
  auto result = ::posix_spawn_file_actions_init(&actions);
  if (result != 0) {
    close_all();
    return unexpected(system_error("posix_spawn_file_actions_init", result));
  }
  const auto fail_actions =
      [&](std::string_view operation,
          int error_number) -> expected<SpawnedClient, ProtocolError> {
    static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
    close_all();
    return unexpected(system_error(operation, error_number));
  };
  result = ::posix_spawn_file_actions_adddup2(&actions, (*input_pipe)[0], STDIN_FILENO);
  if (result != 0) {
    return fail_actions("posix_spawn_file_actions_adddup2(stdin)", result);
  }
  result =
      ::posix_spawn_file_actions_adddup2(&actions, (*output_pipe)[1], STDOUT_FILENO);
  if (result != 0) {
    return fail_actions("posix_spawn_file_actions_adddup2(stdout)", result);
  }
  result =
      ::posix_spawn_file_actions_adddup2(&actions, (*error_pipe)[1], STDERR_FILENO);
  if (result != 0) {
    return fail_actions("posix_spawn_file_actions_adddup2(stderr)", result);
  }
#if defined(__GLIBC__) && defined(__USE_MISC)
  result = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  if (result != 0) {
    return fail_actions("posix_spawn_file_actions_addclosefrom_np", result);
  }
#else
  for (const auto descriptor :
       {(*input_pipe)[0], (*input_pipe)[1], (*output_pipe)[0], (*output_pipe)[1],
        (*error_pipe)[0], (*error_pipe)[1]}) {
    result = ::posix_spawn_file_actions_addclose(&actions, descriptor);
    if (result != 0) {
      return fail_actions("posix_spawn_file_actions_addclose", result);
    }
  }
#endif

  posix_spawnattr_t attributes;
  result = ::posix_spawnattr_init(&attributes);
  if (result != 0) {
    return fail_actions("posix_spawnattr_init", result);
  }
  const auto fail_attributes =
      [&](std::string_view operation,
          int error_number) -> expected<SpawnedClient, ProtocolError> {
    static_cast<void>(::posix_spawnattr_destroy(&attributes));
    return fail_actions(operation, error_number);
  };
  result = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (result != 0) {
    return fail_attributes("posix_spawnattr_setflags", result);
  }
  result = ::posix_spawnattr_setpgroup(&attributes, 0);
  if (result != 0) {
    return fail_attributes("posix_spawnattr_setpgroup", result);
  }

  std::vector<std::string> arguments{options.tmux_binary.string(),
                                     "-N",
                                     "-u",
                                     "-S",
                                     options.socket_path.string(),
                                     "-C",
                                     "attach-session",
                                     "-f",
                                     "no-output",
                                     "-t",
                                     "=" + options.session_name};
  auto argument_pointers = writable_pointers(arguments);
  auto environment = sanitized_environment();
  auto environment_pointers = writable_pointers(environment);
  pid_t pid = -1;
  result = ::posix_spawnp(&pid, arguments.front().c_str(), &actions, &attributes,
                          argument_pointers.data(), environment_pointers.data());
  const auto destroy_attributes = ::posix_spawnattr_destroy(&attributes);
  const auto destroy_actions = ::posix_spawn_file_actions_destroy(&actions);
  if (result != 0) {
    close_all();
    return unexpected(system_error("posix_spawnp", result));
  }
  if (destroy_attributes != 0 || destroy_actions != 0) {
    static_cast<void>(::kill(-pid, SIGKILL));
    static_cast<void>(::waitpid(pid, nullptr, 0));
    close_all();
    return unexpected(ProtocolError{"posix_spawn cleanup failed"});
  }

  close_fd((*input_pipe)[0]);
  close_fd((*output_pipe)[1]);
  close_fd((*error_pipe)[1]);
  for (const auto descriptor :
       {(*input_pipe)[1], (*output_pipe)[0], (*error_pipe)[0]}) {
    auto nonblocking = make_nonblocking(descriptor);
    if (!nonblocking) {
      static_cast<void>(::kill(-pid, SIGKILL));
      static_cast<void>(::waitpid(pid, nullptr, 0));
      close_fd((*input_pipe)[1]);
      close_fd((*output_pipe)[0]);
      close_fd((*error_pipe)[0]);
      return unexpected(nonblocking.error());
    }
  }
  return SpawnedClient{.pid = pid,
                       .input = (*input_pipe)[1],
                       .output = (*output_pipe)[0],
                       .error = (*error_pipe)[0]};
}

std::optional<std::uint64_t> guard_flags(const ControlBlock& block) {
  const auto raw = std::span<const std::byte>{block.begin_metadata};
  const std::string_view metadata{reinterpret_cast<const char*>(raw.data()),
                                  raw.size()};
  const auto last_space = metadata.rfind(' ');
  if (last_space == std::string_view::npos || last_space + 1U == metadata.size()) {
    return std::nullopt;
  }
  std::uint64_t flags = 0;
  const auto field = metadata.substr(last_space + 1U);
  const auto converted =
      std::from_chars(field.data(), field.data() + field.size(), flags);
  if (converted.ec != std::errc{} || converted.ptr != field.data() + field.size()) {
    return std::nullopt;
  }
  return flags;
}

std::string notification_text(const Notification& notification) {
  const auto raw = std::span<const std::byte>{notification.body};
  return {reinterpret_cast<const char*>(raw.data()), raw.size()};
}

// Take the SIGPIPE this thread just generated off the pending queue, so that
// unblocking the mask does not deliver it to a process that never asked for it.
//
// The caller has established the exact situation this is safe in: `write`
// answered EPIPE, and SIGPIPE was not pending beforehand. A SIGPIPE raised by
// `write` is directed at the calling thread, so it is queued for this thread
// and no other thread can consume it first.
void drain_pending_sigpipe(const sigset_t& blocked) {
#if defined(__linux__) && !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  const timespec no_wait{.tv_sec = 0, .tv_nsec = 0};
  for (;;) {
    const auto signal = ::sigtimedwait(&blocked, nullptr, &no_wait);
    if (signal == SIGPIPE || (signal < 0 && errno == EAGAIN)) {
      return;
    }
    if (signal < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
#else
  // macOS has no `sigtimedwait`, and the nearest call — `sigwait` — blocks
  // forever when nothing is queued. The caller's reasoning says something is,
  // but "should be queued" is not a safe basis for an unbounded wait on the
  // writer thread: a platform that answers EPIPE without raising SIGPIPE would
  // hang the connection rather than report anything.
  //
  // So ask first and only then take it. `sigpending` is cheap, and after it
  // says SIGPIPE is there, `sigwait` returns without waiting.
  for (;;) {
    sigset_t queued{};
    if (sigpending(&queued) != 0 || sigismember(&queued, SIGPIPE) != 1) {
      return;
    }
    int signal = 0;
    // POSIX has `sigwait` return the error number; macOS returns -1 and sets
    // errno. Zero means it took one either way, and both spellings of EINTR
    // are worth retrying.
    const auto result = ::sigwait(&blocked, &signal);
    if (result == 0) {
      return;
    }
    if (result != EINTR && errno != EINTR) {
      return;
    }
  }
#endif
}

expected<ssize_t, ProtocolError> write_without_sigpipe(int descriptor, const void* data,
                                                       std::size_t size) {
  sigset_t blocked{};
  sigset_t previous{};
  sigset_t pending{};
  // Unqualified: these four are functions on Linux but macros on macOS, and a
  // macro will not take a `::`.
  if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGPIPE) != 0) {
    return unexpected(system_error("prepare SIGPIPE mask", errno));
  }
  const auto mask_result = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
  if (mask_result != 0) {
    return unexpected(system_error("pthread_sigmask(SIG_BLOCK)", mask_result));
  }
  if (sigpending(&pending) != 0) {
    const auto saved_errno = errno;
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
    return unexpected(system_error("sigpending", saved_errno));
  }
  const auto already_pending = sigismember(&pending, SIGPIPE) == 1;

  const auto count = ::write(descriptor, data, size);
  const auto write_errno = errno;
  if (count < 0 && write_errno == EPIPE && !already_pending) {
    drain_pending_sigpipe(blocked);
  }
  const auto restore_result = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  if (restore_result != 0) {
    return unexpected(system_error("pthread_sigmask(SIG_SETMASK)", restore_result));
  }
  errno = write_errno;
  return count;
}

} // namespace

struct Connection::State {
  struct PendingRequest {
    explicit PendingRequest(std::size_t operation_count) {
      result.operations.resize(operation_count);
    }

    ControlRequestResult result;
    std::size_t next_operation{0U};
    bool complete{false};
  };

  // `parser` reads `options`, which is why it is spelled out here rather than
  // left to a default member initialiser: this order is the one the compiler
  // checks, and a reordered declaration is a warning instead of a decoder that
  // quietly took the defaults.
  State(ConnectionOptions requested_options, SpawnedClient client)
      : options(std::move(requested_options)), pid(client.pid), input(client.input),
        output(client.output), error(client.error),
        parser(options.retained_reply_bytes, options.line_bytes) {}

  ~State() noexcept {
    // Kill first. Closing the input needs the writer's mutex, and a writer
    // blocked against a child that is not draining holds it for as long as its
    // own deadline allows — so waiting for the mutex before killing made the
    // teardown wait on the very thing the kill unsticks.
    const auto waiter_started = waiter.joinable();
    {
      std::lock_guard lock{mutex};
      signal_child_locked(SIGKILL);
    }
    {
      std::lock_guard write_lock{write_mutex};
      close_fd(input);
    }
    if (reader.joinable()) {
      reader.join();
    } else {
      close_fd(output);
      close_fd(error);
    }
    if (waiter.joinable()) {
      waiter.join();
    }
    if (!waiter_started) {
      int status = 0;
      pid_t waited = -1;
      do {
        waited = ::waitpid(pid, &status, 0);
      } while (waited < 0 && errno == EINTR);
      std::lock_guard lock{mutex};
      child_exited = waited == pid || (waited < 0 && errno == ECHILD);
      reaped = child_exited;
      if (waited == pid) {
        wait_status = status;
      }
    }
  }

  State(const State&) = delete;
  State& operator=(const State&) = delete;

  void signal_child_locked(int signal_number) const noexcept {
    // Gated on `reaped` alone. Until the child is reaped its pid cannot be
    // reused, so signalling is safe; after it is reaped the number could name
    // somebody else, so it is not. `child_exited` used to gate this too, which
    // meant a wrong belief that the child had exited disabled the signal that
    // would have made it true — and signalling a process that really has
    // exited is a harmless no-op, so there was nothing to buy.
    if (pid > 0 && !reaped) {
      static_cast<void>(::kill(-pid, signal_number));
    }
  }

  void fail_locked(ProtocolError failure) {
    if (!fatal_error) {
      fatal_error = std::move(failure);
    }
    for (auto& pending_request : pending) {
      if (!pending_request->result.connection_error) {
        pending_request->result.connection_error = fatal_error.value();
      }
      pending_request->complete = true;
    }
    pending.clear();
    condition.notify_all();
  }

  void accept_event(Event event) {
    std::lock_guard lock{mutex};
    if (auto* notification = std::get_if<Notification>(&event)) {
      const auto text = notification_text(*notification);
      if (text == "%exit" || text.starts_with("%exit ")) {
        saw_exit = true;
      }
      // Bounded. A caller that never drains — and a subprocess-shaped caller
      // never will — would otherwise grow this for the life of the
      // connection, one entry per mutating command. The oldest go first,
      // because what a watcher wants is what just happened.
      constexpr std::size_t maximum_notifications = 4096U;
      if (notifications.size() >= maximum_notifications) {
        notifications.erase(notifications.begin());
        ++notifications_dropped;
      }
      notifications.push_back(std::move(*notification));
      condition.notify_all();
      return;
    }

    auto block = std::move(std::get<ControlBlock>(event));
    const auto flags = guard_flags(block);
    if (!flags) {
      fail_locked(ProtocolError{"control block has invalid guard flags"});
      return;
    }
    if (!startup_seen) {
      startup_seen = true;
      if (*flags != 0U || block.terminal != ControlTerminal::end) {
        fail_locked(ProtocolError{"control client attach failed"});
        return;
      }
      ready = true;
      condition.notify_all();
      return;
    }
    if (*flags == 0U) {
      return;
    }
    if (*flags != 1U) {
      fail_locked(ProtocolError{"control block has unsupported guard flags"});
      return;
    }
    if (pending.empty()) {
      if (!closing && !fatal_error) {
        fail_locked(ProtocolError{"control block has no pending request"});
      }
      return;
    }

    auto pending_request = pending.front();
    if (pending_request->next_operation >= pending_request->result.operations.size()) {
      fail_locked(ProtocolError{"control request received too many blocks"});
      return;
    }
    auto& operation =
        pending_request->result.operations[pending_request->next_operation];
    operation.attribution = Attribution::exact;
    operation.block = std::move(block);
    ++pending_request->next_operation;
    if (operation.block->terminal == ControlTerminal::error) {
      for (auto index = pending_request->next_operation;
           index < pending_request->result.operations.size(); ++index) {
        pending_request->result.operations[index].attribution = Attribution::skipped;
      }
      pending_request->next_operation = pending_request->result.operations.size();
    }
    if (pending_request->next_operation == pending_request->result.operations.size()) {
      pending_request->complete = true;
      pending.pop_front();
      condition.notify_all();
    }
  }

  void read_output() {
    std::array<std::byte, 4096> buffer{};
    for (;;) {
      const auto count = ::read(output, buffer.data(), buffer.size());
      if (count > 0) {
        auto parsed = parser.feed(
            std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(count)});
        if (!parsed) {
          std::lock_guard lock{mutex};
          fail_locked(parsed.error());
          return;
        }
        for (auto& event : *parsed) {
          accept_event(std::move(event));
        }
        continue;
      }
      if (count == 0) {
        close_fd(output);
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      const auto saved_errno = errno;
      close_fd(output);
      std::lock_guard lock{mutex};
      fail_locked(system_error("read(control stdout)", saved_errno));
      return;
    }
  }

  void read_error() {
    std::array<char, 4096> buffer{};
    for (;;) {
      const auto count = ::read(error, buffer.data(), buffer.size());
      if (count > 0) {
        constexpr std::size_t maximum = 64U * 1024U;
        const auto available = maximum - std::min(maximum, stderr_tail.size());
        const auto captured = std::min(available, static_cast<std::size_t>(count));
        stderr_tail.append(buffer.data(), captured);
        continue;
      }
      if (count == 0) {
        close_fd(error);
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      const auto saved_errno = errno;
      close_fd(error);
      std::lock_guard lock{mutex};
      fail_locked(system_error("read(control stderr)", saved_errno));
      return;
    }
  }

  // True only for the ways a process can be over. `waitid` reports stops and
  // continues with the same success code as an exit, distinguished by nothing
  // but this field.
  [[nodiscard]] static bool is_exit(int signal_code) noexcept {
    return signal_code == CLD_EXITED || signal_code == CLD_KILLED ||
           signal_code == CLD_DUMPED;
  }

  void waiter_main() noexcept {
    siginfo_t child{};
    int observed = -1;
    for (;;) {
      child = siginfo_t{};
      observed = ::waitid(P_PID, static_cast<id_t>(pid), &child, kChildWaitOptions);
      if (observed < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      // A successful return is not an exit. `si_pid` is zero when there was
      // nothing to report, and `si_code` says whether what happened was an exit
      // or merely a stop — which a platform may report even though this call
      // asked only for exits. Taking either for an exit is what deadlocked
      // shutdown: the flag it sets suppresses the very signals that would make
      // the child exit, so a stopped child could never be killed, this thread
      // never returned, and joining it hung for good.
      if (child.si_pid != 0 && is_exit(child.si_code)) {
        break;
      }
      // `WNOWAIT` leaves what was reported pending, so asking again returns the
      // same answer immediately. Wait a little rather than spin on a platform
      // that keeps offering a stop.
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    if (observed < 0) {
      const auto saved_errno = errno;
      std::lock_guard lock{mutex};
      if (saved_errno == ECHILD) {
        child_exited = true;
        reaped = true;
      } else {
        fail_locked(system_error("waitid(control client)", saved_errno));
      }
      condition.notify_all();
      return;
    }
    {
      std::lock_guard lock{mutex};
      child_exited = true;
      condition.notify_all();
    }

    int status = 0;
    pid_t waited = -1;
    do {
      waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    const auto saved_errno = errno;
    {
      std::lock_guard lock{mutex};
      if (waited == pid || (waited < 0 && saved_errno == ECHILD)) {
        reaped = true;
        if (waited == pid) {
          wait_status = status;
        }
      } else {
        fail_locked(system_error("waitpid(control client)", saved_errno));
      }
      condition.notify_all();
    }
  }

  void reader_main() noexcept {
    try {
      while (output >= 0 || error >= 0) {
        std::array<pollfd, 2> descriptors{{
            {.fd = output, .events = POLLIN, .revents = 0},
            {.fd = error, .events = POLLIN, .revents = 0},
        }};
        const auto result = ::poll(descriptors.data(), descriptors.size(), 50);
        if (result < 0) {
          if (errno == EINTR) {
            continue;
          }
          std::lock_guard lock{mutex};
          fail_locked(system_error("poll(control client)", errno));
          break;
        }
        if (output >= 0 &&
            (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
          read_output();
        }
        if (error >= 0 &&
            (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
          read_error();
        }
        {
          std::lock_guard lock{mutex};
          if (fatal_error) {
            break;
          }
        }
      }

      const auto finished = parser.finish();
      if (!finished) {
        std::lock_guard lock{mutex};
        fail_locked(finished.error());
      }
    } catch (const std::exception& exception) {
      std::lock_guard lock{mutex};
      fail_locked(
          ProtocolError{std::string{"control reader exception: "} + exception.what()});
    } catch (...) {
      std::lock_guard lock{mutex};
      fail_locked(ProtocolError{"control reader exception"});
    }

    bool terminate_client = false;
    {
      std::lock_guard lock{mutex};
      if (!saw_exit && !fatal_error) {
        fail_locked(ProtocolError{"control client exited without %exit"});
      }
      if (!pending.empty()) {
        fail_locked(ProtocolError{"control client exited with pending requests"});
      }
      terminate_client = fatal_error.has_value();
      if (terminate_client) {
        closing = true;
      }
    }
    if (terminate_client) {
      {
        std::lock_guard write_lock{write_mutex};
        close_fd(input);
      }
      {
        std::lock_guard lock{mutex};
        signal_child_locked(SIGTERM);
      }
    }
    close_fd(output);
    close_fd(error);
    {
      std::lock_guard lock{mutex};
      reader_done = true;
      condition.notify_all();
    }
  }

  expected<void, ProtocolError> write_bytes(std::string_view bytes_to_write,
                                            Clock::time_point deadline,
                                            bool permit_closing = false) {
    std::size_t written = 0U;
    while (written < bytes_to_write.size()) {
      if (!permit_closing) {
        std::lock_guard lock{mutex};
        if (closing || fatal_error) {
          return unexpected(
              ProtocolError{"control write cancelled by connection closure"});
        }
      }
      const auto now = Clock::now();
      if (now >= deadline) {
        return unexpected(ProtocolError{"control write deadline expired"});
      }
      const auto remaining =
          std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
      const auto timeout =
          std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds{50});
      pollfd descriptor{.fd = input, .events = POLLOUT, .revents = 0};
      const auto polled = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
      if (polled < 0) {
        if (errno == EINTR) {
          continue;
        }
        return unexpected(system_error("poll(control input)", errno));
      }
      if (polled == 0) {
        continue;
      }
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return unexpected(ProtocolError{"control input pipe closed"});
      }
      auto safe_write = write_without_sigpipe(input, bytes_to_write.data() + written,
                                              bytes_to_write.size() - written);
      if (!safe_write) {
        return unexpected(safe_write.error());
      }
      const auto count = *safe_write;
      if (count > 0) {
        written += static_cast<std::size_t>(count);
        if (!permit_closing) {
          partial_command_frame = written != bytes_to_write.size();
        }
        continue;
      }
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      return unexpected(system_error("write(control input)", errno));
    }
    return {};
  }

  ConnectionOptions options;
  pid_t pid{-1};
  int input{-1};
  int output{-1};
  int error{-1};
  Parser parser;
  std::thread reader;
  std::thread waiter;
  std::mutex mutex;
  std::timed_mutex write_mutex;
  std::timed_mutex lifecycle_mutex;
  std::condition_variable condition;
  std::deque<std::shared_ptr<PendingRequest>> pending;
  std::vector<Notification> notifications;
  // How many were discarded to keep the buffer bounded, so a caller can tell
  // "nothing happened" from "more happened than I collected".
  std::size_t notifications_dropped{0};
  std::optional<ProtocolError> fatal_error;
  std::string stderr_tail;
  std::optional<int> wait_status;
  bool startup_seen{false};
  bool ready{false};
  bool closing{false};
  bool saw_exit{false};
  bool reader_done{false};
  bool child_exited{false};
  bool reaped{false};
  bool shutdown_complete{false};
  bool partial_command_frame{false};
  std::optional<ProtocolError> shutdown_error;
};

Connection::Connection(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

Connection::~Connection() noexcept {
  if (!state_) {
    return;
  }
  try {
    static_cast<void>(shutdown(Clock::now() + state_->options.shutdown_timeout));
  } catch (...) {
  }
}

Connection::Connection(Connection&&) noexcept = default;

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  state_.reset();
  state_ = std::move(other.state_);
  return *this;
}

expected<Connection, ProtocolError> Connection::connect(ConnectionOptions options) {
  if (options.tmux_binary.empty()) {
    return unexpected(ProtocolError{"tmux binary path is empty"});
  }
  if (options.socket_path.empty()) {
    return unexpected(ProtocolError{"tmux socket path is empty"});
  }
  if (options.session_name.empty()) {
    return unexpected(ProtocolError{"tmux session name is empty"});
  }
  if (contains_nul(options.tmux_binary.native()) ||
      contains_nul(options.socket_path.native()) ||
      contains_nul(options.session_name)) {
    return unexpected(ProtocolError{"connection option contains NUL"});
  }
  if (options.startup_timeout <= std::chrono::milliseconds::zero() ||
      options.shutdown_timeout <= std::chrono::milliseconds::zero()) {
    return unexpected(ProtocolError{"connection timeouts must be positive"});
  }

  auto spawned = spawn_client(options);
  if (!spawned) {
    return unexpected(spawned.error());
  }
  auto state = std::make_unique<State>(std::move(options), *spawned);
  try {
    state->waiter =
        std::thread{[raw_state = state.get()] { raw_state->waiter_main(); }};
    state->reader =
        std::thread{[raw_state = state.get()] { raw_state->reader_main(); }};
  } catch (const std::exception& exception) {
    return unexpected(
        ProtocolError{std::string{"control worker start failed: "} + exception.what()});
  }

  const auto startup_deadline = Clock::now() + state->options.startup_timeout;
  std::optional<ProtocolError> failure;
  {
    std::unique_lock lock{state->mutex};
    static_cast<void>(state->condition.wait_until(lock, startup_deadline, [&] {
      return state->ready || state->fatal_error || state->reader_done;
    }));
    if (state->ready) {
      return Connection{std::move(state)};
    }
    const auto fatal_error = state->fatal_error;
    if (fatal_error) {
      failure = fatal_error.value();
    } else {
      failure = ProtocolError{"control client startup deadline expired"};
    }
  }

  Connection cleanup{std::move(state)};
  static_cast<void>(
      cleanup.shutdown(Clock::now() + cleanup.state_->options.shutdown_timeout));
  return unexpected(std::move(failure.value()));
}

ControlRequestResult Connection::execute(ControlRequest request,
                                         Clock::time_point deadline) {
  const auto operation_count = request.group.size();
  auto pending = std::make_shared<State::PendingRequest>(operation_count);
  auto rendered = render_request(std::move(request));
  if (!rendered) {
    pending->result.connection_error = rendered.error();
    return std::move(pending->result);
  }
  if (!state_) {
    pending->result.connection_error = ProtocolError{"control connection has no state"};
    return std::move(pending->result);
  }
  if (Clock::now() >= deadline) {
    pending->result.connection_error =
        ProtocolError{"control request deadline expired before dispatch"};
    return std::move(pending->result);
  }

  bool poison_connection = false;
  {
    std::unique_lock<std::timed_mutex> write_lock{state_->write_mutex, std::defer_lock};
    if (!write_lock.try_lock_until(deadline) || Clock::now() >= deadline) {
      pending->result.connection_error =
          ProtocolError{"control writer acquisition deadline expired"};
      return std::move(pending->result);
    }
    {
      std::lock_guard state_lock{state_->mutex};
      const auto fatal_error = state_->fatal_error;
      if (fatal_error) {
        pending->result.connection_error = fatal_error.value();
        return std::move(pending->result);
      }
      if (!state_->ready || state_->closing || state_->shutdown_complete) {
        pending->result.connection_error =
            ProtocolError{"control connection is not accepting requests"};
        return std::move(pending->result);
      }
      state_->pending.push_back(pending);
    }

    auto written = state_->write_bytes(*rendered, deadline);
    if (!written) {
      std::lock_guard state_lock{state_->mutex};
      if (!state_->closing) {
        state_->closing = true;
        state_->fail_locked(written.error());
        poison_connection = true;
      }
    }
  }

  {
    std::unique_lock lock{state_->mutex};
    if (!pending->complete) {
      const auto completed = state_->condition.wait_until(
          lock, deadline, [&] { return pending->complete; });
      if (!completed) {
        state_->closing = true;
        state_->fail_locked(
            ProtocolError{"control request deadline expired after dispatch"});
        poison_connection = true;
      }
    }
  }

  if (poison_connection) {
    {
      std::lock_guard write_lock{state_->write_mutex};
      close_fd(state_->input);
    }
    {
      std::lock_guard lock{state_->mutex};
      state_->signal_child_locked(SIGTERM);
    }
  }
  return std::move(pending->result);
}

std::vector<Notification> Connection::take_notifications() {
  if (!state_) {
    return {};
  }
  std::lock_guard lock{state_->mutex};
  std::vector<Notification> available;
  available.swap(state_->notifications);
  return available;
}

std::size_t Connection::dropped_notifications() const noexcept {
  if (!state_) {
    return 0;
  }
  std::lock_guard lock{state_->mutex};
  return state_->notifications_dropped;
}

std::int64_t Connection::native_child_pid() const noexcept {
  if (!state_) {
    return -1;
  }
  return static_cast<std::int64_t>(state_->pid);
}

expected<void, ProtocolError> Connection::shutdown(Clock::time_point deadline) {
  if (!state_) {
    return {};
  }
  std::unique_lock<std::timed_mutex> lifecycle_lock{state_->lifecycle_mutex,
                                                    std::defer_lock};
  if (!lifecycle_lock.try_lock_until(deadline) || Clock::now() >= deadline) {
    return unexpected(ProtocolError{"control shutdown acquisition deadline expired"});
  }
  {
    std::lock_guard lock{state_->mutex};
    if (state_->shutdown_complete) {
      const auto shutdown_error = state_->shutdown_error;
      if (shutdown_error) {
        return unexpected(shutdown_error.value());
      }
      return {};
    }
  }

  {
    std::lock_guard lock{state_->mutex};
    state_->closing = true;
    for (auto& pending_request : state_->pending) {
      if (!pending_request->result.connection_error) {
        pending_request->result.connection_error =
            ProtocolError{"control connection shut down"};
      }
      pending_request->complete = true;
    }
    state_->pending.clear();
    state_->condition.notify_all();
  }
  {
    std::unique_lock<std::timed_mutex> write_lock{state_->write_mutex, std::defer_lock};
    if (!write_lock.try_lock_until(deadline)) {
      return unexpected(ProtocolError{"control shutdown writer deadline expired"});
    }
    if (state_->input >= 0 && !state_->partial_command_frame &&
        Clock::now() < deadline) {
      const auto write_deadline =
          std::min(deadline, Clock::now() + std::chrono::milliseconds{50});
      static_cast<void>(state_->write_bytes("\n", write_deadline, true));
    }
    close_fd(state_->input);
  }

  const auto started = Clock::now();
  const auto remaining =
      deadline > started ? deadline - started : Clock::duration::zero();
  const auto graceful_deadline = started + remaining / 2;
  const auto terminate_deadline = started + (remaining * 3) / 4;
  {
    std::unique_lock lock{state_->mutex};
    static_cast<void>(state_->condition.wait_until(lock, graceful_deadline, [&] {
      return state_->reader_done && state_->reaped;
    }));
  }
  {
    std::lock_guard lock{state_->mutex};
    state_->signal_child_locked(SIGTERM);
  }
  {
    std::unique_lock lock{state_->mutex};
    static_cast<void>(state_->condition.wait_until(lock, terminate_deadline, [&] {
      return state_->reader_done && state_->reaped;
    }));
  }
  {
    std::lock_guard lock{state_->mutex};
    state_->signal_child_locked(SIGKILL);
  }
  bool shutdown_finished = false;
  {
    std::unique_lock lock{state_->mutex};
    static_cast<void>(state_->condition.wait_until(
        lock, deadline, [&] { return state_->reader_done && state_->reaped; }));
    shutdown_finished = state_->reader_done && state_->reaped;
  }
  if (!shutdown_finished) {
    return unexpected(ProtocolError{"control client shutdown deadline expired"});
  }
  if (state_->reader.joinable()) {
    state_->reader.join();
  }
  if (state_->waiter.joinable()) {
    state_->waiter.join();
  }

  std::optional<ProtocolError> shutdown_error;
  {
    std::lock_guard lock{state_->mutex};
    const auto fatal_error = state_->fatal_error;
    if (fatal_error) {
      shutdown_error = fatal_error.value();
    } else if (!state_->saw_exit) {
      shutdown_error = ProtocolError{"control client did not emit %exit"};
    }
    state_->shutdown_error = shutdown_error;
    state_->shutdown_complete = true;
  }
  if (shutdown_error) {
    return unexpected(std::move(shutdown_error.value()));
  }
  return {};
}

LIBTMUX_NAMESPACE_END
