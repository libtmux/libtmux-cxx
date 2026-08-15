#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <poll.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int /*signal_number*/) { stop_requested = 1; }

std::optional<std::string> environment_value(const char* name) {
  const auto* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string{value};
}

void append_trace(const std::vector<std::string>& fields) {
  const auto trace = environment_value("LIBTMUX_FAKE_TRACE");
  if (!trace.has_value()) {
    return;
  }
  std::ofstream output{*trace, std::ios::app};
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0U) {
      output << '\t';
    }
    output << fields[index];
  }
  output << '\n';
}

std::string shown_environment(const char* name) {
  auto value = environment_value(name);
  return value.has_value() ? *value : "<unset>";
}

std::filesystem::path name_metadata_path(std::string_view name) {
  const auto root = environment_value("TMUX_TMPDIR").value_or("/tmp");
  return std::filesystem::path{root} / ("fake-name-" + std::string{name} + ".meta");
}

std::filesystem::path socket_metadata_path(const std::filesystem::path& socket) {
  auto metadata = socket;
  metadata += ".meta";
  return metadata;
}

void record_session_mutation(pid_t pid) {
  const auto marker = environment_value("LIBTMUX_FAKE_MUTATION_MARKER");
  if (!marker.has_value()) {
    return;
  }
  std::ofstream output{*marker, std::ios::app};
  output << pid << '\n';
}

struct Selector {
  std::optional<std::string> name;
  std::optional<std::filesystem::path> path;
};

void rebind_path_metadata(const Selector& selector,
                          const std::filesystem::path& socket) {
  const auto replacement = environment_value("LIBTMUX_FAKE_REBIND_PID");
  if (!selector.path.has_value() || !replacement.has_value()) {
    return;
  }
  std::ofstream output{socket_metadata_path(*selector.path), std::ios::trunc};
  output << std::stol(*replacement) << '\n' << socket.string() << '\n';
}

Selector parse_selector(const std::vector<std::string>& arguments) {
  Selector selector;
  for (std::size_t index = 0; index + 1U < arguments.size(); ++index) {
    if (arguments[index] == "-L") {
      selector.name = arguments[index + 1U];
    } else if (arguments[index] == "-S") {
      selector.path = arguments[index + 1U];
    }
  }
  return selector;
}

std::optional<std::pair<pid_t, std::filesystem::path>>
read_metadata(const Selector& selector) {
  std::filesystem::path metadata;
  if (selector.name.has_value()) {
    metadata = name_metadata_path(*selector.name);
  } else if (selector.path.has_value()) {
    metadata = socket_metadata_path(*selector.path);
  } else {
    return std::nullopt;
  }
  std::ifstream input{metadata};
  long raw_pid = 0;
  std::string socket;
  if (!(input >> raw_pid >> socket)) {
    return std::nullopt;
  }
  return std::pair{static_cast<pid_t>(raw_pid), std::filesystem::path{socket}};
}

std::vector<std::string> trace_fields(std::string role,
                                      const std::vector<std::string>& arguments) {
  std::vector<std::string> fields;
  fields.reserve(arguments.size() + 7U);
  fields.push_back(std::move(role));
  fields.insert(fields.end(), arguments.begin(), arguments.end());
  fields.push_back("HOME=" + shown_environment("HOME"));
  // The shell a pane would run, which decides whether a test sees output or
  // an interactive first-run wizard.
  fields.push_back("SHELL=" + shown_environment("SHELL"));
  fields.push_back("TMUX=" + shown_environment("TMUX"));
  fields.push_back("TMUX_PANE=" + shown_environment("TMUX_PANE"));
  fields.push_back("TMUX_TMPDIR=" + shown_environment("TMUX_TMPDIR"));
  return fields;
}

void write_large_output() {
  std::array<char, 4096> stdout_block{};
  std::array<char, 4096> stderr_block{};
  stdout_block.fill('o');
  stderr_block.fill('e');
  for (int index = 0; index < 64; ++index) {
    std::cout.write(stdout_block.data(),
                    static_cast<std::streamsize>(stdout_block.size()));
    std::cerr.write(stderr_block.data(),
                    static_cast<std::streamsize>(stderr_block.size()));
    std::cout.flush();
    std::cerr.flush();
  }
}

