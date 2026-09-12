#include "services.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <termios.h>
#include <unistd.h>

#include "process.hpp"

namespace libtmux::workspace::cli {
namespace {
volatile std::sig_atomic_t interrupted_signal{};
void interrupted(int signal) { interrupted_signal = signal; }

class SignalGuard {
  struct sigaction interrupt_ {
  }, terminate_{};

public:
  SignalGuard() {
    struct sigaction action {};
    action.sa_handler = interrupted;
    ::sigemptyset(&action.sa_mask);
    interrupted_signal = 0;
    if (::sigaction(SIGINT, &action, &interrupt_) != 0)
      throw Failure{1, "SIGNAL_HANDLER", "cannot install child interruption handler"};
    if (::sigaction(SIGTERM, &action, &terminate_) != 0) {
      (void)::sigaction(SIGINT, &interrupt_, nullptr);
      throw Failure{1, "SIGNAL_HANDLER", "cannot install child termination handler"};
    }
  }
  ~SignalGuard() {
    (void)::sigaction(SIGTERM, &terminate_, nullptr);
    (void)::sigaction(SIGINT, &interrupt_, nullptr);
  }
  SignalGuard(const SignalGuard&) = delete;
  SignalGuard& operator=(const SignalGuard&) = delete;
};

class Terminal {
  int descriptor_{-1};
  pid_t previous_{-1};
  pid_t child_{-1};
  termios settings_{};

