#include "support/process.hpp"
#include "libtmux/expected.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace libtmux::test::detail {

struct ReapTicket {
  int pidfd{-1};
  pid_t fallback_pid{-1};
  ReapTicket* next{nullptr};
};

namespace {

constexpr auto default_cleanup_timeout = std::chrono::milliseconds{100};

std::string system_error(std::string_view operation, int error_number) {
  return std::string{operation} + ": " + std::strerror(error_number);
}

void close_fd(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

// Whether this platform can hand out a pidfd. Where it cannot — macOS, the
// BSDs — reaping falls back to the child's pid, which is what everything did
// before pidfds existed. The race a pidfd closes is that a reaped pid can be
// reused before the next call names it; this helper reaps its own children
// exclusively and nothing else waits for them, which is what keeps the
// fallback sound rather than merely traditional.
// `LIBTMUX_FORCE_PORTABLE_SYSCALLS` selects the fallback on Linux too, so the
// path a macOS runner takes can be run here rather than only there.
#if defined(__linux__) && defined(SYS_pidfd_open) &&                                   \
    !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
inline constexpr bool kHasPidfd = true;
#else
inline constexpr bool kHasPidfd = false;
#endif

int open_pidfd(pid_t pid) noexcept {
#if defined(__linux__) && defined(SYS_pidfd_open) &&                                   \
    !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U));
#else
  static_cast<void>(pid);
  errno = ENOSYS;
  return -1;
#endif
}

int signal_pidfd(int pidfd, int signal_number) noexcept {
#if defined(__linux__) && defined(SYS_pidfd_send_signal) &&                            \
    !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  return static_cast<int>(
      ::syscall(SYS_pidfd_send_signal, pidfd, signal_number, nullptr, 0U));
#else
  static_cast<void>(pidfd);
  static_cast<void>(signal_number);
  errno = ENOSYS;
  return -1;
#endif
}

libtmux::expected<void, std::string> validate_sigchld_disposition() {
  struct sigaction action {};
  if (::sigaction(SIGCHLD, nullptr, &action) != 0) {
    return libtmux::unexpected(system_error("sigaction(SIGCHLD)", errno));
  }
  if (action.sa_handler == SIG_IGN) {
    return libtmux::unexpected("unsafe SIGCHLD disposition: SIG_IGN prevents reaping");
  }
  if ((action.sa_flags & SA_NOCLDWAIT) != 0) {
    return libtmux::unexpected(
        "unsafe SIGCHLD disposition: SA_NOCLDWAIT prevents reaping");
  }
  return {};
}

libtmux::expected<int, std::string> reserve_pidfd_capacity() {
  if constexpr (!kHasPidfd) {
    // The reservation proves a pidfd can still be opened after the spawn. With
    // no pidfds there is nothing to run out of, and -1 closes harmlessly.
    return -1;
  }
  auto descriptor = open_pidfd(::getpid());
  if (descriptor < 0) {
    return libtmux::unexpected(system_error("pidfd_open reservation", errno));
  }
#if defined(__linux__)
  siginfo_t information{};
  if (::waitid(P_PIDFD, static_cast<id_t>(descriptor), &information,
               WEXITED | WNOHANG) < 0 &&
      errno != ECHILD) {
    const auto saved_errno = errno;
    close_fd(descriptor);
    return libtmux::unexpected(system_error("waitid(P_PIDFD) validation", saved_errno));
  }
#endif
  return descriptor;
}

int wait_status_from(const siginfo_t& information) noexcept {
  switch (information.si_code) {
  case CLD_EXITED:
    return (information.si_status & 0xff) << 8;
  case CLD_KILLED:
    return information.si_status & 0x7f;
  case CLD_DUMPED:
    return (information.si_status & 0x7f) | 0x80;
  default:
    return information.si_status & 0xff;
  }
}

bool consume_pidfd_status(int pidfd, std::optional<int>* status) noexcept {
#if defined(__linux__) && !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  siginfo_t information{};
  if (::waitid(P_PIDFD, static_cast<id_t>(pidfd), &information, WEXITED | WNOHANG) ==
      0) {
    if (information.si_pid == 0) {
      return false;
    }
    if (status != nullptr) {
      *status = wait_status_from(information);
    }
    return true;
  }
  return errno == ECHILD;
#else
  static_cast<void>(pidfd);
  static_cast<void>(status);
  return false;
#endif
}

