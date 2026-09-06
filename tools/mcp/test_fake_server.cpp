#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool write_all(int descriptor, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto written = ::write(descriptor, bytes.data(), bytes.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

bool write_marker(std::string_view variable, std::string_view contents) {
  const char* path = std::getenv(std::string{variable}.c_str());
  if (path == nullptr || *path == '\0') {
    return false;
  }
  const int descriptor = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  const bool written = write_all(descriptor, contents) && ::fsync(descriptor) == 0;
  return ::close(descriptor) == 0 && written;
}

bool spawn_heartbeat() {
  const char* path = std::getenv("MCP_SWAP_FAKE_HEARTBEAT");
  if (path == nullptr || *path == '\0') {
    return false;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    const int descriptor =
        ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0) {
      ::_exit(11);
    }
    for (;;) {
      if (!write_all(descriptor, "x") || ::fsync(descriptor) != 0) {
        ::_exit(12);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
  }
  for (int attempt = 0; attempt < 200; ++attempt) {
    struct stat details {};
    if (::stat(path, &details) == 0 && details.st_size > 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return 8;
  }
  const std::string mode{argv[1]};
  if (mode == "oversize") {
    for (std::size_t written = 0; written < 1024U * 1024U + 8192U; written += 8192U) {
      std::cout << std::string(8192U, 'x');
    }
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::seconds{5});
    return 0;
  }
  if (mode == "oversize-tree-stdout" || mode == "oversize-tree-stderr") {
    if (!spawn_heartbeat()) {
      return 10;
    }
    auto& stream = mode == "oversize-tree-stdout" ? std::cout : std::cerr;
    for (std::size_t written = 0; written < 1024U * 1024U + 8192U; written += 8192U) {
      stream << std::string(8192U, 'x');
    }
    stream.flush();
    std::this_thread::sleep_for(std::chrono::seconds{5});
    return 0;
  }
  if (mode == "stderr-failure") {
    std::cerr << "synthetic preflight stderr\n";
    return 6;
  }
  if (mode == "timeout-tree") {
    if (!spawn_heartbeat()) {
      return 10;
    }
    std::this_thread::sleep_for(std::chrono::seconds{5});
    return 0;
  }
  if (mode == "timeout") {
    std::this_thread::sleep_for(std::chrono::seconds{5});
    return 0;
  }
  if (mode == "require-env") {
    const char* value = std::getenv("MCP_SWAP_FAKE_ENV");
    if (value == nullptr || std::string_view{value} != "present") {
      std::cerr << "environment missing\n";
      return 6;
    }
  }
  for (const auto expected :
       {"initialize", "notifications/initialized", "tools/list"}) {
    std::string line;
    if (!std::getline(std::cin, line)) {
      return 9;
    }
    const auto frame = nlohmann::json::parse(line);
    if (frame.value("method", "") != expected) {
      return 9;
    }
  }
  if (mode == "stay-alive-tree") {
    if (!write_marker("MCP_SWAP_FAKE_PID", std::to_string(::getpid())) ||
        !spawn_heartbeat()) {
      return 10;
    }
  }
  const auto version = mode == "wrong-version" ? "1900-01-01" : "2025-06-18";
  const auto name = mode == "wrong-server" ? "another-server" : "libtmux-cxx";
  std::cout << nlohmann::json{{"jsonrpc", "2.0"},
                              {"id", 1},
                              {"result",
                               {{"protocolVersion", version},
                                {"capabilities", {{"tools", nlohmann::json::object()}}},
                                {"serverInfo", {{"name", name}, {"version", "test"}}}}}}
                   .dump()
            << '\n';
  nlohmann::json tools = nlohmann::json::array();
  if (mode != "empty-tools") {
    for (const auto tool :
         {"get_server_info", "list_panes", "list_sessions", "list_windows"}) {
      tools.push_back({{"name", tool}});
    }
  }
  if (mode != "missing-tools") {
    const nlohmann::json reply{
        {"jsonrpc", "2.0"}, {"id", 2}, {"result", {{"tools", tools}}}};
    std::cout << reply.dump() << '\n';
    if (mode == "duplicate-reply") {
      std::cout << reply.dump() << '\n';
    }
  }
  if (mode == "garbage-stdout") {
    std::cout << "not a protocol frame\n";
  }
  std::cout.flush();
  if (mode == "crash-after-reply") {
    std::cerr << "synthetic crash\n";
    return 7;
  }
  if (mode == "stay-alive-tree") {
    std::this_thread::sleep_for(std::chrono::seconds{5});
  }
  return 0;
}