  bool foreground(pid_t group, bool restore_settings = false) const noexcept {
    sigset_t blocked, previous;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGTTOU);
    if (::sigprocmask(SIG_BLOCK, &blocked, &previous) != 0)
      return false;
    const bool changed = ::tcsetpgrp(descriptor_, group) == 0;
    if (changed && restore_settings)
      (void)::tcsetattr(descriptor_, TCSANOW, &settings_);
    (void)::sigprocmask(SIG_SETMASK, &previous, nullptr);
    return changed;
  }

public:
  explicit Terminal(bool allowed, bool concrete = false) {
    if (!allowed)
      return;
    descriptor_ = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (descriptor_ < 0)
      return;
    if (concrete) {
      const auto session = ::tcgetsid(descriptor_);
      int terminal{-1};
      for (int source = 0; session >= 0 && source < 3; ++source) {
        std::array<char, PATH_MAX> path{};
        if (::tcgetsid(source) != session ||
            ::ttyname_r(source, path.data(), path.size()) != 0 ||
            std::strcmp(path.data(), "/dev/tty") == 0)
          continue;
        terminal = ::open(path.data(), O_RDWR | O_CLOEXEC | O_NOCTTY);
        if (terminal >= 0 && ::tcgetsid(terminal) == session)
          break;
        if (terminal >= 0)
          ::close(terminal);
        terminal = -1;
      }
      ::close(descriptor_);
      descriptor_ = terminal;
      if (descriptor_ < 0)
        return;
    }
    previous_ = ::tcgetpgrp(descriptor_);
    if (previous_ != ::getpgrp()) {
      ::close(descriptor_);
      descriptor_ = -1;
      throw Failure{1, "TERMINAL_BACKGROUND", "command requires a foreground terminal"};
    }
    if (::tcgetattr(descriptor_, &settings_) != 0) {
      ::close(descriptor_);
      descriptor_ = -1;
      throw Failure{1, "TERMINAL_SETTINGS", "cannot read terminal settings"};
    }
  }
  ~Terminal() {
    if (descriptor_ >= 0) {
      if (child_ > 0 && ::tcgetpgrp(descriptor_) == child_)
        (void)foreground(previous_, true);
      ::close(descriptor_);
    }
  }
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;
  int descriptor() const { return descriptor_; }
  std::optional<std::string> handoff(int child) {
    child_ = child;
    if (!foreground(child_))
      return "terminal foreground handoff failed";
    // A reader may have stopped with SIGTTIN before the handoff completed.
    if (::kill(-child_, SIGCONT) != 0 && errno != ESRCH)
      return "terminal child could not resume";
    return std::nullopt;
  }
};
std::string text(const std::vector<std::byte>& bytes) {
  if (bytes.empty())
    return {};
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::size_t complete_prefix(std::string_view bytes) {
  if (bytes.empty())
    return 0;
  auto first = bytes.size() - 1;
  while (first > 0 && (static_cast<unsigned char>(bytes[first]) & 0xc0U) == 0x80U)
    --first;
  const auto lead = static_cast<unsigned char>(bytes[first]);
  const std::size_t width = lead >= 0xc2U && lead <= 0xdfU   ? 2
                            : lead >= 0xe0U && lead <= 0xefU ? 3
                            : lead >= 0xf0U && lead <= 0xf4U ? 4
                                                             : 1;
  return bytes.size() - first < width ? first : bytes.size();
}
} // namespace
std::vector<std::string> split_command(const std::string& value) {
  std::vector<std::string> result;
  std::string word;
  char quote{};
  bool active{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (character == '\\' && quote != '\'') {
      if (++index == value.size())
        throw Failure{2, "USAGE", "incomplete escape in command"};
      if (value[index] == '\n')
        continue;
      if (quote == '"' && value[index] != '"' && value[index] != '\\' &&
          value[index] != '$' && value[index] != '`')
        word += '\\';
      word += value[index];
      active = true;
    } else if (quote) {
      if (character == quote)
        quote = 0;
      else
        word += character;
    } else if (character == '\'' || character == '"') {
      quote = character;
      active = true;
    } else if (character == ' ' || character == '\t' || character == '\n') {
      if (active)
        result.push_back(std::move(word));
      word.clear();
      active = false;
    } else {
      word += character;
      active = true;
    }
  }
  if (quote)
    throw Failure{2, "USAGE", "unterminated quote in command"};
  if (active)
    result.push_back(std::move(word));
  if (result.empty() || result.front().empty())
    throw Failure{2, "USAGE", "command needs an executable"};
  return result;
}
void require_terminal() {
  const Terminal display{true, true};
  if (display.descriptor() < 0)
    throw Failure{2, "USAGE",
                  "load requires a foreground controlling terminal; use -d"};
}
ChildOutput run_child(const std::vector<std::string>& arguments, ChildOptions options) {
  if (arguments.empty())
    throw Failure{2, "USAGE", "child command is empty"};
  SignalGuard signals;
  Terminal display{options.terminal, options.terminal_required};
  if (options.terminal_required && display.descriptor() < 0)
    throw Failure{1, "TERMINAL_UNAVAILABLE", "controlling terminal is unavailable"};
  libtmux::detail::ProcessRequest request;
  request.executable = arguments.front();
  request.timeout = options.timeout;
  request.working_directory = options.directory;
  request.fail_on_capture_limit = true;
  request.cancelled = [] { return interrupted_signal != 0; };
  if (display.descriptor() >= 0) {
    request.stdio = libtmux::detail::StdioPolicy::inherit_terminal;
    request.terminal_descriptors.fill(display.descriptor());
    request.on_started = [&display](int child) { return display.handoff(child); };
  }
  for (auto argument = arguments.begin() + 1; argument != arguments.end(); ++argument)
    request.arguments.push_back({*argument});
  std::array<std::string, 2> pending;
  const auto output = [&](std::string_view stream, std::string_view bytes) {
    if (!options.output)
      return;
    auto& buffer = pending[stream == "stdout" ? 0 : 1];
    buffer += bytes;
    const auto size = complete_prefix(buffer);
    if (size != 0) {
      options.output(stream, std::string_view{buffer}.substr(0, size));
      buffer.erase(0, size);
    }
  };
  const auto reply = libtmux::detail::run_process(
      request, output,
      options.terminate_descendants ? libtmux::detail::DescendantPolicy::terminate
                                    : libtmux::detail::DescendantPolicy::leave_running);
  for (std::size_t index = 0; index < pending.size(); ++index)
    if (!pending[index].empty())
      options.output(index == 0 ? "stdout" : "stderr", pending[index]);
  if (!reply) {
    if (reply.error().kind == libtmux::detail::ProcessError::Kind::cancelled)
      return {128 + interrupted_signal, text(reply.error().stdout_bytes),
              text(reply.error().stderr_bytes), reply.error().output_truncated};
    const bool limited =
        reply.error().kind == libtmux::detail::ProcessError::Kind::output_limit;
    Failure failure{1, limited ? "OUTPUT_LIMIT" : "PROCESS_FAILED",
                    limited ? "child output exceeds 1 MiB per stream"
                            : reply.error().diagnostic};
    failure.child_output =
        ChildOutput{1, text(reply.error().stdout_bytes),
                    text(reply.error().stderr_bytes), reply.error().output_truncated}
            .value();
    throw failure;
  }
  if (reply->output_truncated) {
    Failure failure{1, "OUTPUT_LIMIT", "child output exceeds 1 MiB"};
    failure.child_output =
        ChildOutput{1, text(reply->stdout_bytes), text(reply->stderr_bytes), true}
            .value();
    throw failure;
  }
  const int status =
      std::holds_alternative<libtmux::detail::Exited>(reply->termination)
          ? std::get<libtmux::detail::Exited>(reply->termination).code
          : 128 + std::get<libtmux::detail::Signaled>(reply->termination).signal;
  return {status, text(reply->stdout_bytes), text(reply->stderr_bytes)};
}
} // namespace libtmux::workspace::cli
