#include "libtmux/testing/tmux_version.hpp"

#include "process.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace libtmux::test {
namespace {

struct Resolved {
  Version version;
  std::string description;
};

std::string trim(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

Resolved ask(const std::filesystem::path& tmux_binary) {
  // Two things this call gets right that are easy to get wrong. `arguments` is
  // argv *after* argv[0] — `ChildProcess` prepends the executable, and passing
  // it again runs `tmux tmux -V`. And the environment is the caller's, not a
  // stripped one: `tmux_binary` defaults to a bare name, so `PATH` picks which
  // tmux answers, and it has to be the same `PATH` that will pick when the
  // fixture spawns the server.
  auto child =
      detail::ChildProcess::spawn({.executable = tmux_binary,
                                   .arguments = {"-V"},
                                   .environment = detail::current_environment()});
  if (!child.has_value()) {
    // Unreadable: report the newest possible, so nothing is skipped.
    return {Version{.unbounded = true}, "unknown"};
  }
  const auto deadline = detail::ProcessClock::now() + std::chrono::milliseconds{5000};
  child->drain_until(deadline);
  static_cast<void>(child->wait_until(deadline));
  child->drain_until(deadline);

  auto description = trim(child->stdout_text());
  if (description.empty()) {
    return {Version{.unbounded = true}, "unknown"};
  }
  const auto parsed = parse_version(description);
  if (!parsed.has_value()) {
    return {Version{.unbounded = true}, std::move(description)};
  }
  return {*parsed, std::move(description)};
}

// Keyed by the binary asked about, so a suite that drives two tmux builds in
// one process gets the right answer for each.
const Resolved& resolve(const std::filesystem::path& tmux_binary) {
  static std::mutex guard;
  static std::map<std::string, Resolved> cache;
  const std::lock_guard<std::mutex> held{guard};
  const auto key = tmux_binary.string();
  const auto found = cache.find(key);
  if (found != cache.end()) {
    return found->second;
  }
  return cache.emplace(key, ask(tmux_binary)).first->second;
}

} // namespace

const Version& running_tmux(const std::filesystem::path& tmux_binary) {
  return resolve(tmux_binary).version;
}

std::string describe_running_tmux(const std::filesystem::path& tmux_binary) {
  return resolve(tmux_binary).description;
}

} // namespace libtmux::test
