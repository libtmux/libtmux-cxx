#include "posix_child.hpp"

#include "libtmux/expected.hpp"
#include "path.hpp"
#include "spawn_signals.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <map>
#include <span>
#include <utility>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

LIBTMUX_NAMESPACE_BEGIN
namespace detail {
namespace {

class OwnedFd final {
public:
  OwnedFd() = default;
  explicit OwnedFd(int descriptor) noexcept : descriptor_{descriptor} {}
  ~OwnedFd() { reset(); }
  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  OwnedFd(OwnedFd&& other) noexcept
      : descriptor_{std::exchange(other.descriptor_, -1)} {}
  OwnedFd& operator=(OwnedFd&& other) noexcept {
    if (this != &other) {
      reset();
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }
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

[[nodiscard]] expected<Pipe, int> create_pipe() {
  std::array<int, 2> descriptors{-1, -1};
#if defined(__linux__) && !defined(LIBTMUX_FORCE_PORTABLE_SYSCALLS)
  if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
    return unexpected(errno);
  }
#else
  if (::pipe(descriptors.data()) != 0) {
    return unexpected(errno);
  }
  for (const int descriptor : descriptors) {
    const auto descriptor_flags = ::fcntl(descriptor, F_GETFD);
    const auto status_flags = ::fcntl(descriptor, F_GETFL);
    if (descriptor_flags < 0 || status_flags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
        ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) != 0) {
      const auto error_number = errno;
      static_cast<void>(::close(descriptors[0]));
      static_cast<void>(::close(descriptors[1]));
      return unexpected(error_number);
    }
  }
#endif
  return Pipe{OwnedFd{descriptors[0]}, OwnedFd{descriptors[1]}};
}

[[nodiscard]] std::vector<std::string> environment_overlay(
    const std::vector<std::pair<std::string, std::optional<std::string>>>& overlay) {
  std::map<std::string, std::string, std::less<>> values;
  for (auto entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view current{*entry};
    const auto separator = current.find('=');
    if (separator != std::string_view::npos) {
      values.insert_or_assign(std::string{current.substr(0U, separator)},
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
    result.push_back(name + "=" + value);
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

[[nodiscard]] bool request_is_valid(const ProcessRequest& request) {
  const auto executable = libtmux_path::command_string(request.executable);
  if (executable.empty() || contains_nul(executable)) {
    return false;
  }
  for (const auto& argument : request.arguments) {
    if (contains_nul(argument.value)) {
      return false;
    }
  }
  for (const auto& [name, value] : request.environment) {
    if (name.empty() || name.find('=') != std::string::npos || contains_nul(name) ||
        (value.has_value() && contains_nul(*value))) {
      return false;
    }
  }
  // Handing a child a terminal it does not have is a request that cannot be
  // honoured, not a failure of the spawn.
  return request.stdio != StdioPolicy::inherit_terminal ||
         (::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0);
}

[[nodiscard]] ProcessError::Kind spawn_error_kind(int error_number) {
  return error_number == ENOENT || error_number == ENOTDIR
             ? ProcessError::Kind::spawn
             : ProcessError::Kind::pre_exec;
}

[[nodiscard]] std::string_view spawn_operation(ProcessError::Kind kind) {
  return kind == ProcessError::Kind::spawn ? "spawn" : "pre-exec";
}

} // namespace

std::string render_request(const ProcessRequest& request) {
  std::string rendered =
      escape_diagnostic_value(libtmux_path::command_string(request.executable));
  for (const auto& argument : request.arguments) {
    rendered.push_back(' ');
    rendered.append(argument.sensitivity == Sensitivity::secret
                        ? "[REDACTED]"
                        : escape_diagnostic_value(argument.value));
  }
  return rendered;
}

ProcessError process_error(ProcessError::Kind kind, DeliveryStatus delivery,
                           std::string_view operation,
                           std::string_view rendered_request, std::error_code cause,
                           Capture capture) {
  std::string message{operation};
  message.append(" failure running ");
  message.append(rendered_request);
  message.append(": ");
  message.append(cause.message());
  return {
      .kind = kind,
      .delivery = delivery,
      .cause = cause,
      .diagnostic = std::move(message),
      .stdout_bytes = std::move(capture.stdout_bytes),
      .stderr_bytes = std::move(capture.stderr_bytes),
      .output_truncated = capture.truncated,
  };
}

expected<PosixChild, ProcessError> PosixChild::launch(const ProcessRequest& request) {
  auto rendered = render_request(request);
  const auto fail = [&rendered](ProcessError::Kind kind, std::string_view operation,
                                int error_number) {
    return unexpected(process_error(kind, DeliveryStatus::not_started, operation,
                                    rendered, generic_error(error_number)));
  };
  if (!request_is_valid(request)) {
    return fail(ProcessError::Kind::validation, "validation", EINVAL);
  }

  const bool capturing = request.stdio == StdioPolicy::capture;
  Pipe stdout_pipe;
  Pipe stderr_pipe;
  if (capturing) {
    auto created_stdout = create_pipe();
    if (!created_stdout) {
      return fail(ProcessError::Kind::pipe, "pipe", created_stdout.error());
    }
    stdout_pipe = std::move(*created_stdout);
    auto created_stderr = create_pipe();
    if (!created_stderr) {
      return fail(ProcessError::Kind::pipe, "pipe", created_stderr.error());
    }
    stderr_pipe = std::move(*created_stderr);
  }

  posix_spawn_file_actions_t actions;
  auto result = ::posix_spawn_file_actions_init(&actions);
  if (result != 0) {
    return fail(ProcessError::Kind::pipe, "pipe", result);
  }
  const auto action_failure = [&](int error_number) {
    static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
    return fail(ProcessError::Kind::pipe, "pipe", error_number);
  };
  if (capturing) {
    result = ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                                O_RDONLY, 0);
    if (result == 0) {
      result = ::posix_spawn_file_actions_adddup2(&actions, stdout_pipe.write.get(),
                                                  STDOUT_FILENO);
    }
    if (result == 0) {
      result = ::posix_spawn_file_actions_adddup2(&actions, stderr_pipe.write.get(),
                                                  STDERR_FILENO);
    }
    if (result == 0) {
#if defined(__GLIBC__)
      result = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#else
      for (const int descriptor : {stdout_pipe.read.get(), stdout_pipe.write.get(),
                                   stderr_pipe.read.get(), stderr_pipe.write.get()}) {
        result = ::posix_spawn_file_actions_addclose(&actions, descriptor);
        if (result != 0) {
          break;
        }
      }
#endif
    }
    if (result != 0) {
      return action_failure(result);
    }
  }

  posix_spawnattr_t attributes;
  result = ::posix_spawnattr_init(&attributes);
  if (result != 0) {
    return action_failure(result);
  }
  const auto attribute_failure = [&](int error_number) {
    static_cast<void>(::posix_spawnattr_destroy(&attributes));
    return action_failure(error_number);
  };
  result = apply_clean_signal_attributes(attributes, POSIX_SPAWN_SETPGROUP);
  if (result == 0) {
    result = ::posix_spawnattr_setpgroup(&attributes, 0);
  }
  if (result != 0) {
    return attribute_failure(result);
  }

  std::vector<std::string> arguments;
  arguments.reserve(request.arguments.size() + 1U);
  arguments.push_back(libtmux_path::command_string(request.executable));
  for (const auto& argument : request.arguments) {
    arguments.push_back(argument.value);
  }
  auto argument_pointers = mutable_pointers(arguments);
  auto environment = environment_overlay(request.environment);
  auto environment_pointers = mutable_pointers(environment);

  pid_t child = -1;
  const auto spawn_result =
      ::posix_spawnp(&child, arguments.front().c_str(), &actions, &attributes,
                     argument_pointers.data(), environment_pointers.data());
  const auto attributes_destroyed = ::posix_spawnattr_destroy(&attributes);
  const auto actions_destroyed = ::posix_spawn_file_actions_destroy(&actions);
  stdout_pipe.write.reset();
  stderr_pipe.write.reset();
  if (spawn_result != 0) {
    const auto kind = spawn_error_kind(spawn_result);
    return unexpected(process_error(kind, DeliveryStatus::not_started,
                                    spawn_operation(kind), rendered,
                                    generic_error(spawn_result)));
  }

  PosixChild launched{child, stdout_pipe.read.release(), stderr_pipe.read.release(),
                      request.capture_limit, std::move(rendered)};
  launched.capture_.stdout_bytes.reserve(
      std::min(request.capture_limit, std::size_t{4096U}));
  launched.capture_.stderr_bytes.reserve(
      std::min(request.capture_limit, std::size_t{4096U}));
  // The child is running either way; a teardown that failed is reported once
  // its owner exists, so nothing is leaked to report it.
  const auto teardown =
      attributes_destroyed != 0 ? attributes_destroyed : actions_destroyed;
  if (teardown != 0) {
    launched.spawn_teardown_error_ = teardown;
  }
  return launched;
}

PosixChild::PosixChild(pid_t pid, int stdout_fd, int stderr_fd,
                       std::size_t capture_limit, std::string rendered) noexcept
    : pid_{pid}, stdout_fd_{stdout_fd}, stderr_fd_{stderr_fd},
      capture_limit_{capture_limit}, rendered_request_{std::move(rendered)},
      status_{ChildStatus::running} {}

PosixChild::~PosixChild() noexcept {
  assert(pid_ < 0 || status_ != ChildStatus::running);
  close_descriptor(ChildStream::stdout_stream);
  close_descriptor(ChildStream::stderr_stream);
}

PosixChild::PosixChild(PosixChild&& other) noexcept
    : pid_{std::exchange(other.pid_, -1)},
      stdout_fd_{std::exchange(other.stdout_fd_, -1)},
      stderr_fd_{std::exchange(other.stderr_fd_, -1)},
      capture_limit_{std::exchange(other.capture_limit_, 0U)},
      rendered_request_{std::move(other.rendered_request_)},
      capture_{std::move(other.capture_)},
      wait_status_{std::exchange(other.wait_status_, 0)},
      status_{std::exchange(other.status_, ChildStatus::none)},
      spawn_teardown_error_{std::exchange(other.spawn_teardown_error_, 0)} {}

PosixChild& PosixChild::operator=(PosixChild&& other) noexcept {
  if (this != &other) {
    assert(pid_ < 0 || status_ != ChildStatus::running);
    close_descriptor(ChildStream::stdout_stream);
    close_descriptor(ChildStream::stderr_stream);
    pid_ = std::exchange(other.pid_, -1);
    stdout_fd_ = std::exchange(other.stdout_fd_, -1);
    stderr_fd_ = std::exchange(other.stderr_fd_, -1);
    capture_limit_ = std::exchange(other.capture_limit_, 0U);
    rendered_request_ = std::move(other.rendered_request_);
    capture_ = std::move(other.capture_);
    wait_status_ = std::exchange(other.wait_status_, 0);
    status_ = std::exchange(other.status_, ChildStatus::none);
    spawn_teardown_error_ = std::exchange(other.spawn_teardown_error_, 0);
  }
  return *this;
}

pid_t PosixChild::pid() const noexcept { return pid_; }

int PosixChild::descriptor(ChildStream stream) const noexcept {
  return stream == ChildStream::stdout_stream ? stdout_fd_ : stderr_fd_;
}

ChildStatus PosixChild::status() const noexcept { return status_; }

bool PosixChild::output_closed() const noexcept {
  return stdout_fd_ < 0 && stderr_fd_ < 0;
}

std::string_view PosixChild::rendered_request() const noexcept {
  return rendered_request_;
}

int PosixChild::spawn_teardown_error() const noexcept { return spawn_teardown_error_; }

void PosixChild::close_descriptor(ChildStream stream) noexcept {
  auto& descriptor = stream == ChildStream::stdout_stream ? stdout_fd_ : stderr_fd_;
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

std::optional<ProcessError> PosixChild::drain(ChildStream stream,
                                              ChildClock::time_point boundary,
                                              DeliveryStatus delivery) noexcept {
  const int descriptor_value = descriptor(stream);
  if (descriptor_value < 0) {
    return std::nullopt;
  }
  auto& destination = stream == ChildStream::stdout_stream ? capture_.stdout_bytes
                                                           : capture_.stderr_bytes;
  std::array<std::byte, 16384> buffer{};
  for (;;) {
    if (ChildClock::now() >= boundary) {
      return std::nullopt;
    }
    const auto count = ::read(descriptor_value, buffer.data(), buffer.size());
    if (count > 0) {
      const auto size = static_cast<std::size_t>(count);
      const auto available = destination.size() < capture_limit_
                                 ? capture_limit_ - destination.size()
                                 : 0U;
      const auto retained = std::min(available, size);
      const std::span<const std::byte> kept{buffer.data(), retained};
      destination.insert(destination.end(), kept.begin(), kept.end());
      capture_.truncated = capture_.truncated || retained != size;
      continue;
    }
    if (count == 0) {
      close_descriptor(stream);
      return std::nullopt;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::nullopt;
    }
    const auto error_number = errno;
    close_descriptor(stream);
    capture_.truncated = true;
    return process_error(ProcessError::Kind::pipe, delivery, "pipe read",
                         rendered_request_, generic_error(error_number));
  }
}

std::optional<ProcessError>
PosixChild::update_status(DeliveryStatus delivery) noexcept {
  if (status_ != ChildStatus::running) {
    return std::nullopt;
  }
  for (;;) {
    const auto result = ::waitpid(pid_, &wait_status_, WNOHANG);
    if (result == pid_) {
      status_ = ChildStatus::exited;
      return std::nullopt;
    }
    if (result == 0) {
      return std::nullopt;
    }
    if (errno == EINTR) {
      continue;
    }
    const auto error_number = errno;
    auto lost = process_error(ProcessError::Kind::pipe, delivery, "waitpid pipe",
                              rendered_request_, generic_error(error_number));
    if (error_number == ECHILD) {
      // A process that has set SIGCHLD to SIG_IGN — routine in a daemon — has
      // told the kernel to reap its children itself, so there is no status
      // left to collect and waitpid answers ECHILD for every command run.
      // "No child processes" alone does not lead anyone to that.
      status_ = ChildStatus::unknowable;
      lost.diagnostic +=
          " (SIGCHLD is ignored in this process, so the child was reaped before "
          "its exit status could be read)";
    }
    return lost;
  }
}

void PosixChild::signal_group(int signal_number) noexcept {
  if (status_ != ChildStatus::running) {
    return;
  }
  while (::kill(-pid_, signal_number) < 0 && errno == EINTR) {
  }
}

void PosixChild::wait_for_exit() noexcept {
  while (status_ == ChildStatus::running) {
    const auto result = ::waitpid(pid_, &wait_status_, 0);
    if (result == pid_) {
      status_ = ChildStatus::exited;
      return;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && errno == ECHILD) {
      status_ = ChildStatus::unknowable;
    }
    return;
  }
}

void PosixChild::close_stream(ChildStream stream) noexcept {
  if (descriptor(stream) >= 0) {
    capture_.truncated = true;
    close_descriptor(stream);
  }
}

void PosixChild::close_output() noexcept {
  close_stream(ChildStream::stdout_stream);
  close_stream(ChildStream::stderr_stream);
}

Capture PosixChild::take_capture() noexcept { return std::move(capture_); }

Termination PosixChild::termination() const noexcept {
  assert(status_ == ChildStatus::exited);
  return WIFEXITED(wait_status_) ? Termination{Exited{WEXITSTATUS(wait_status_)}}
                                 : Termination{Signaled{WTERMSIG(wait_status_)}};
}

} // namespace detail
LIBTMUX_NAMESPACE_END
