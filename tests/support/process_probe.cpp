#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool write_all(int descriptor, const void* data, std::size_t size) {
  const auto* cursor = static_cast<const char*>(data);
  while (size != 0U) {
    const auto count = ::write(descriptor, cursor, size);
    if (count > 0) {
      const auto written = static_cast<std::size_t>(count);
      cursor += written;
      size -= written;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

int append_environment_marker(const char* name, std::string_view contents) {
  const auto* path = std::getenv(name);
  if (path == nullptr) {
    return 0;
  }
  const auto descriptor = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (descriptor < 0) {
    return 14;
  }
  const auto wrote = write_all(descriptor, contents.data(), contents.size());
  const auto close_result = ::close(descriptor);
  return wrote && close_result == 0 ? 0 : 14;
}

int parsed_integer(std::string_view value) {
  int result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return -1;
  }
  return result;
}

int write_large_output(std::size_t byte_count) {
  std::array<char, 4096> stdout_block{};
  std::array<char, 4096> stderr_block{};
  stdout_block.fill('o');
  stderr_block.fill('e');
  const auto stderr_writer = ::fork();
  if (stderr_writer < 0) {
    return 3;
  }
  if (stderr_writer == 0) {
    std::size_t written = 0;
    while (written < byte_count) {
      const auto block_size = std::min(stderr_block.size(), byte_count - written);
      if (!write_all(STDERR_FILENO, stderr_block.data(), block_size)) {
        std::_Exit(15);
      }
      written += block_size;
    }
    std::_Exit(0);
  }

  std::size_t written = 0;
  while (written < byte_count) {
    const auto block_size = std::min(stdout_block.size(), byte_count - written);
    if (!write_all(STDOUT_FILENO, stdout_block.data(), block_size)) {
      static_cast<void>(::kill(stderr_writer, SIGKILL));
      static_cast<void>(::waitpid(stderr_writer, nullptr, 0));
      return 3;
    }
    written += block_size;
  }
  int status = 0;
  while (::waitpid(stderr_writer, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 15;
}

[[noreturn]] void wait_forever() {
  for (;;) {
    static_cast<void>(::poll(nullptr, 0, 1000));
  }
}

int report_pid_and_wait(bool resist_term) {
  if (resist_term) {
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGTERM, &action, nullptr) != 0) {
      return 4;
    }
  }
  const auto line = std::string{"PID="} + std::to_string(::getpid()) + "\n";
  if (!write_all(STDERR_FILENO, line.data(), line.size())) {
    return 5;
  }
  wait_forever();
}

int report_stream_then_wait() {
  const auto line =
      std::string{"PID="} + std::to_string(::getpid()) + "\npartial-output\n";
  if (!write_all(STDERR_FILENO, line.data(), line.size())) {
    return 16;
  }
  wait_forever();
}

int term_resistant_group() {
  const auto* trace_path = std::getenv("LIBTMUX_PROCESS_TRACE");
  if (trace_path == nullptr) {
    return 17;
  }
  const auto trace_fd = ::open(trace_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (trace_fd < 0) {
    return 17;
  }
  std::array<int, 2> ready{-1, -1};
  if (::pipe(ready.data()) != 0) {
    static_cast<void>(::close(trace_fd));
    return 17;
  }

  static const char direct_term[] = "TERM=direct\n";
  static const char descendant_term[] = "TERM=descendant\n";
  static const char* term_text = direct_term;
  static std::size_t term_size = sizeof(direct_term) - 1U;
  const auto handler = [](int /*signal_number*/) {
    static_cast<void>(write_all(STDERR_FILENO, term_text, term_size));
  };
  struct sigaction action {};
  action.sa_handler = handler;
  ::sigemptyset(&action.sa_mask);
  if (::sigaction(SIGTERM, &action, nullptr) != 0) {
    static_cast<void>(::close(ready[0]));
    static_cast<void>(::close(ready[1]));
    static_cast<void>(::close(trace_fd));
    return 17;
  }

  const auto descendant = ::fork();
  if (descendant < 0) {
    static_cast<void>(::close(ready[0]));
    static_cast<void>(::close(ready[1]));
    static_cast<void>(::close(trace_fd));
    return 17;
  }
  if (descendant == 0) {
    static_cast<void>(::close(ready[0]));
    term_text = descendant_term;
    term_size = sizeof(descendant_term) - 1U;
    constexpr char ready_byte = 'r';
    static_cast<void>(write_all(ready[1], &ready_byte, sizeof(ready_byte)));
    static_cast<void>(::close(ready[1]));
    wait_forever();
  }

  static_cast<void>(::close(ready[1]));
  char ready_byte = 0;
  const auto ready_count = ::read(ready[0], &ready_byte, sizeof(ready_byte));
  static_cast<void>(::close(ready[0]));
  if (ready_count != 1 || ready_byte != 'r') {
    static_cast<void>(::kill(descendant, SIGKILL));
    static_cast<void>(::waitpid(descendant, nullptr, 0));
    static_cast<void>(::close(trace_fd));
    return 17;
  }
  const auto identities = std::string{"PID="} + std::to_string(::getpid()) +
                          " DESCENDANT=" + std::to_string(descendant) + "\n";
  if (!write_all(trace_fd, identities.data(), identities.size())) {
    return 17;
  }
  static_cast<void>(::close(trace_fd));
  const auto output = identities + "ready\n";
  if (!write_all(STDERR_FILENO, output.data(), output.size())) {
    return 17;
  }
  wait_forever();
}

int escape_with_descriptors() {
  std::array<int, 2> ready{-1, -1};
  if (::pipe(ready.data()) != 0) {
    return 6;
  }
  const auto descendant = ::fork();
  if (descendant < 0) {
    static_cast<void>(::close(ready[0]));
    static_cast<void>(::close(ready[1]));
    return 6;
  }
  if (descendant == 0) {
    static_cast<void>(::close(ready[0]));
    if (::setsid() < 0) {
      std::_Exit(7);
    }
    constexpr char ready_byte = 'r';
    static_cast<void>(write_all(ready[1], &ready_byte, sizeof(ready_byte)));
    static_cast<void>(::close(ready[1]));
    static_cast<void>(::poll(nullptr, 0, 4000));
    std::_Exit(0);
  }
  static_cast<void>(::close(ready[1]));
  char ready_byte = 0;
  const auto ready_count = ::read(ready[0], &ready_byte, sizeof(ready_byte));
  static_cast<void>(::close(ready[0]));
  if (ready_count != 1 || ready_byte != 'r') {
    static_cast<void>(::kill(descendant, SIGKILL));
    return 7;
  }
  const auto marker = std::string{"PID="} + std::to_string(::getpid()) +
                      " HOLDER=" + std::to_string(descendant) + "\n";
  return write_all(STDERR_FILENO, marker.data(), marker.size()) ? 0 : 7;
}

int terminal_probe() {
  if (::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0) {
    return 8;
  }
  constexpr std::string_view stdout_marker{"terminal-stdout\n"};
  constexpr std::string_view stderr_marker{"terminal-stderr\n"};
  return write_all(STDOUT_FILENO, stdout_marker.data(), stdout_marker.size()) &&
                 write_all(STDERR_FILENO, stderr_marker.data(), stderr_marker.size())
             ? 0
             : 9;
}

int descriptor_closed(std::string_view value) {
  const auto descriptor = parsed_integer(value);
  if (descriptor < 0) {
    return 19;
  }
  errno = 0;
  if (::fcntl(descriptor, F_GETFD) != -1 || errno != EBADF) {
    return 19;
  }
  constexpr std::string_view marker{"descriptor-closed\n"};
  return write_all(STDOUT_FILENO, marker.data(), marker.size()) ? 0 : 19;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return 2;
  }
  const auto launch_result = append_environment_marker(
      "LIBTMUX_PROCESS_LAUNCH_MARKER",
      std::string{"PID="} + std::to_string(::getpid()) + "\n");
  if (launch_result != 0) {
    return launch_result;
  }
  const std::string_view mode{argv[1]};
  if (mode == "normal") {
    constexpr std::string_view stdout_text{"stdout-probe\n"};
    constexpr std::string_view stderr_text{"stderr-probe\n"};
    return write_all(STDOUT_FILENO, stdout_text.data(), stdout_text.size()) &&
                   write_all(STDERR_FILENO, stderr_text.data(), stderr_text.size())
               ? 0
               : 3;
  }
  if (mode == "exit" && argc == 3) {
    const auto line = std::string{"PID="} + std::to_string(::getpid()) + "\n";
    return write_all(STDERR_FILENO, line.data(), line.size()) ? parsed_integer(argv[2])
                                                              : 3;
  }
  if (mode == "signal") {
    return ::raise(SIGUSR1) == 0 ? 10 : 11;
  }
  if (mode == "binary") {
    constexpr std::array<unsigned char, 5> stdout_bytes{0x00, 0xff, 0x41, 0x80, 0x0a};
    constexpr std::array<unsigned char, 5> stderr_bytes{0xfe, 0x00, 0x42, 0x7f, 0x0a};
    return write_all(STDOUT_FILENO, stdout_bytes.data(), stdout_bytes.size()) &&
                   write_all(STDERR_FILENO, stderr_bytes.data(), stderr_bytes.size())
               ? 0
               : 12;
  }
  if (mode == "large" && argc == 3) {
    const auto byte_count = parsed_integer(argv[2]);
    return byte_count >= 0 ? write_large_output(static_cast<std::size_t>(byte_count))
                           : 2;
  }
  if (mode == "wait") {
    return report_pid_and_wait(false);
  }
  if (mode == "resist-term") {
    return report_pid_and_wait(true);
  }
  if (mode == "stream-then-wait") {
    return report_stream_then_wait();
  }
  if (mode == "term-group") {
    return term_resistant_group();
  }
  if (mode == "escaped-holder") {
    return escape_with_descriptors();
  }
  if (mode == "terminal") {
    return terminal_probe();
  }
  if (mode == "descriptor-closed" && argc == 3) {
    return descriptor_closed(argv[2]);
  }
  if (mode == "environment" && argc == 3) {
    const auto* value = std::getenv(argv[2]);
    const std::string rendered =
        value == nullptr ? "<unset>\n" : std::string{value} + "\n";
    return write_all(STDOUT_FILENO, rendered.data(), rendered.size()) ? 0 : 13;
  }
  if (mode == "arguments") {
    for (int index = 2; index < argc; ++index) {
      const auto argument = std::string{argv[index]} + "\n";
      if (!write_all(STDOUT_FILENO, argument.data(), argument.size())) {
        return 18;
      }
    }
    return 0;
  }
  return 2;
}
