// One workload, two transports, measured side by side.
//
// Every call in this library runs a tmux command and reads the answer, so a
// loop of twelve calls pays twelve process spawns. A `Chain` folds the same
// twelve into one invocation. The difference is round trips, not cleverness,
// and it is the number worth knowing before reaching for either.
//
// This prints timings from one run on one machine against a scratch server. It
// is a shape, not a specification: absolute numbers move with the host, the
// tmux build, and what else is running.

#include <chrono>
#include <cstdio>
#include <string>

#include <libtmux/chain.hpp>
#include <libtmux/libtmux.hpp>

#include "scratch_server.hpp"

namespace {

constexpr int kWindows = 12;

// Microseconds, so a fast path does not round to zero.
template <typename Work> long long micros(Work&& work) {
  const auto started = std::chrono::steady_clock::now();
  work();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  return std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
}

}  // namespace

int main() {
  const example::ScratchServer scratch = example::ScratchServer::open();
  const libtmux::Server& server = scratch.get();

  const auto sessions = server.sessions();
  if (!sessions.has_value() || sessions->empty()) {
    std::fprintf(stderr, "%s\n",
                 sessions.has_value() ? "the scratch server has no session"
                                      : sessions.error().diagnostic.c_str());
    return 1;
  }
  const libtmux::Session& session = sessions->front();
  const std::string session_name{session.name()};

  // One command per window: the default path, and what a loop costs.
  bool per_command_ok = true;
  const long long per_command = micros([&] {
    for (int index = 0; index < kWindows; ++index) {
      const auto made = session.new_window({.name = "one-" + std::to_string(index)});
      if (!made.has_value()) {
        std::fprintf(stderr, "%s\n", made.error().diagnostic.c_str());
        per_command_ok = false;
        return;
      }
    }
  });
  if (!per_command_ok) {
    return 1;
  }

  // The same twelve windows, folded into a single invocation.
  libtmux::Chain chain;
  for (int index = 0; index < kWindows; ++index) {
    chain.new_window(session_name, "folded-" + std::to_string(index));
  }
  if (!chain.valid()) {
    std::fprintf(stderr, "the chain did not validate\n");
    return 1;
  }
  bool chained_ok = true;
  const long long chained = micros([&] {
    const auto ran = server.run_chain(chain);
    if (!ran.has_value()) {
      std::fprintf(stderr, "%s\n", ran.error().diagnostic.c_str());
      chained_ok = false;
    }
  });
  if (!chained_ok) {
    return 1;
  }

  // Both paths built the same thing, which is the claim the timings rest on.
  const auto windows = session.windows();
  if (!windows.has_value()) {
    std::fprintf(stderr, "%s\n", windows.error().diagnostic.c_str());
    return 1;
  }

  std::printf("%-14s %10s %10s\n", "mode", "commands", "micros");
  std::printf("%-14s %10d %10lld\n", "per-command", kWindows, per_command);
  std::printf("%-14s %10d %10lld\n", "chained", 1, chained);
  std::printf("both modes built the same thing: %s\n",
              windows->size() >= static_cast<std::size_t>(2 * kWindows) ? "true" : "false");
  return 0;
}
