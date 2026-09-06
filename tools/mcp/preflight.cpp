#include "mcp_swap.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace libtmux::mcp_swap {
namespace {

using Json = nlohmann::json;
constexpr std::size_t output_limit = 1024U * 1024U;
constexpr auto response_stability = std::chrono::milliseconds{25};

struct Pipe {
  int read{-1};
  int write{-1};
};

Pipe make_pipe() {
  std::array<int, 2> descriptors{};
  if (::pipe(descriptors.data()) != 0) {
    throw Error{"could not create preflight pipe: " +
                std::string{std::strerror(errno)}};
  }
  for (const int descriptor : descriptors) {
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
      const int saved = errno;
      static_cast<void>(::close(descriptors[0]));
      static_cast<void>(::close(descriptors[1]));
      errno = saved;
      throw Error{"could not secure preflight pipe: " +
                  std::string{std::strerror(errno)}};
    }
  }
  return {descriptors[0], descriptors[1]};
}

void close_if_open(int& descriptor) {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

void write_request(int descriptor, std::string_view request) {
  std::size_t offset = 0;
  while (offset < request.size()) {
    const auto count =
        ::write(descriptor, request.data() + offset, request.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EPIPE) {
        return;
      }
      throw Error{"could not write MCP preflight request: " +
                  std::string{std::strerror(errno)}};
    }
    offset += static_cast<std::size_t>(count);
  }
}

void set_nonblocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    throw Error{"could not configure preflight pipe: " +
                std::string{std::strerror(errno)}};
  }
}

bool drain(int descriptor, std::string& output, bool& overflow) {
  std::array<char, 8192> buffer{};
  for (;;) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count > 0) {
      const auto length = static_cast<std::size_t>(count);
      if (output.size() + length > output_limit) {
        overflow = true;
        return true;
      } else {
        output.append(buffer.data(), length);
      }
      continue;
    }
    if (count == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    throw Error{"could not read MCP preflight response: " +
                std::string{std::strerror(errno)}};
  }
}

std::string request_frames() {
  const std::array frames{
      Json{{"jsonrpc", "2.0"},
           {"id", 1},
           {"method", "initialize"},
           {"params",
            {{"protocolVersion", "2025-06-18"},
             {"capabilities", Json::object()},
             {"clientInfo", {{"name", "mcp_swap-preflight"}, {"version", "1"}}}}}},
      Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
      Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
  };
  std::string result;
  for (const auto& frame : frames) {
    result += frame.dump();
    result.push_back('\n');
  }
  return result;
}

std::string stderr_tail(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto newline = text.find('\n', start);
    const auto end = newline == std::string_view::npos ? text.size() : newline;
    lines.push_back(text.substr(start, end - start));
    if (newline == std::string_view::npos) {
      break;
    }
    start = newline + 1;
  }
  if (lines.empty() || (lines.size() == 1U && lines.front().empty())) {
    return {};
  }
  const auto first = lines.size() > 3U ? lines.size() - 3U : 0U;
  std::ostringstream result;
  for (std::size_t index = first; index < lines.size(); ++index) {
    if (index != first) {
      result << '\n';
    }
    result << lines[index];
  }
  return result.str();
}

std::string timeout_text(std::chrono::milliseconds timeout) {
  std::ostringstream result;
  const auto milliseconds = timeout.count();
  if (milliseconds % 1000 == 0) {
    result << milliseconds / 1000;
  } else {
    result << std::fixed << std::setprecision(1)
           << static_cast<double>(milliseconds) / 1000.0;
  }
  return result.str();
}

void kill_group(pid_t child, bool direct_reaped = false) {
  if (::kill(-child, SIGKILL) != 0 && !direct_reaped) {
    static_cast<void>(::kill(child, SIGKILL));
  }
}

enum class ProtocolOutcome : std::uint8_t { pending, accepted, rejected };

struct ProtocolEvaluation {
  ProtocolOutcome outcome{ProtocolOutcome::pending};
  std::string diagnostic;
};

