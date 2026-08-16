#pragma once

// Run the MCP server against a socket, write a script to its stdin, and
// collect what it wrote back.
//
// Specific rather than general on purpose: the server reads stdin to end and
// answers on stdout, so this needs one write, one read, and a reap. The
// examples' suite has its own runner for a different shape, and neither is
// worth turning into a shared subprocess library.

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libtmux/expected.hpp>

namespace libtmux::mcp::test {

namespace detail {

inline std::vector<char*> writable(std::vector<std::string>& entries) {
  std::vector<char*> pointers;
  pointers.reserve(entries.size() + 1U);
  for (auto& entry : entries) {
    pointers.push_back(entry.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

} // namespace detail

// stdout only. The server's diagnostics go to stderr and are left on the
// suite's, where a failure shows them.
inline libtmux::expected<std::string, std::string>
run_server(const std::filesystem::path& program, std::vector<std::string> arguments,
           std::vector<std::string> environment, const std::string& input,
           std::chrono::seconds timeout) {
  std::array<int, 2> to_child{};
  std::array<int, 2> from_child{};
  if (::pipe(to_child.data()) != 0 || ::pipe(from_child.data()) != 0) {
    return libtmux::unexpected(std::string{"pipe: "} + std::strerror(errno));
  }

  posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, to_child[1]);
  ::posix_spawn_file_actions_addclose(&actions, from_child[0]);

  std::vector<std::string> argv{program.string()};
  argv.insert(argv.end(), arguments.begin(), arguments.end());
  auto argv_pointers = detail::writable(argv);
  auto environment_pointers = detail::writable(environment);

  pid_t child = 0;
  const int spawned = ::posix_spawn(&child, program.c_str(), &actions, nullptr,
                                    argv_pointers.data(), environment_pointers.data());
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(to_child[0]);
  ::close(from_child[1]);
  if (spawned != 0) {
    ::close(to_child[1]);
    ::close(from_child[0]);
    return libtmux::unexpected(program.string() + ": " + std::strerror(spawned));
  }

  // SIGPIPE would kill the suite if the server exited early; a short write is
  // reported instead.
  static_cast<void>(::signal(SIGPIPE, SIG_IGN));
  std::size_t written = 0;
  while (written < input.size()) {
    const auto wrote =
        ::write(to_child[1], input.data() + written, input.size() - written);
    if (wrote <= 0) {
      break;
    }
    written += static_cast<std::size_t>(wrote);
  }
  ::close(to_child[1]);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string output;
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
      ::close(from_child[0]);
      return libtmux::unexpected(program.string() + " did not finish in time");
    }
    pollfd waiting{.fd = from_child[0], .events = POLLIN, .revents = 0};
    if (::poll(&waiting, 1, static_cast<int>(remaining.count())) <= 0) {
      continue;
    }
    std::array<char, 4096> buffer{};
    const auto read_bytes = ::read(from_child[0], buffer.data(), buffer.size());
    if (read_bytes <= 0) {
      break;
    }
    output.append(buffer.data(), static_cast<std::size_t>(read_bytes));
  }
  ::close(from_child[0]);

  int status = 0;
  if (::waitpid(child, &status, 0) < 0) {
    return libtmux::unexpected(std::string{"waitpid: "} + std::strerror(errno));
  }
  if (WIFSIGNALED(status)) {
    return libtmux::unexpected(program.string() + " was killed by signal " +
                               std::to_string(WTERMSIG(status)));
  }
  return output;
}

} // namespace libtmux::mcp::test