bool consume_reap_ticket(ReapTicket& ticket) noexcept {
  if (ticket.pidfd >= 0) {
    return consume_pidfd_status(ticket.pidfd, nullptr);
  }
  if (ticket.fallback_pid <= 0) {
    return true;
  }

  // Numeric identity is retained only when post-spawn pidfd acquisition
  // failed. The exclusive-child-reaping invariant prevents PID reuse until
  // this direct child is consumed or ECHILD exposes an external violation.
  int status = 0;
  const auto result = ::waitpid(ticket.fallback_pid, &status, WNOHANG);
  return result == ticket.fallback_pid || (result < 0 && errno == ECHILD);
}

bool terminate_and_reap_without_pidfd(pid_t pid,
                                      ProcessClock::time_point deadline) noexcept {
  bool kill_requested = false;
  for (;;) {
    int status = 0;
    const auto result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
      return true;
    }
    if (result < 0 && errno != EINTR) {
      return false;
    }
    if (!kill_requested) {
      if (::kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        return false;
      }
      kill_requested = true;
    }
    const auto now = ProcessClock::now();
    if (now >= deadline) {
      return false;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    const auto delay = std::min(remaining, std::chrono::milliseconds{1});
    static_cast<void>(::poll(nullptr, 0, static_cast<int>(delay.count())));
  }
}

class ReaperService final {
public:
  ReaperService() : worker_([this] { run(); }) {}

  ~ReaperService() = delete;

  ReaperService(const ReaperService&) = delete;
  ReaperService& operator=(const ReaperService&) = delete;

  void handoff(ReapTicket* ticket) noexcept {
    auto* head = incoming_.load(std::memory_order_relaxed);
    do {
      ticket->next = head;
    } while (!incoming_.compare_exchange_weak(head, ticket, std::memory_order_release,
                                              std::memory_order_relaxed));
    condition_.notify_one();
  }

private:
  void run() noexcept {
    ReapTicket* active = nullptr;
    for (;;) {
      auto* incoming = incoming_.exchange(nullptr, std::memory_order_acquire);
      while (incoming != nullptr) {
        auto* ticket = incoming;
        incoming = ticket->next;
        ticket->next = active;
        active = ticket;
      }

      auto** link = &active;
      while (*link != nullptr) {
        auto* ticket = *link;
        if (consume_reap_ticket(*ticket)) {
          *link = ticket->next;
          close_fd(ticket->pidfd);
          delete ticket;
        } else {
          link = &ticket->next;
        }
      }

      std::unique_lock lock{sleep_mutex_};
      static_cast<void>(condition_.wait_for(lock, std::chrono::milliseconds{5}, [this] {
        return incoming_.load(std::memory_order_acquire) != nullptr;
      }));
    }
  }

  std::atomic<ReapTicket*> incoming_{nullptr};
  std::mutex sleep_mutex_;
  std::condition_variable condition_;
  std::thread worker_;
};

ReaperService& reaper_service() {
  // Teardown may hand off children during static destruction. Keep both the
  // service and its worker alive until the process exits.
  static auto* service = new ReaperService;
  return *service;
}

libtmux::expected<std::unique_ptr<ReapTicket>, std::string> make_reap_ticket() {
  try {
    static_cast<void>(reaper_service());
    return std::make_unique<ReapTicket>();
  } catch (const std::exception& error) {
    return libtmux::unexpected(std::string{"reaper initialization failed: "} +
                               error.what());
  } catch (...) {
    return libtmux::unexpected("reaper initialization failed");
  }
}

libtmux::expected<std::array<int, 2>, std::string> create_pipe() {
  std::array<int, 2> descriptors{-1, -1};
#if defined(__linux__)
  if (::pipe2(descriptors.data(), O_CLOEXEC) != 0) {
    return libtmux::unexpected(system_error("pipe2", errno));
  }
#else
  if (::pipe(descriptors.data()) != 0) {
    return libtmux::unexpected(system_error("pipe", errno));
  }
  for (const auto descriptor : descriptors) {
    const auto flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
      const auto saved_errno = errno;
      auto read_descriptor = descriptors[0];
      auto write_descriptor = descriptors[1];
      close_fd(read_descriptor);
      close_fd(write_descriptor);
      return libtmux::unexpected(system_error("fcntl(FD_CLOEXEC)", saved_errno));
    }
  }
#endif
  return descriptors;
}

bool make_nonblocking(int descriptor) noexcept {
  const auto flags = ::fcntl(descriptor, F_GETFL);
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
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

} // namespace

ChildProcess::ChildProcess(pid_t pid, int stdout_fd, int stderr_fd,
                           std::size_t capture_limit,
                           std::unique_ptr<ReapTicket> reap_ticket) noexcept
    : pid_(pid), stdout_fd_(stdout_fd), stderr_fd_(stderr_fd),
      capture_limit_(capture_limit), reap_ticket_(std::move(reap_ticket)),
      owns_child_(true) {}

