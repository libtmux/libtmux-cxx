#include "process.hpp"
#include "libtmux/expected.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto poll_quantum = std::chrono::milliseconds{10};
constexpr auto terminate_grace = std::chrono::milliseconds{100};
constexpr auto post_exit_drain = std::chrono::milliseconds{100};
constexpr auto kill_reap_grace = std::chrono::milliseconds{500};

class OwnedFd final {
public:
  OwnedFd() = default;
  explicit OwnedFd(int descriptor) noexcept : descriptor_(descriptor) {}

  ~OwnedFd() { reset(); }

  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;

  OwnedFd(OwnedFd&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  OwnedFd& operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
      reset();
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

  void reset() noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
      descriptor_ = -1;
    }
  }

private:
  int descriptor_{-1};
};

struct Pipe final {
  OwnedFd read;
  OwnedFd write;
};

struct Capture final {
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool truncated{false};
};

struct DrainFailure final {
  std::string_view operation;
  int error_number;
};

[[nodiscard]] std::error_code generic_error(int error_number) {
  return {error_number, std::generic_category()};
}

[[nodiscard]] bool contains_nul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] std::string escape_diagnostic_value(std::string_view value) {
  constexpr std::string_view hexadecimal{"0123456789abcdef"};
  std::string escaped;
  escaped.reserve(value.size());
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 0x20U && byte <= 0x7eU && byte != '\\') {
      escaped.push_back(character);
      continue;
    }
    if (byte == '\\') {
      escaped.append("\\\\");
      continue;
    }
    escaped.append("\\x");
    escaped.push_back(hexadecimal[byte >> 4U]);
    escaped.push_back(hexadecimal[byte & 0x0fU]);
  }
  return escaped;
}

[[nodiscard]] std::string render_request(const ProcessRequest& request) {
  std::string rendered = escape_diagnostic_value(request.executable.string());
  for (const auto& argument : request.arguments) {
    rendered.push_back(' ');
    rendered.append(argument.sensitivity == Sensitivity::secret
                        ? "[REDACTED]"
                        : escape_diagnostic_value(argument.value));
  }
  return rendered;
}

[[nodiscard]] std::string diagnostic(std::string_view operation,
                                     const ProcessRequest& request,
                                     const std::error_code& cause) {
  std::string result{operation};
  result.append(" failure running ");
  result.append(render_request(request));
  result.append(": ");
  result.append(cause.message());
  return result;
}

[[nodiscard]] ProcessError make_error(ProcessError::Kind kind, DispatchPhase phase,
                                      std::string_view operation,
                                      const ProcessRequest& request,
                                      std::error_code cause, Capture capture = {}) {
  return {
      .kind = kind,
      .dispatch_phase = phase,
      .cause = cause,
      .diagnostic = diagnostic(operation, request, cause),
      .stdout_bytes = std::move(capture.stdout_bytes),
      .stderr_bytes = std::move(capture.stderr_bytes),
      .output_truncated = capture.truncated,
  };
}

[[nodiscard]] std::optional<ProcessError>
validate_request(const ProcessRequest& request) {
  const auto invalid = [&]() {
    return make_error(ProcessError::Kind::validation, DispatchPhase::not_dispatched,
                      "validation", request,
                      std::make_error_code(std::errc::invalid_argument));
  };

  const auto executable = request.executable.string();
  if (executable.empty() || contains_nul(executable)) {
    return invalid();
  }
  for (const auto& argument : request.arguments) {
    if (contains_nul(argument.value)) {
      return invalid();
    }
  }
  for (const auto& [name, value] : request.environment) {
    if (name.empty() || name.find('=') != std::string::npos || contains_nul(name) ||
        (value.has_value() && contains_nul(*value))) {
      return invalid();
    }
  }
  if (request.stdio == StdioPolicy::inherit_terminal &&
      (::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0)) {
    return invalid();
  }
  return std::nullopt;
}

[[nodiscard]] expected<Pipe, int> create_pipe() {
  std::array<int, 2> descriptors{-1, -1};
#if defined(__linux__) && !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  if (::pipe2(descriptors.data(), O_CLOEXEC) != 0) {
    return unexpected(errno);
  }
  OwnedFd read{descriptors[0]};
  OwnedFd write{descriptors[1]};