void write_continuous_output() {
  constexpr int writer_count = 8;
  std::vector<pid_t> writers;
  writers.reserve(writer_count);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{400};
  for (int index = 0; index < writer_count; ++index) {
    const auto child = ::fork();
    if (child == 0) {
      std::array<char, 4096> block{};
      block.fill('c');
      while (std::chrono::steady_clock::now() < deadline) {
        const auto count = ::write(STDOUT_FILENO, block.data(), block.size());
        if (count < 0 && errno != EINTR) {
          break;
        }
      }
      std::_Exit(0);
    }
    if (child > 0) {
      writers.push_back(child);
    }
  }
  for (const auto writer : writers) {
    while (::waitpid(writer, nullptr, 0) < 0 && errno == EINTR) {
    }
  }
}

int run_process_probe(const std::vector<std::string>& arguments) {
  if (arguments.size() < 2U) {
    return 2;
  }
  append_trace({"process-probe", "PID=" + std::to_string(::getpid())});
  if (arguments[1] == "streams") {
    std::cout << "stdout-probe\n";
    std::cerr << "stderr-probe\n";
    return 0;
  }
  if (arguments[1] == "large-output") {
    write_large_output();
    return 0;
  }
  if (arguments[1] == "continuous-output") {
    write_continuous_output();
    return 0;
  }
  if (arguments[1] == "escaped-holder") {
    const auto child = ::fork();
    if (child == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds{800});
      std::_Exit(0);
    }
    return child < 0 ? 3 : 0;
  }
#if defined(__linux__)
  if (arguments[1] == "ptrace-wait") {
    if (::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0) {
      return 4;
    }
    append_trace({"ptrace-ready", "PID=" + std::to_string(::getpid())});
    for (;;) {
      ::pause();
    }
  }
#endif
  if (arguments[1] == "wait") {
    for (;;) {
      ::pause();
    }
  }
  return 2;
}

int run_server(const std::vector<std::string>& arguments, const Selector& selector) {
  const auto mode = environment_value("LIBTMUX_FAKE_MODE").value_or("normal");
  std::filesystem::path socket;
  std::filesystem::path selector_metadata;
  if (selector.path.has_value()) {
    socket = *selector.path;
    selector_metadata = socket_metadata_path(socket);
  } else if (selector.name.has_value()) {
    const auto root = environment_value("TMUX_TMPDIR").value_or("/tmp");
    socket = std::filesystem::path{root} / ("resolved-" + *selector.name + ".sock");
    selector_metadata = name_metadata_path(*selector.name);
  } else {
    return 2;
  }

#if defined(__linux__)
  if (mode == "ptrace-reap-delay" &&
      ::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0) {
    return 5;
  }
#endif

  auto reported_socket = socket;
  if (mode == "path-mismatch") {
    reported_socket = socket.parent_path() / "different-socket";
  } else if (mode == "name-path-escape") {
    reported_socket = std::filesystem::path{"/tmp"} /
                      ("libtmux-unowned-" + std::to_string(::getpid()) + ".sock");
  }

  std::error_code error;
  std::filesystem::create_directories(socket.parent_path(), error);
  {
    std::ofstream socket_marker{socket};
    socket_marker << ::getpid() << '\n';
  }
  if (reported_socket != socket) {
    std::ofstream socket_marker{reported_socket};
    socket_marker << ::getpid() << '\n';
  }
  const auto metadata = socket_metadata_path(socket);
  const auto reported_metadata = socket_metadata_path(reported_socket);
  for (const auto& destination : {selector_metadata, metadata, reported_metadata}) {
    std::ofstream output{destination};
    output << ::getpid() << '\n' << reported_socket.string() << '\n';
  }

  auto fields = trace_fields("server", arguments);
  fields.push_back("PID=" + std::to_string(::getpid()));
  fields.push_back("SOCKET=" + socket.string());
  fields.push_back("REPORTED_SOCKET=" + reported_socket.string());
  append_trace(fields);

  if (mode == "large-output") {
    write_large_output();
  }
  if (mode == "escaped-holder") {
    const auto descendant = ::fork();
    if (descendant == 0) {
      // Lives until killed, rather than for a fixed interval. The test proves
      // teardown neither waited for this process nor signalled it by finding
      // it alive afterwards, and a descendant that exits on a timer makes
      // that proof a race against how long teardown happened to take.
      for (;;) {
        ::pause();
      }
    }
    if (descendant > 0) {
      append_trace({"descendant", "PID=" + std::to_string(descendant)});
    }
  }

  struct sigaction action {};
  action.sa_handler =
      mode == "term-resistant" || mode == "ptrace-reap-delay" ? SIG_IGN : request_stop;
  sigemptyset(&action.sa_mask);
  if (::sigaction(SIGTERM, &action, nullptr) != 0) {
    return 4;
  }

  const auto started = std::chrono::steady_clock::now();
  while (stop_requested == 0) {
    if (mode == "self-exit" &&
        std::chrono::steady_clock::now() - started > std::chrono::milliseconds{250}) {
      break;
    }
    ::poll(nullptr, 0, 10);
  }

  if (mode != "self-exit") {
    std::filesystem::remove(socket, error);
    std::filesystem::remove(metadata, error);
    std::filesystem::remove(selector_metadata, error);
    std::filesystem::remove(reported_socket, error);
    std::filesystem::remove(reported_metadata, error);
  }
  return 0;
}