libtmux::expected<ChildProcess, std::string>
ChildProcess::spawn(ProcessOptions options) {
  auto sigchld = validate_sigchld_disposition();
  if (!sigchld.has_value()) {
    return libtmux::unexpected(sigchld.error());
  }
  auto reap_ticket = make_reap_ticket();
  if (!reap_ticket.has_value()) {
    return libtmux::unexpected(reap_ticket.error());
  }
  auto stdout_pipe = create_pipe();
  if (!stdout_pipe.has_value()) {
    return libtmux::unexpected(stdout_pipe.error());
  }
  auto stderr_pipe = create_pipe();
  if (!stderr_pipe.has_value()) {
    auto stdout_read = (*stdout_pipe)[0];
    auto stdout_write = (*stdout_pipe)[1];
    close_fd(stdout_read);
    close_fd(stdout_write);
    return libtmux::unexpected(stderr_pipe.error());
  }

  auto stdout_read = (*stdout_pipe)[0];
  auto stdout_write = (*stdout_pipe)[1];
  auto stderr_read = (*stderr_pipe)[0];
  auto stderr_write = (*stderr_pipe)[1];
  const auto close_pipes = [&]() noexcept {
    close_fd(stdout_read);
    close_fd(stdout_write);
    close_fd(stderr_read);
    close_fd(stderr_write);
  };

  if (!make_nonblocking(stdout_read) || !make_nonblocking(stderr_read)) {
    const auto saved_errno = errno;
    close_pipes();
    return libtmux::unexpected(system_error("fcntl(O_NONBLOCK)", saved_errno));
  }

  posix_spawn_file_actions_t actions;
  auto result = ::posix_spawn_file_actions_init(&actions);
  if (result != 0) {
    close_pipes();
    return libtmux::unexpected(system_error("posix_spawn_file_actions_init", result));
  }
  const auto fail_action =
      [&](std::string_view operation,
          int action_result) -> libtmux::expected<ChildProcess, std::string> {
    static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
    close_pipes();
    return libtmux::unexpected(system_error(operation, action_result));
  };

  result = ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                              O_RDONLY, 0);
  if (result != 0) {
    return fail_action("posix_spawn_file_actions_addopen", result);
  }
  result = ::posix_spawn_file_actions_adddup2(&actions, stdout_write, STDOUT_FILENO);
  if (result != 0) {
    return fail_action("posix_spawn_file_actions_adddup2(stdout)", result);
  }
  result = ::posix_spawn_file_actions_adddup2(&actions, stderr_write, STDERR_FILENO);
  if (result != 0) {
    return fail_action("posix_spawn_file_actions_adddup2(stderr)", result);
  }
#if defined(__GLIBC__) && defined(__USE_MISC)
  result = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  if (result != 0) {
    return fail_action("posix_spawn_file_actions_addclosefrom_np", result);
  }
#else
  for (const auto descriptor : {stdout_read, stdout_write, stderr_read, stderr_write}) {
    result = ::posix_spawn_file_actions_addclose(&actions, descriptor);
    if (result != 0) {
      return fail_action("posix_spawn_file_actions_addclose", result);
    }
  }