#else
  // No `pipe2` outside Linux. Two calls instead of one, which leaves a window
  // where another thread forking would inherit these descriptors; this library
  // forks only through `spawn`, which closes what it does not pass on.
  if (::pipe(descriptors.data()) != 0) {
    return unexpected(errno);
  }
  OwnedFd read{descriptors[0]};
  OwnedFd write{descriptors[1]};
  for (const int descriptor : {read.get(), write.get()}) {
    const auto descriptor_flags = ::fcntl(descriptor, F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
      return unexpected(errno);
    }
  }
#endif
  const auto flags = ::fcntl(read.get(), F_GETFL);
  if (flags < 0 || ::fcntl(read.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
    return unexpected(errno);
  }
  return Pipe{.read = std::move(read), .write = std::move(write)};
}

[[nodiscard]] std::vector<std::string> environment_overlay(
    const std::vector<std::pair<std::string, std::optional<std::string>>>& overlay) {
  std::map<std::string, std::string, std::less<>> values;
  for (auto entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view current{*entry};
    const auto separator = current.find('=');
    if (separator != std::string_view::npos) {
      values.insert_or_assign(std::string{current.substr(0, separator)},
                              std::string{current.substr(separator + 1U)});
    }
  }
  for (const auto& [name, value] : overlay) {
    if (value.has_value()) {
      values.insert_or_assign(name, *value);
    } else {
      values.erase(name);
    }
  }

  std::vector<std::string> result;
  result.reserve(values.size());
  for (const auto& [name, value] : values) {
    result.push_back(name);
    result.back().push_back('=');
    result.back().append(value);
  }
  return result;
}

[[nodiscard]] std::vector<char*> mutable_pointers(std::vector<std::string>& values) {
  std::vector<char*> result;
  result.reserve(values.size() + 1U);
  for (auto& value : values) {
    result.push_back(value.data());
  }
  result.push_back(nullptr);
  return result;
}

void append_capture(std::vector<std::byte>& destination,
                    std::span<const std::byte> bytes, std::size_t limit,
                    bool& truncated) {
  const auto available = destination.size() < limit ? limit - destination.size() : 0U;
  const auto retained = std::min(available, bytes.size());
  const auto retained_bytes = bytes.first(retained);
  destination.insert(destination.end(), retained_bytes.begin(), retained_bytes.end());
  truncated = truncated || retained != bytes.size();
}

[[nodiscard]] std::optional<DrainFailure>
drain_descriptor(OwnedFd& descriptor, std::vector<std::byte>& destination,
                 std::size_t limit, bool& truncated, Clock::time_point boundary) {
  std::array<std::byte, 16384> buffer{};
  for (;;) {
    if (Clock::now() >= boundary) {
      return std::nullopt;
    }
    const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count > 0) {
      append_capture(
          destination,
          std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(count)},
          limit, truncated);
      continue;
    }
    if (count == 0) {
      descriptor.reset();
      return std::nullopt;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::nullopt;
    }
    const auto saved_errno = errno;
    descriptor.reset();
    truncated = true;
    return DrainFailure{.operation = "pipe read", .error_number = saved_errno};
  }
}

[[nodiscard]] int poll_timeout(Clock::time_point boundary) {
  const auto now = Clock::now();
  if (now >= boundary) {
    return 0;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(boundary - now);
  // Explicit, because milliseconds::rep is long on one standard library and
  // long long on another, and deduction picks neither.
  const auto bounded = std::min<std::chrono::milliseconds::rep>(
      remaining.count(), std::numeric_limits<int>::max());
  return static_cast<int>(bounded);
}

[[nodiscard]] std::optional<DrainFailure>
poll_and_drain(OwnedFd& stdout_read, OwnedFd& stderr_read, Capture& capture,
               std::size_t limit, Clock::time_point boundary) {
  std::array<pollfd, 2> descriptors{{
      {.fd = stdout_read.get(), .events = POLLIN, .revents = 0},
      {.fd = stderr_read.get(), .events = POLLIN, .revents = 0},
  }};
  int result = -1;
  for (;;) {
    if (Clock::now() >= boundary) {
      return std::nullopt;
    }
    result = ::poll(descriptors.data(), descriptors.size(), poll_timeout(boundary));
    if (result >= 0) {
      break;
    }
    if (errno != EINTR) {
      return DrainFailure{.operation = "pipe poll", .error_number = errno};
    }
  }

  const auto drain =
      [&](pollfd descriptor, OwnedFd& owned,
          std::vector<std::byte>& destination) -> std::optional<DrainFailure> {
    if (!owned.valid()) {
      return std::nullopt;
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
      owned.reset();
      capture.truncated = true;
      return DrainFailure{.operation = "pipe poll", .error_number = EBADF};
    }
    if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      return drain_descriptor(owned, destination, limit, capture.truncated, boundary);
    }
    return std::nullopt;
  };

  if (auto failure = drain(descriptors[0], stdout_read, capture.stdout_bytes)) {
    return failure;
  }
  return drain(descriptors[1], stderr_read, capture.stderr_bytes);
}