ProtocolEvaluation validate_protocol(std::string_view output, std::string_view error,
                                     bool terminal) {
  std::map<int, std::vector<Json>> replies;
  std::size_t start = 0;
  while (start < output.size()) {
    const auto newline = output.find('\n', start);
    if (newline == std::string_view::npos && !terminal) {
      break;
    }
    const auto end = newline == std::string_view::npos ? output.size() : newline;
    const auto line = output.substr(start, end - start);
    if (!line.empty()) {
      Json message;
      try {
        message = Json::parse(line);
      } catch (const nlohmann::json::exception&) {
        return {ProtocolOutcome::rejected,
                "server wrote non-JSON data to the MCP stdout transport"};
      }
      if (!message.is_object()) {
        return {ProtocolOutcome::rejected,
                "server wrote a non-object JSON value to the MCP stdout transport"};
      }
      const auto identifier = message.find("id");
      if (identifier != message.end() && identifier->is_number_integer()) {
        const auto id = identifier->get<int>();
        if (id == 1 || id == 2) {
          replies[id].push_back(std::move(message));
        }
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    start = newline + 1;
  }
  if (std::ranges::any_of(replies,
                          [](const auto& pair) { return pair.second.size() > 1U; })) {
    return {ProtocolOutcome::rejected,
            "server answered an MCP preflight request more than once"};
  }
  const auto initialized_reply = replies.find(1);
  if (initialized_reply == replies.end() || initialized_reply->second.empty()) {
    if (!terminal) {
      return {};
    }
    const auto tail = stderr_tail(error);
    return {ProtocolOutcome::rejected,
            tail.empty() ? "server exited without answering initialize" : tail};
  }
  const auto result = initialized_reply->second.front().find("result");
  if (result == initialized_reply->second.front().end() || !result->is_object()) {
    const auto tail = stderr_tail(error);
    return {ProtocolOutcome::rejected,
            tail.empty() ? "server exited without answering initialize" : tail};
  }
  if (result->value("protocolVersion", "") != "2025-06-18") {
    return {ProtocolOutcome::rejected,
            "server did not echo the requested MCP protocol version"};
  }
  const auto info = result->find("serverInfo");
  if (info == result->end() || !info->is_object() ||
      (info->value("name", "") != "libtmux" &&
       info->value("name", "") != "libtmux-cxx")) {
    return {ProtocolOutcome::rejected,
            "initialize response did not identify a libtmux MCP server"};
  }
  const auto listed_reply = replies.find(2);
  if (listed_reply == replies.end() || listed_reply->second.empty()) {
    return terminal
               ? ProtocolEvaluation{ProtocolOutcome::rejected,
                                    "server initialized but did not answer tools/list"}
               : ProtocolEvaluation{};
  }
  const auto listed = listed_reply->second.front().find("result");
  if (listed == listed_reply->second.front().end() || !listed->is_object() ||
      !listed->contains("tools") || !(*listed)["tools"].is_array()) {
    return {ProtocolOutcome::rejected,
            "server initialized but did not answer tools/list"};
  }
  std::set<std::string, std::less<>> tools;
  for (const auto& tool : (*listed)["tools"]) {
    if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string()) {
      return {ProtocolOutcome::rejected,
              "server returned a malformed MCP tool catalog"};
    }
    tools.insert(tool["name"].get<std::string>());
  }
  std::vector<std::string> missing;
  for (const auto name :
       {"get_server_info", "list_panes", "list_sessions", "list_windows"}) {
    if (!tools.contains(name)) {
      missing.emplace_back(name);
    }
  }
  if (!missing.empty()) {
    std::ostringstream message;
    message << "server tool catalog is missing: ";
    for (std::size_t index = 0; index < missing.size(); ++index) {
      if (index != 0) {
        message << ", ";
      }
      message << missing[index];
    }
    return {ProtocolOutcome::rejected, message.str()};
  }
  return {ProtocolOutcome::accepted, {}};
}

} // namespace