#endif

  std::vector<std::string> arguments;
  arguments.reserve(options.arguments.size() + 1U);
  arguments.push_back(options.executable.string());
  arguments.insert(arguments.end(), std::make_move_iterator(options.arguments.begin()),
                   std::make_move_iterator(options.arguments.end()));
  auto argument_pointers = writable_pointers(arguments);
  auto environment_pointers = writable_pointers(options.environment);

  auto pidfd_reservation = reserve_pidfd_capacity();
  if (!pidfd_reservation.has_value()) {
    static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
    close_pipes();
    return libtmux::unexpected(pidfd_reservation.error());
  }

  pid_t child_pid = -1;
  result = ::posix_spawnp(&child_pid, arguments.front().c_str(), &actions, nullptr,
                          argument_pointers.data(), environment_pointers.data());
  const auto destroy_result = ::posix_spawn_file_actions_destroy(&actions);
  close_fd(stdout_write);
  close_fd(stderr_write);
  close_fd(*pidfd_reservation);
  if (result != 0) {
    close_fd(stdout_read);
    close_fd(stderr_read);
    return libtmux::unexpected(system_error("posix_spawnp", result));
  }
  const auto pidfd = open_pidfd(child_pid);
  if (pidfd < 0 && !kHasPidfd) {
    // Expected here, and not an error: the ticket carries the pid instead, and
    // signalling and reaping go through it.
    (*reap_ticket)->fallback_pid = child_pid;
  } else if (pidfd < 0) {
    const auto saved_errno = errno;
    close_fd(stdout_read);
    close_fd(stderr_read);
    auto error = system_error("pidfd_open", saved_errno);
    // No pidfd exists on this path. Under the exclusive direct-child reaping
    // invariant, the unreaped child's numeric PID cannot be reused while this
    // bounded emergency cleanup terminates and consumes it.
    if (!terminate_and_reap_without_pidfd(child_pid, ProcessClock::now() +
                                                         default_cleanup_timeout)) {
      (*reap_ticket)->fallback_pid = child_pid;
      reaper_service().handoff(reap_ticket->release());
      error.append("; direct-child cleanup exceeded its deadline");
    }
    return libtmux::unexpected(std::move(error));
  }
  (*reap_ticket)->pidfd = pidfd;
  ChildProcess child{child_pid, stdout_read, stderr_read, options.capture_limit,
                     std::move(*reap_ticket)};
  if (destroy_result != 0) {
    child.terminate_and_reap(ProcessClock::now() + std::chrono::milliseconds{100});
    return libtmux::unexpected(
        system_error("posix_spawn_file_actions_destroy", destroy_result));
  }
  try {
    child.stdout_.reserve(options.capture_limit);
    child.stderr_.reserve(options.capture_limit);
  } catch (...) {
    return libtmux::unexpected("process capture allocation failed");
  }
  return child;
}

ChildProcess::~ChildProcess() noexcept {
  cleanup(ProcessClock::now() + default_cleanup_timeout);
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(std::exchange(other.pid_, -1)),
      stdout_fd_(std::exchange(other.stdout_fd_, -1)),
      stderr_fd_(std::exchange(other.stderr_fd_, -1)),
      capture_limit_(std::exchange(other.capture_limit_, 0)),
      stdout_(std::move(other.stdout_)), stderr_(std::move(other.stderr_)),
      wait_status_(std::exchange(other.wait_status_, std::nullopt)),
      reap_ticket_(std::move(other.reap_ticket_)),
      owns_child_(std::exchange(other.owns_child_, false)) {}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  cleanup(ProcessClock::now() + default_cleanup_timeout);
  pid_ = std::exchange(other.pid_, -1);
  stdout_fd_ = std::exchange(other.stdout_fd_, -1);
  stderr_fd_ = std::exchange(other.stderr_fd_, -1);
  capture_limit_ = std::exchange(other.capture_limit_, 0);
  stdout_ = std::move(other.stdout_);
  stderr_ = std::move(other.stderr_);
  wait_status_ = std::exchange(other.wait_status_, std::nullopt);
  reap_ticket_ = std::move(other.reap_ticket_);
  owns_child_ = std::exchange(other.owns_child_, false);
  return *this;
}

pid_t ChildProcess::pid() const noexcept { return pid_; }

bool ChildProcess::is_running() noexcept {
  update_status();
  return owns_child_;
}

bool ChildProcess::wait_until(ProcessClock::time_point deadline) noexcept {
  for (;;) {
    update_status();
    if (!owns_child_) {
      drain_once(deadline, std::chrono::milliseconds{0});
      return true;
    }
    const auto now = ProcessClock::now();
    if (now >= deadline) {
      return false;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    drain_once(deadline, std::min(remaining, std::chrono::milliseconds{10}));
  }
}

bool ChildProcess::send_signal(int signal_number) noexcept {
  update_status();
  if (!owns_child_ || !reap_ticket_) {
    return false;
  }
  if (reap_ticket_->pidfd < 0) {
    if (reap_ticket_->fallback_pid <= 0) {
      return false;
    }
    if (::kill(reap_ticket_->fallback_pid, signal_number) == 0) {
      return true;
    }
    if (errno == ESRCH) {
      update_status();
    }
    return false;
  }
  if (signal_pidfd(reap_ticket_->pidfd, signal_number) == 0) {
    return true;
  }
  if (errno == ESRCH) {
    update_status();
  }
  return false;
}

void ChildProcess::drain_until(ProcessClock::time_point deadline) noexcept {
  while ((stdout_fd_ >= 0 || stderr_fd_ >= 0) && ProcessClock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - ProcessClock::now());
    drain_once(deadline, std::min(remaining, std::chrono::milliseconds{10}));
  }
}

void ChildProcess::terminate_and_reap(ProcessClock::time_point deadline) noexcept {
  cleanup(deadline);
}

void ChildProcess::close_output() noexcept {
  close_fd(stdout_fd_);
  close_fd(stderr_fd_);
}