void close_remaining_output(OwnedFd& stdout_read, OwnedFd& stderr_read,
                            Capture& capture) {
  if (stdout_read.valid() || stderr_read.valid()) {
    capture.truncated = true;
  }
  stdout_read.reset();
  stderr_read.reset();
}

[[nodiscard]] bool update_status(pid_t child, bool& reaped, int& status,
                                 int& error_number) {
  if (reaped) {
    return true;
  }
  for (;;) {
    const auto result = ::waitpid(child, &status, WNOHANG);
    if (result == child) {
      reaped = true;
      return true;
    }
    if (result == 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    error_number = errno;
    return false;
  }
}

void signal_group(pid_t group, int signal_number) {
  while (::kill(-group, signal_number) < 0 && errno == EINTR) {
  }
}

void cleanup_group(pid_t child, bool& reaped, int& status, OwnedFd& stdout_read,
                   OwnedFd& stderr_read, Capture& capture, std::size_t capture_limit,
                   bool allow_term) {
  if (allow_term) {
    signal_group(child, SIGTERM);
    const auto term_deadline = Clock::now() + terminate_grace;
    while (!reaped && Clock::now() < term_deadline) {
      int wait_error = 0;
      static_cast<void>(update_status(child, reaped, status, wait_error));
      if (auto failure =
              poll_and_drain(stdout_read, stderr_read, capture, capture_limit,
                             std::min(term_deadline, Clock::now() + poll_quantum))) {
        if (failure->operation == "pipe read") {
          continue;
        }
        break;
      }
    }
  }

  signal_group(child, SIGKILL);
  const auto reap_deadline = Clock::now() + kill_reap_grace;
  while (!reaped && Clock::now() < reap_deadline) {
    int wait_error = 0;
    if (!update_status(child, reaped, status, wait_error)) {
      break;
    }
    if (auto failure =
            poll_and_drain(stdout_read, stderr_read, capture, capture_limit,
                           std::min(reap_deadline, Clock::now() + poll_quantum))) {
      if (failure->operation == "pipe read") {
        continue;
      }
      break;
    }
  }
  while (!reaped) {
    const auto result = ::waitpid(child, &status, 0);
    if (result == child) {
      reaped = true;
      break;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && errno == ECHILD) {
      reaped = true;
    }
    break;
  }

  const auto drain_deadline = Clock::now() + post_exit_drain;
  while ((stdout_read.valid() || stderr_read.valid()) &&
         Clock::now() < drain_deadline) {
    if (poll_and_drain(stdout_read, stderr_read, capture, capture_limit,
                       std::min(drain_deadline, Clock::now() + poll_quantum))) {
      break;
    }
  }
  close_remaining_output(stdout_read, stderr_read, capture);
}

[[nodiscard]] ProcessError::Kind spawn_error_kind(int error_number) {
  if (error_number == ENOENT || error_number == ENOTDIR) {
    return ProcessError::Kind::spawn;
  }
  return ProcessError::Kind::pre_exec;
}

[[nodiscard]] std::string_view spawn_operation(ProcessError::Kind kind) {
  return kind == ProcessError::Kind::spawn ? "spawn" : "pre-exec";
}

} // namespace

