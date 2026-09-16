// What listing a growing snapshot, decoding it, and draining a control
// connection's notifications each cost, as one table.
//
//   $ cmake --build build/matrix --target libtmux_operations_bench
//   $ ./build/matrix/benchmarks/libtmux_operations_bench
//
// Complements apps/matrix, which measures command-dispatch cost (one process
// per command against a batched Chain). Dispatch is one call; these are the
// three costs that ride on top of it once a caller actually reads something
// back.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <libtmux/control.hpp>
#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>

namespace {

using Clock = std::chrono::steady_clock;

// Sessions x windows/session x panes/window. Large enough that a listing
// cost is visible against process overhead, small enough that setup itself
// stays a small fraction of the run.
constexpr int kSessions = 3;
constexpr int kWindowsPerSession = 2;
constexpr int kPanesPerWindow = 2;
constexpr int kBurstLines = 2000;

[[nodiscard]] std::string_view text(std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] double milliseconds(Clock::duration elapsed) {
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

void report(std::string_view label, double ms, std::string_view detail) {
  std::printf("%-28s %9.2fms  %s\n", std::string{label}.c_str(), ms,
              std::string{detail}.c_str());
}

// Three sessions, two windows each, two panes each window: enough rows that
// each deeper listing visibly costs more than the one above it.
[[nodiscard]] bool build_topology(const libtmux::Server& server) {
  for (int session = 1; session < kSessions; ++session) {
    auto created = server.new_session("bench-" + std::to_string(session));
    if (!created.has_value()) {
      std::fprintf(stderr, "benchmarks: %s\n", created.error().diagnostic.c_str());
      return false;
    }
  }
  const auto sessions = server.sessions();
  if (!sessions.has_value() ||
      sessions->size() != static_cast<std::size_t>(kSessions)) {
    std::fprintf(stderr, "benchmarks: expected %d sessions, got %s\n", kSessions,
                 sessions.has_value() ? std::to_string(sessions->size()).c_str()
                                      : sessions.error().diagnostic.c_str());
    return false;
  }
  for (const libtmux::Session& session : *sessions) {
    for (int window = 1; window < kWindowsPerSession; ++window) {
      const auto added = session.new_window({.name = "w" + std::to_string(window)});
      if (!added.has_value()) {
        std::fprintf(stderr, "benchmarks: %s\n", added.error().diagnostic.c_str());
        return false;
      }
    }
    const auto windows = session.windows();
    if (!windows.has_value()) {
      std::fprintf(stderr, "benchmarks: %s\n", windows.error().diagnostic.c_str());
      return false;
    }
    for (const libtmux::Window& window : *windows) {
      for (int pane = 1; pane < kPanesPerWindow; ++pane) {
        if (!window.split().has_value()) {
          std::fprintf(stderr, "benchmarks: could not split a pane\n");
          return false;
        }
      }
    }
  }
  return true;
}

// Listing costs more the deeper the entity: a session lists cheaply, a pane
// listing reads every row underneath it.
bool bench_listing_depth(const libtmux::Server& server) {
  std::puts("\nlisting depth");
  std::puts("path                          wall  detail");
  std::puts("----------------------------------------------------------------"
            "------------");

  const auto sessions_start = Clock::now();
  const auto sessions = server.sessions();
  const auto sessions_ms = milliseconds(Clock::now() - sessions_start);
  if (!sessions.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", sessions.error().diagnostic.c_str());
    return false;
  }
  report("sessions()", sessions_ms, std::to_string(sessions->size()) + " row(s)");

  const auto windows_start = Clock::now();
  const auto windows = server.windows();
  const auto windows_ms = milliseconds(Clock::now() - windows_start);
  if (!windows.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", windows.error().diagnostic.c_str());
    return false;
  }
  report("windows()", windows_ms, std::to_string(windows->size()) + " row(s)");

  const auto panes_start = Clock::now();
  const auto panes = server.panes();
  const auto panes_ms = milliseconds(Clock::now() - panes_start);
  if (!panes.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", panes.error().diagnostic.c_str());
    return false;
  }
  report("panes()", panes_ms, std::to_string(panes->size()) + " row(s)");

  // Same call site's raw round trip, tmux's own default format rather than
  // the fields the typed decoder reads: not the same query, so this is an
  // upper bound on decode cost, not an isolation of it.
  const auto raw_start = Clock::now();
  const auto raw = server.run({"list-panes", "-a"});
  const auto raw_ms = milliseconds(Clock::now() - raw_start);
  if (!raw.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", raw.error().diagnostic.c_str());
    return false;
  }
  report("panes() [raw, undecoded]", raw_ms, std::to_string(raw->size()) + " byte(s)");
  return true;
}

// A control connection listening for one pane's output, draining a burst it
// did not ask tmux to slow down for.
bool bench_control_throughput(const libtmux::test::ScopedTmuxServer& fixture,
                              const libtmux::Server& server) {
  std::puts("\ncontrol-mode notification throughput");

  // The client this connection opens attaches to one session, and tmux scopes
  // pane-output delivery to what that client can see: a pane picked from
  // `server.panes()` server-wide could belong to a different session and
  // never be delivered at all.
  const auto owner = server.session(fixture.session_name());
  if (!owner.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", owner.error().diagnostic.c_str());
    return false;
  }
  const auto panes = owner->panes();
  if (!panes.has_value() || panes->empty()) {
    std::fprintf(stderr, "benchmarks: no pane to stream from\n");
    return false;
  }
  const std::string pane_id{panes->front().id()};

  auto connected =
      libtmux::Connection::connect({.socket_path = fixture.socket_path(),
                                    .session_name = std::string{fixture.session_name()},
                                    .pane_output = true});
  if (!connected.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", connected.error().message.c_str());
    return false;
  }
  auto connection = std::move(*connected);
  if (!connection.resume_pane_output(pane_id, Clock::now() + std::chrono::seconds{30})
           .has_value()) {
    std::fprintf(stderr, "benchmarks: could not resume pane output\n");
    return false;
  }
  static_cast<void>(connection.take_notifications());

  const auto burst =
      connection.execute({.group = {{{"send-keys", "-t", pane_id,
                                      "seq 1 " + std::to_string(kBurstLines) +
                                          // The single-quote split keeps this
                                          // marker out of the shell's own echo
                                          // of the typed line, which would
                                          // otherwise match before the seq
                                          // output - let alone the marker -
                                          // ever arrives.
                                          "; echo bench-burst-do''ne",
                                      "Enter"}}}},
                         Clock::now() + std::chrono::seconds{30});
  if (burst.connection_error.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", burst.connection_error->message.c_str());
    return false;
  }

  std::size_t notifications = 0;
  std::size_t bytes = 0;
  const auto drain_start = Clock::now();
  // Two bounds, because they answer different questions. `kDrainIdle` is the
  // real one: a burst that is still arriving keeps resetting it, so silence
  // for this long means the output path stopped, which is the only failure
  // worth reporting. `kDrainCap` exists so a pathologically slow machine
  // cannot wedge the lane forever. Neither is a throughput budget -- the
  // measurement is the elapsed time recorded below, and a shared CI runner
  // drains this burst orders of magnitude slower than a quiet one, so any cap
  // tight enough to be interesting would turn "the box was busy" into "the
  // output path is broken".
  constexpr auto kDrainIdle = std::chrono::seconds{10};
  constexpr auto kDrainCap = std::chrono::seconds{120};
  const auto cap = drain_start + kDrainCap;
  bool done = false;
  while (!done && Clock::now() < cap) {
    const auto batch =
        connection.wait_for_notifications(std::min(cap, Clock::now() + kDrainIdle));
    if (batch.empty()) {
      break;
    }
    for (const auto& notification : batch) {
      ++notifications;
      bytes += notification.body.size();
      if (libtmux::parse(notification).kind == libtmux::NotificationKind::output &&
          text(notification.body).find("bench-burst-done") != std::string_view::npos) {
        done = true;
      }
    }
  }
  const auto elapsed_ms = milliseconds(Clock::now() - drain_start);
  const auto seconds = elapsed_ms / 1000.0;
  const auto bytes_per_second =
      seconds > 0.0 ? static_cast<double>(bytes) / seconds : 0.0;
  report("drain " + std::to_string(kBurstLines) + " line burst", elapsed_ms,
         std::to_string(notifications) + " notification(s), " + std::to_string(bytes) +
             " byte(s), " +
             std::to_string(static_cast<long long>(bytes_per_second / 1024)) +
             " KiB/s");
  if (!done) {
    std::fprintf(
        stderr,
        "benchmarks: the burst marker never arrived; throughput not measured\n");
    return false;
  }
  static_cast<void>(connection.shutdown(Clock::now() + std::chrono::seconds{2}));
  return true;
}

} // namespace

int main() {
  auto fixture = libtmux::test::ScopedTmuxServer::start({.session_name = "bench-ops"});
  if (!fixture.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", fixture.error().c_str());
    return 1;
  }
  auto opened = libtmux::Server::at_socket_path(fixture->socket_path().string());
  if (!opened.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", opened.error().diagnostic.c_str());
    return 1;
  }
  const libtmux::Server& server = *opened;

  if (!build_topology(server)) {
    return 1;
  }

  bool ok = bench_listing_depth(server);
  ok = bench_control_throughput(*fixture, server) && ok;
  return ok ? 0 : 1;
}
