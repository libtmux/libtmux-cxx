#pragma once

// Run a program, capture what it said, and report how it ended.
//
// `libtmux::testing` has a better process supervisor — pidfd reaping,
// descriptor accounting, interposed-syscall failure injection — and does not
// export it. This is the duplication that costs.

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libtmux/expected.hpp>

namespace libtmux::examples {

struct ProgramResult {
  int exit_code{};
  std::string output;
};

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

// stdout and stderr are merged: a failing example's diagnostic and the output
// leading up to it belong in one stream, in order, in the failure message.
inline libtmux::expected<ProgramResult, std::string>
run_program(const std::filesystem::path& program, std::vector<std::string> environment,
            std::chrono::seconds timeout) {
  std::array<int, 2> pipe_ends{};
  if (::pipe(pipe_ends.data()) != 0) {
    return libtmux::unexpected(std::string{"pipe: "} + std::strerror(errno));
  }

  posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  ::posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
  ::posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);

  std::vector<std::string> arguments{program.string()};
  auto argument_pointers = detail::writable(arguments);
  auto environment_pointers = detail::writable(environment);

  pid_t child = 0;
  const int spawned =
      ::posix_spawn(&child, program.c_str(), &actions, nullptr,
                    argument_pointers.data(), environment_pointers.data());
  ::posix_spawn_file_actions_destroy(&actions);
  ::close(pipe_ends[1]);
  if (spawned != 0) {
    ::close(pipe_ends[0]);
    return libtmux::unexpected(program.string() + ": " + std::strerror(spawned));
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string output;
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      ::kill(child, SIGKILL);
      ::waitpid(child, nullptr, 0);
      ::close(pipe_ends[0]);
      return libtmux::unexpected(program.string() + " did not finish in time");
    }
    pollfd waiting{.fd = pipe_ends[0], .events = POLLIN, .revents = 0};
    if (::poll(&waiting, 1, static_cast<int>(remaining.count())) <= 0) {
      continue;
    }
    std::array<char, 4096> buffer{};
    const auto read_bytes = ::read(pipe_ends[0], buffer.data(), buffer.size());
    if (read_bytes <= 0) {
      break;
    }
    output.append(buffer.data(), static_cast<std::size_t>(read_bytes));
  }
  ::close(pipe_ends[0]);

  int status = 0;
  if (::waitpid(child, &status, 0) < 0) {
    return libtmux::unexpected(std::string{"waitpid: "} + std::strerror(errno));
  }
  if (WIFSIGNALED(status)) {
    return libtmux::unexpected(program.string() + " was killed by signal " +
                               std::to_string(WTERMSIG(status)) + '\n' + output);
  }
  return ProgramResult{WEXITSTATUS(status), std::move(output)};
}

} // namespace libtmux::examples