expected<ProcessReply, ProcessError> run_posix(const ProcessRequest& request) {
  if (auto validation = validate_request(request)) {
    return unexpected(std::move(*validation));
  }
  if (request.timeout.has_value() &&
      *request.timeout <= std::chrono::milliseconds::zero()) {
    return unexpected(make_error(ProcessError::Kind::timeout,
                                 DispatchPhase::not_dispatched, "timeout", request,
                                 std::make_error_code(std::errc::timed_out)));
  }

  std::optional<Clock::time_point> deadline;
  if (request.timeout.has_value()) {
    deadline = Clock::now() + *request.timeout;
  }
  Pipe stdout_pipe;
  Pipe stderr_pipe;
  if (request.stdio == StdioPolicy::capture) {
    auto created_stdout = create_pipe();
    if (!created_stdout) {
      const auto cause = generic_error(created_stdout.error());
      return unexpected(make_error(ProcessError::Kind::pipe,
                                   DispatchPhase::not_dispatched, "pipe", request,
                                   cause));
    }
    stdout_pipe = std::move(*created_stdout);
    auto created_stderr = create_pipe();
    if (!created_stderr) {
      const auto cause = generic_error(created_stderr.error());
      return unexpected(make_error(ProcessError::Kind::pipe,
                                   DispatchPhase::not_dispatched, "pipe", request,
                                   cause));
    }
    stderr_pipe = std::move(*created_stderr);
  }

  posix_spawn_file_actions_t actions;
  auto setup_result = ::posix_spawn_file_actions_init(&actions);
  if (setup_result != 0) {
    const auto cause = generic_error(setup_result);
    return unexpected(make_error(ProcessError::Kind::pipe,
                                 DispatchPhase::not_dispatched, "pipe", request,
                                 cause));
  }
  bool actions_initialized = true;
  const auto destroy_actions = [&]() {
    if (!actions_initialized) {
      return 0;
    }
    actions_initialized = false;
    return ::posix_spawn_file_actions_destroy(&actions);
  };
  const auto action_failure = [&](int error_number) {
    static_cast<void>(destroy_actions());
    return unexpected(make_error(ProcessError::Kind::pipe,
                                 DispatchPhase::not_dispatched, "pipe", request,
                                 generic_error(error_number)));
  };

  if (request.stdio == StdioPolicy::capture) {
    setup_result = ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                                      "/dev/null", O_RDONLY, 0);
    if (setup_result != 0) {
      return action_failure(setup_result);
    }
    setup_result = ::posix_spawn_file_actions_adddup2(&actions, stdout_pipe.write.get(),
                                                      STDOUT_FILENO);
    if (setup_result != 0) {
      return action_failure(setup_result);
    }
    setup_result = ::posix_spawn_file_actions_adddup2(&actions, stderr_pipe.write.get(),
                                                      STDERR_FILENO);
    if (setup_result != 0) {
      return action_failure(setup_result);
    }
  }
#if defined(__GLIBC__)
  setup_result = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  if (setup_result != 0) {
    return action_failure(setup_result);
  }
