#include "environment.hpp"
#include "libtmux/lifecycle.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <sstream>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

LIBTMUX_NAMESPACE_BEGIN
expected<DiscoveryResult, CommandFailure> discover_servers(DiscoveryOptions options) {
#if defined(_WIN32)
  static_cast<void>(options);
  return unexpected(
      CommandFailure{.kind = FailureKind::unsupported,
                     .diagnostic = "socket discovery requires POSIX tmux"});
#else
  if (options.max_roots == 0 || options.max_entries == 0 || options.max_probes == 0 ||
      options.timeout <= std::chrono::milliseconds::zero() ||
      options.probe_timeout <= std::chrono::milliseconds::zero()) {
    return unexpected(
        CommandFailure{.kind = FailureKind::validation,
                       .diagnostic = "discovery limits and timeouts must be positive"});
  }
  if (!options.policy.child_environment)
    options.policy.child_environment = libtmux_env::snapshot();
  if (options.configured_roots) {
    if (auto configured =
            libtmux_env::value(*options.policy.child_environment, "TMUX_TMPDIR")) {
      options.roots.emplace_back(std::filesystem::path{*configured} /
                                 ("tmux-" + std::to_string(::getuid())));
    }
    options.roots.emplace_back(std::filesystem::path{"/tmp"} /
                               ("tmux-" + std::to_string(::getuid())));
  }
  DiscoveryResult result;
  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  std::set<std::string> roots;
  std::set<std::pair<std::uintmax_t, std::uintmax_t>> sockets;
  const auto limit = [&](const std::filesystem::path& path, std::string reason) {
    result.truncated = true;
    result.diagnostics.push_back({path, std::move(reason), {}});
  };
  const auto stopped = [&](const std::filesystem::path& path) {
    if ((options.cancelled && options.cancelled())) {
      limit(path, "cancelled");
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      limit(path, "time limit");
      return true;
    }
    return false;
  };
  for (const auto& root : options.roots) {
    if (stopped(root))
      break;
    if (result.roots_examined == options.max_roots) {
      limit(root, "root limit");
      break;
    }
    ++result.roots_examined;
    std::error_code error;
    if (!root.is_absolute()) {
      result.diagnostics.push_back({root, "root must be absolute", {}});
      continue;
    }
    const auto resolved = std::filesystem::canonical(root, error);
    if (error) {
      result.diagnostics.push_back({root, "root: " + error.message(), {}});
      continue;
    }
    if (!roots.insert(resolved.string()).second) {
      result.diagnostics.push_back({root, "duplicate root", {}});
      continue;
    }
    auto entries = std::filesystem::directory_iterator(resolved, error);
    if (error) {
      result.diagnostics.push_back({root, "root: " + error.message(), {}});
      continue;
    }
    result.diagnostics.push_back({root, "root opened", {}});
    for (; entries != std::filesystem::directory_iterator{}; entries.increment(error)) {
      if (error) {
        result.diagnostics.push_back(
            {root, "directory enumeration: " + error.message(), {}});
        break;
      }
      const auto path = entries->path();
      if (stopped(path))
        break;
      if (result.entries_examined == options.max_entries) {
        limit(path, "entry limit");
        break;
      }
      ++result.entries_examined;
      struct stat status {};
      if (::stat(path.c_str(), &status) != 0) {
        result.diagnostics.push_back(
            {path,
             "stat: " + std::error_code{errno, std::generic_category()}.message(),
             {}});
        continue;
      }
      if (!S_ISSOCK(status.st_mode)) {
        result.diagnostics.push_back({path, "not a socket", {}});
        continue;
      }
      const auto key = std::pair{static_cast<std::uintmax_t>(status.st_dev),
                                 static_cast<std::uintmax_t>(status.st_ino)};
      if (!sockets.insert(key).second) {
        result.diagnostics.push_back({path, "duplicate socket", {}});
        continue;
      }
      if (result.probes == options.max_probes) {
        limit(path, "probe limit");
        break;
      }
      ++result.probes;
      auto server = Server::at_socket_path(path, {}, options.policy);
      if (!server) {
        result.diagnostics.push_back({path, "open failed", server.error()});
        continue;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) {
        limit(path, "time limit");
        break;
      }
      const auto reply = server->run(
          {"display-message", "-p", "__libtmux_discovery__ #{pid} #{start_time}"},
          std::min(options.probe_timeout, remaining), 4096);
      if (!reply) {
        result.diagnostics.push_back({path, "probe failed", reply.error()});
        continue;
      }
      std::istringstream fields{*reply};
      std::string marker, pid, start, extra;
      fields >> marker >> pid >> start;
      const auto decimal = [](const std::string& value, bool positive) {
        unsigned long long n{};
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), n);
        return !value.empty() && parsed.ec == std::errc{} &&
               parsed.ptr == value.data() + value.size() && (!positive || n != 0);
      };
      if (marker != "__libtmux_discovery__" || !decimal(pid, true) ||
          !decimal(start, false) || (fields >> extra)) {
        result.diagnostics.push_back({path, "invalid probe response", {}});
        continue;
      }
      result.servers.push_back({path, pid, start});
      result.diagnostics.push_back({path, "server", {}});
    }
    if (error)
      result.diagnostics.push_back(
          {root, "directory enumeration: " + error.message(), {}});
    if (result.truncated)
      break;
  }
  return result;
#endif
}
LIBTMUX_NAMESPACE_END