int run_client(const std::vector<std::string>& arguments, const Selector& selector) {
  append_trace(trace_fields("client", arguments));
  const auto metadata = read_metadata(selector);
  if (!metadata.has_value()) {
    return 1;
  }
  const auto mode = environment_value("LIBTMUX_FAKE_MODE").value_or("normal");
  if (mode == "query-failure") {
    return 1;
  }

  std::string command;
  for (const auto& argument : arguments) {
    if (argument == "display-message" || argument == "new-session" ||
        argument == "has-session" || argument == "if-shell" ||
        argument == "kill-server") {
      command = argument;
      break;
    }
  }
  if (command == "display-message") {
    if (!arguments.empty() && arguments.back() == "#{pid}") {
      std::cout << metadata->first << '\n';
      std::cout.flush();
      if (mode == "startup-rebind") {
        rebind_path_metadata(selector, metadata->second);
      }
    } else if (!arguments.empty() && arguments.back() == "#{socket_path}") {
      std::cout << metadata->second.string() << '\n';
    } else {
      return 2;
    }
    return 0;
  }
  if (command == "new-session") {
    record_session_mutation(metadata->first);
    return 0;
  }
  if (command == "has-session") {
    return ::kill(metadata->first, 0) == 0 ? 0 : 1;
  }
  if (command == "if-shell") {
    const auto condition = "#{==:#{pid}," + std::to_string(metadata->first) + "}";
    const auto condition_matches =
        std::find(arguments.begin(), arguments.end(), condition) != arguments.end();
    const auto creates_session =
        std::ranges::any_of(arguments, [](const std::string& argument) {
          return argument.starts_with("new-session ");
        });
    if (creates_session) {
      if (condition_matches) {
        record_session_mutation(metadata->first);
        std::cout << "libtmux-session-created\n";
      } else {
        std::cout << "libtmux-session-rejected\n";
      }
      return 0;
    }
    if (condition_matches && mode != "term-resistant" && mode != "ptrace-reap-delay") {
      static_cast<void>(::kill(metadata->first, SIGTERM));
    }
    return 0;
  }
  if (command == "kill-server") {
    if (mode != "term-resistant") {
      static_cast<void>(::kill(metadata->first, SIGTERM));
    }
    return 0;
  }
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  if (arguments.size() > 1U && arguments[1] == "--process-probe") {
    return run_process_probe(
        std::vector<std::string>{arguments.begin() + 1, arguments.end()});
  }

  const auto selector = parse_selector(arguments);
  const bool is_server =
      std::find(arguments.begin(), arguments.end(), "-D") != arguments.end();
  return is_server ? run_server(arguments, selector) : run_client(arguments, selector);
}