#endif

  posix_spawnattr_t attributes;
  setup_result = ::posix_spawnattr_init(&attributes);
  if (setup_result != 0) {
    return action_failure(setup_result);
  }
  bool attributes_initialized = true;
  const auto destroy_attributes = [&]() {
    if (!attributes_initialized) {
      return 0;
    }
    attributes_initialized = false;
    return ::posix_spawnattr_destroy(&attributes);
  };
  setup_result = ::posix_spawnattr_setflags(&attributes,
                                            static_cast<short>(POSIX_SPAWN_SETPGROUP));
  if (setup_result == 0) {
    setup_result = ::posix_spawnattr_setpgroup(&attributes, 0);
  }
  if (setup_result != 0) {
    static_cast<void>(destroy_attributes());
    return action_failure(setup_result);
  }

  std::vector<std::string> arguments;
  arguments.reserve(request.arguments.size() + 1U);
  arguments.push_back(request.executable.string());
  for (const auto& argument : request.arguments) {
    arguments.push_back(argument.value);
  }
  auto argument_pointers = mutable_pointers(arguments);
  auto environment = environment_overlay(request.environment);
  auto environment_pointers = mutable_pointers(environment);

  if (deadline.has_value() && Clock::now() >= *deadline) {
    static_cast<void>(destroy_attributes());
    static_cast<void>(destroy_actions());
    stdout_pipe.read.reset();
    stdout_pipe.write.reset();
    stderr_pipe.read.reset();
    stderr_pipe.write.reset();
    return unexpected(make_error(ProcessError::Kind::timeout,
                                 DispatchPhase::not_dispatched, "timeout", request,
                                 std::make_error_code(std::errc::timed_out)));
  }

  pid_t child = -1;
  const auto spawn_result =
      ::posix_spawnp(&child, arguments.front().c_str(), &actions, &attributes,
                     argument_pointers.data(), environment_pointers.data());
  const auto attribute_destroy_result = destroy_attributes();
  const auto action_destroy_result = destroy_actions();
  stdout_pipe.write.reset();
  stderr_pipe.write.reset();

  if (spawn_result != 0) {
    const auto kind = spawn_error_kind(spawn_result);
    return unexpected(make_error(kind, DispatchPhase::not_dispatched,
                                 spawn_operation(kind), request,
                                 generic_error(spawn_result)));
  }

  Capture capture;
  bool reaped = false;
  int status = 0;
  if (deadline.has_value() && Clock::now() >= *deadline) {
    cleanup_group(child, reaped, status, stdout_pipe.read, stderr_pipe.read, capture,
                  request.capture_limit, true);
    return unexpected(make_error(
        ProcessError::Kind::timeout, DispatchPhase::dispatch_uncertain, "timeout",
        request, std::make_error_code(std::errc::timed_out), std::move(capture)));
  }

  capture.stdout_bytes.reserve(std::min(request.capture_limit, std::size_t{4096}));
  capture.stderr_bytes.reserve(std::min(request.capture_limit, std::size_t{4096}));

  const auto post_spawn_setup_error =
      attribute_destroy_result != 0 ? attribute_destroy_result : action_destroy_result;
  if (post_spawn_setup_error != 0) {
    cleanup_group(child, reaped, status, stdout_pipe.read, stderr_pipe.read, capture,
                  request.capture_limit, false);
    return unexpected(
        make_error(ProcessError::Kind::pipe, DispatchPhase::dispatch_uncertain, "pipe",
                   request, generic_error(post_spawn_setup_error), std::move(capture)));
  }

  auto child_exit_drain_deadline = Clock::time_point::max();
  for (;;) {
    int wait_error = 0;
    if (!update_status(child, reaped, status, wait_error)) {
      cleanup_group(child, reaped, status, stdout_pipe.read, stderr_pipe.read, capture,
                    request.capture_limit, false);
      auto failure = make_error(ProcessError::Kind::pipe,
                                DispatchPhase::dispatch_uncertain, "waitpid pipe",
                                request, generic_error(wait_error), std::move(capture));
      // A process that has set SIGCHLD to SIG_IGN — routine in a daemon — has
      // told the kernel to reap its children itself, so there is no status
      // left to collect and waitpid answers ECHILD for every command run.
      // "No child processes" alone does not lead anyone to that.
      if (wait_error == ECHILD) {
        failure.diagnostic +=
            " (SIGCHLD is ignored in this process, so the child was reaped before "
            "its exit status could be read)";
      }
      return unexpected(std::move(failure));
    }
    if (reaped && child_exit_drain_deadline == Clock::time_point::max()) {
      child_exit_drain_deadline = Clock::now() + post_exit_drain;
    }
    if (reaped && !stdout_pipe.read.valid() && !stderr_pipe.read.valid()) {
      break;
    }
    if (reaped && Clock::now() >= child_exit_drain_deadline) {
      close_remaining_output(stdout_pipe.read, stderr_pipe.read, capture);
      break;
    }
    if (!reaped && deadline.has_value() && Clock::now() >= *deadline) {
      cleanup_group(child, reaped, status, stdout_pipe.read, stderr_pipe.read, capture,
                    request.capture_limit, true);
      return unexpected(make_error(
          ProcessError::Kind::timeout, DispatchPhase::dispatch_uncertain, "timeout",
          request, std::make_error_code(std::errc::timed_out), std::move(capture)));
    }

    const auto poll_boundary = Clock::now() + poll_quantum;
    const auto boundary =
        reaped ? std::min(child_exit_drain_deadline, poll_boundary)
               : (deadline.has_value() ? std::min(*deadline, poll_boundary)
                                       : poll_boundary);
    if (auto failure = poll_and_drain(stdout_pipe.read, stderr_pipe.read, capture,
                                      request.capture_limit, boundary)) {
      cleanup_group(child, reaped, status, stdout_pipe.read, stderr_pipe.read, capture,
                    request.capture_limit, false);
      return unexpected(
          make_error(ProcessError::Kind::pipe, DispatchPhase::dispatch_uncertain,
                     failure->operation, request, generic_error(failure->error_number),
                     std::move(capture)));
    }
  }

  Termination termination = WIFEXITED(status) ? Termination{Exited{WEXITSTATUS(status)}}
                                              : Termination{Signaled{WTERMSIG(status)}};
  return ProcessReply{
      .termination = termination,
      .stdout_bytes = std::move(capture.stdout_bytes),
      .stderr_bytes = std::move(capture.stderr_bytes),
      .output_truncated = capture.truncated,
  };
}

} // namespace detail
LIBTMUX_NAMESPACE_END