std::optional<int> ChildProcess::wait_status() const noexcept { return wait_status_; }

const std::string& ChildProcess::stdout_text() const noexcept { return stdout_; }

const std::string& ChildProcess::stderr_text() const noexcept { return stderr_; }

void ChildProcess::cleanup(ProcessClock::time_point deadline) noexcept {
  update_status();
  if (owns_child_) {
    static_cast<void>(send_signal(SIGKILL));
    static_cast<void>(wait_until(deadline));
  }
  if (owns_child_) {
    handoff_reap();
  }
  close_output();
}

void ChildProcess::handoff_reap() noexcept {
  if (!owns_child_) {
    return;
  }
  if (reap_ticket_) {
    reaper_service().handoff(reap_ticket_.release());
  }
  owns_child_ = false;
}

void ChildProcess::drain_once(ProcessClock::time_point deadline,
                              std::chrono::milliseconds maximum_wait) noexcept {
  std::array<pollfd, 2> descriptors{{
      {.fd = stdout_fd_, .events = POLLIN, .revents = 0},
      {.fd = stderr_fd_, .events = POLLIN, .revents = 0},
  }};
  int poll_result = 0;
  for (;;) {
    const auto now = ProcessClock::now();
    if (now >= deadline) {
      return;
    }
    int timeout_count = 0;
    if (maximum_wait > std::chrono::milliseconds{0}) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
          std::min(deadline - now, ProcessClock::duration{maximum_wait}));
      // Explicit, because milliseconds::rep is long on one standard library
      // and long long on another, and deduction picks neither.
      const auto bounded = std::min<std::chrono::milliseconds::rep>(
          remaining.count(), std::numeric_limits<int>::max());
      timeout_count = static_cast<int>(bounded);
    }
    poll_result = ::poll(descriptors.data(), descriptors.size(), timeout_count);
    if (poll_result >= 0 || errno != EINTR) {
      break;
    }
  }
  if (poll_result < 0) {
    close_fd(stdout_fd_);
    close_fd(stderr_fd_);
    return;
  }

  const auto read_descriptor = [&](pollfd& descriptor, int& owned_descriptor,
                                   std::string& capture) noexcept {
    if (descriptor.fd < 0 || (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
      return;
    }
    std::array<char, 4096> buffer{};
    for (;;) {
      if (ProcessClock::now() >= deadline) {
        return;
      }
      const auto count = ::read(descriptor.fd, buffer.data(), buffer.size());
      if (count > 0) {
        const auto byte_count = static_cast<std::size_t>(count);
        const auto available =
            capture_limit_ > capture.size() ? capture_limit_ - capture.size() : 0U;
        const auto retained = std::min(byte_count, available);
        capture.append(buffer.data(), retained);
        continue;
      }
      if (count == 0) {
        close_fd(owned_descriptor);
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close_fd(owned_descriptor);
      }
      if (count < 0 && errno == EINTR && ProcessClock::now() < deadline) {
        continue;
      }
      break;
    }
  };
  read_descriptor(descriptors[0], stdout_fd_, stdout_);
  read_descriptor(descriptors[1], stderr_fd_, stderr_);
}

void ChildProcess::update_status() noexcept {
  if (!owns_child_) {
    return;
  }
  if (reap_ticket_ && reap_ticket_->pidfd < 0 && reap_ticket_->fallback_pid > 0) {
    int status = 0;
    const auto reaped = ::waitpid(reap_ticket_->fallback_pid, &status, WNOHANG);
    if (reaped == reap_ticket_->fallback_pid) {
      wait_status_ = status;
      owns_child_ = false;
    } else if (reaped < 0 && errno == ECHILD) {
      owns_child_ = false;
    }
  } else if (reap_ticket_ && consume_pidfd_status(reap_ticket_->pidfd, &wait_status_)) {
    owns_child_ = false;
  }
  if (!owns_child_ && reap_ticket_) {
    close_fd(reap_ticket_->pidfd);
    reap_ticket_.reset();
  }
}

std::vector<std::string> current_environment() {
  std::vector<std::string> result;
  for (auto entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    result.emplace_back(*entry);
  }
  return result;
}

void erase_environment(std::vector<std::string>& environment, std::string_view name) {
  const auto prefix = std::string{name} + "=";
  std::erase_if(environment,
                [&](const std::string& entry) { return entry.starts_with(prefix); });
}

void set_environment(std::vector<std::string>& environment, std::string_view name,
                     std::string_view value) {
  erase_environment(environment, name);
  environment.push_back(std::string{name} + "=" + std::string{value});
}

} // namespace libtmux::test::detail