std::optional<std::string> preflight_spec(const ServerSpec& spec,
                                          std::chrono::milliseconds timeout) {
  if (spec.command.empty()) {
    return "could not launch an empty server command";
  }
  Pipe input;
  Pipe output;
  Pipe error;
  try {
    input = make_pipe();
    output = make_pipe();
    error = make_pipe();
  } catch (...) {
    close_if_open(input.read);
    close_if_open(input.write);
    close_if_open(output.read);
    close_if_open(output.write);
    close_if_open(error.read);
    close_if_open(error.write);
    throw;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    const auto failure = std::string{std::strerror(errno)};
    close_if_open(input.read);
    close_if_open(input.write);
    close_if_open(output.read);
    close_if_open(output.write);
    close_if_open(error.read);
    close_if_open(error.write);
    return "could not launch " + spec.command + ": " + failure;
  }
  if (child == 0) {
    static_cast<void>(::setpgid(0, 0));
    static_cast<void>(::dup2(input.read, STDIN_FILENO));
    static_cast<void>(::dup2(output.write, STDOUT_FILENO));
    static_cast<void>(::dup2(error.write, STDERR_FILENO));
    close_if_open(input.read);
    close_if_open(input.write);
    close_if_open(output.read);
    close_if_open(output.write);
    close_if_open(error.read);
    close_if_open(error.write);
    for (const auto& [name, value] : spec.environment) {
      if (::setenv(name.c_str(), value.c_str(), 1) != 0) {
        ::_exit(126);
      }
    }
    std::vector<std::string> owned;
    owned.reserve(spec.arguments.size() + 1U);
    owned.push_back(spec.command);
    owned.insert(owned.end(), spec.arguments.begin(), spec.arguments.end());
    std::vector<char*> arguments;
    arguments.reserve(owned.size() + 1U);
    for (auto& value : owned) {
      arguments.push_back(value.data());
    }
    arguments.push_back(nullptr);
    ::execvp(arguments.front(), arguments.data());
    const auto message = std::string{"could not launch "} + spec.command + ": " +
                         std::strerror(errno) + "\n";
    static_cast<void>(::write(STDERR_FILENO, message.data(), message.size()));
    ::_exit(127);
  }

  static_cast<void>(::setpgid(child, child));
  close_if_open(input.read);
  close_if_open(output.write);
  close_if_open(error.write);

  struct sigaction ignored {};
  struct sigaction previous {};
  ignored.sa_handler = SIG_IGN;
  // Not `::sigemptyset`: macOS defines it as a function-like macro, which a
  // qualified name cannot expand.
  static_cast<void>(sigemptyset(&ignored.sa_mask));
  static_cast<void>(::sigaction(SIGPIPE, &ignored, &previous));
  try {
    write_request(input.write, request_frames());
  } catch (...) {
    static_cast<void>(::sigaction(SIGPIPE, &previous, nullptr));
    close_if_open(input.write);
    kill_group(child);
    static_cast<void>(::waitpid(child, nullptr, 0));
    close_if_open(output.read);
    close_if_open(error.read);
    throw;
  }
  static_cast<void>(::sigaction(SIGPIPE, &previous, nullptr));
  close_if_open(input.write);

  try {
    set_nonblocking(output.read);
    set_nonblocking(error.read);
  } catch (...) {
    kill_group(child);
    static_cast<void>(::waitpid(child, nullptr, 0));
    close_if_open(output.read);
    close_if_open(error.read);
    throw;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string stdout_bytes;
  std::string stderr_bytes;
  bool stdout_open = true;
  bool stderr_open = true;
  bool overflow = false;
  bool exited = false;
  std::optional<std::chrono::steady_clock::time_point> accepted_at;
  int status = 0;
  const auto stop = [&] {
    kill_group(child, exited);
    while (!exited) {
      const auto waited = ::waitpid(child, &status, 0);
      if (waited == child || (waited < 0 && errno == ECHILD)) {
        exited = true;
      } else if (waited < 0 && errno != EINTR) {
        break;
      }
    }
    close_if_open(output.read);
    close_if_open(error.read);
  };
  try {
    for (;;) {
      if (!exited) {
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
          exited = true;
        } else if (waited < 0 && errno != EINTR) {
          stop();
          return "could not wait for the MCP preflight server";
        }
      }
      if (stdout_open) {
        stdout_open = drain(output.read, stdout_bytes, overflow);
      }
      if (stderr_open) {
        stderr_open = drain(error.read, stderr_bytes, overflow);
      }
      if (overflow) {
        stop();
        return "server output exceeded the MCP preflight limit";
      }
      if (exited && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        const auto tail = stderr_tail(stderr_bytes);
        stop();
        if (!tail.empty()) {
          return tail;
        }
        if (WIFEXITED(status)) {
          return "server exited with status " + std::to_string(WEXITSTATUS(status));
        }
        return "server terminated during MCP preflight";
      }

      const auto evaluation = validate_protocol(stdout_bytes, stderr_bytes, exited);
      if (evaluation.outcome == ProtocolOutcome::rejected) {
        stop();
        return evaluation.diagnostic;
      }
      const auto now = std::chrono::steady_clock::now();
      if (evaluation.outcome == ProtocolOutcome::accepted) {
        if (!accepted_at.has_value()) {
          accepted_at = now;
        }
        if (exited || now - *accepted_at >= response_stability || now >= deadline) {
          stop();
          return std::nullopt;
        }
      } else if (exited) {
        stop();
        return "server exited without completing the MCP preflight";
      }

      if (now >= deadline) {
        stop();
        return "no MCP response within " + timeout_text(timeout) + "s";
      }
      std::array<pollfd, 2> descriptors{pollfd{stdout_open ? output.read : -1,
                                               static_cast<short>(POLLIN | POLLHUP), 0},
                                        pollfd{stderr_open ? error.read : -1,
                                               static_cast<short>(POLLIN | POLLHUP),
                                               0}};
      const auto wake = accepted_at.has_value()
                            ? std::min(deadline, *accepted_at + response_stability)
                            : deadline;
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(wake - now);
      const auto poll_timeout =
          static_cast<int>(std::min<std::int64_t>(50, remaining.count()));
      static_cast<void>(::poll(descriptors.data(), descriptors.size(), poll_timeout));
    }
  } catch (...) {
    stop();
    throw;
  }
}

} // namespace libtmux::mcp_swap
