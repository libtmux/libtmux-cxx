// What each way of reaching tmux costs, printed as one table.
//
//   $ cmake --build build/matrix --target libtmux_matrix
//   $ ./build/matrix/apps/matrix/libtmux_matrix
//   $ ./build/matrix/apps/matrix/libtmux_matrix --json
//
// Every lane builds the same six-pane window and then answers the same
// question about the resulting server. Equal timings mean nothing unless the
// answers agree, so the answer travels beside the measurement.

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <libtmux/libtmux.hpp>
#include <libtmux/testing/scoped_server.hpp>

namespace {

// Balances matrix runtime against visible per-command costs.
constexpr int kPanesPerWindow = 6;
constexpr long long kNanosecondsPerMillisecond = 1'000'000;

struct Row {
  std::string mode;
  std::string state;
  std::optional<long long> elapsed_ns;
  std::optional<int> processes;
  std::optional<int> clients;
  std::optional<std::string> answer;
  std::optional<std::string> reason;
};

std::string capture(const std::string& command) {
  std::string output;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return output;
  }
  char buffer[512];
  while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
    output += buffer;
  }
  ::pclose(pipe);
  while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) {
    output.pop_back();
  }
  return output;
}

// A shell proxy that records one invocation and execs the real tmux, named
// `tmux` so it can be handed directly as `tmux_binary`.
std::filesystem::path write_proxy(const std::filesystem::path& directory,
                                  const std::string& real_tmux,
                                  const std::filesystem::path& records) {
  const std::filesystem::path proxy = directory / "tmux";
  FILE* file = std::fopen(proxy.c_str(), "w");
  if (file == nullptr) {
    return {};
  }
  std::fprintf(file, "#!/bin/sh\nprintf '1\\n' >> '%s' || exit 125\nexec '%s' \"$@\"\n",
               records.c_str(), real_tmux.c_str());
  std::fclose(file);
  std::filesystem::permissions(proxy, std::filesystem::perms::owner_all);
  return proxy;
}

int count_records(const std::filesystem::path& records) {
  FILE* file = std::fopen(records.c_str(), "r");
  if (file == nullptr) {
    return 0;
  }
  int lines = 0;
  int character = 0;
  while ((character = std::fgetc(file)) != EOF) {
    if (character == '\n') {
      ++lines;
    }
  }
  std::fclose(file);
  return lines;
}

void reset_records(const std::filesystem::path& records) {
  FILE* file = std::fopen(records.c_str(), "w");
  if (file != nullptr) {
    std::fclose(file);
  }
}

// Describes the server so lanes can be compared for equivalence.
std::string search_answer(const libtmux::Server& server) {
  const auto panes = server.panes();
  if (!panes.has_value()) {
    return "unreadable";
  }
  std::string indexes;
  for (const libtmux::Pane& pane : *panes) {
    if (!indexes.empty()) {
      indexes += ' ';
    }
    indexes += std::to_string(pane.index());
  }
  return std::to_string(panes->size()) + " panes on the server [" + indexes + "]";
}

std::vector<std::vector<std::string>> workload(const std::string& target) {
  std::vector<std::vector<std::string>> commands;
  for (int index = 0; index < kPanesPerWindow - 1; ++index) {
    commands.push_back({"split-window", "-t", target});
    // Reapply the layout so repeated splits do not exhaust one pane's width.
    commands.push_back({"select-layout", "-t", target, "tiled"});
  }
  commands.push_back({"rename-window", "-t", target, "built"});
  return commands;
}

// `connections` of zero launches tmux per command; otherwise the build runs
// over that many held-open control clients, as the Go port's pool does.
std::optional<Row> measure(const std::string& mode, const std::string& real_tmux,
                           bool chained, std::size_t connections) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "libtmux-cxx-bench";
  std::filesystem::create_directories(root);
  const std::filesystem::path directory =
      root /
      ("run-" + std::to_string(getpid()) + "-" + mode.substr(0, 4) +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const std::filesystem::path records = directory / "tmux-invocations";
  reset_records(records);
  const std::filesystem::path proxy = write_proxy(directory, real_tmux, records);
  if (proxy.empty()) {
    return std::nullopt;
  }

  // The fixture's server and the policy's measured commands both run through
  // the proxy, so nothing here can bypass the invocation counter.
  auto fixture = libtmux::test::ScopedTmuxServer::start({
      .tmux_binary = proxy,
      .session_name = "bench",
  });
  if (!fixture.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", fixture.error().c_str());
    return std::nullopt;
  }
  auto opened = libtmux::Server::at_socket_path(fixture->socket_path().string(), {},
                                                {.tmux_binary = proxy});
  if (!opened.has_value()) {
    std::fprintf(stderr, "benchmarks: %s\n", opened.error().diagnostic.c_str());
    return std::nullopt;
  }
  // Opening the clients is setup, as starting the server is: only the build
  // below is measured, and it must launch nothing.
  if (connections > 0U) {
    auto controlled = opened->over_control("bench", connections);
    if (!controlled.has_value()) {
      std::fprintf(stderr, "benchmarks: %s\n", controlled.error().message.c_str());
      return std::nullopt;
    }
    opened = *std::move(controlled);
  }
  const libtmux::Server& server = *opened;

  const auto sessions = server.sessions();
  if (!sessions.has_value() || sessions->empty()) {
    return std::nullopt;
  }
  const auto window = sessions->at(0).new_window({.name = "work"});
  if (!window.has_value()) {
    return std::nullopt;
  }
  const std::string target{window->id()};

  reset_records(records);
  const auto started = std::chrono::steady_clock::now();
  if (chained) {
    libtmux::Chain chain;
    for (const auto& command : workload(target)) {
      chain.command(command);
    }
    const auto outcome = server.run_chain(chain);
    if (!outcome.has_value()) {
      std::fprintf(stderr, "benchmarks: %s\n", outcome.error().diagnostic.c_str());
      return std::nullopt;
    }
  } else {
    for (const auto& command : workload(target)) {
      const auto outcome = server.run(libtmux::CommandRequest{command});
      if (!outcome.has_value()) {
        std::fprintf(stderr, "benchmarks: %s\n", outcome.error().diagnostic.c_str());
        return std::nullopt;
      }
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();

  const int processes = count_records(records);
  const std::string answer = search_answer(server);
  const auto clients = server.clients();
  // Leaving one directory behind per lane fills the temporary directory with
  // proxies and record files nothing will read again.
  std::error_code removal;
  std::filesystem::remove_all(directory, removal);
  Row row{mode,
          "supported",
          static_cast<long long>(elapsed),
          processes,
          clients.has_value() ? static_cast<int>(clients->size()) : 0,
          answer,
          std::nullopt};
  return row;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> arguments(argv + 1, argv + argc);
  const bool want_json =
      std::find(arguments.begin(), arguments.end(), "--json") != arguments.end();
  const bool want_check =
      std::find(arguments.begin(), arguments.end(), "--check") != arguments.end();

  const std::string real_tmux = capture("command -v tmux");
  if (real_tmux.empty()) {
    if (want_json) {
      std::printf("{\"error\":\"resolve real tmux executable: not found on PATH\","
                  "\"outcome\":\"failed\",\"port\":\"cxx\",\"schema\":1}\n");
    } else {
      std::fprintf(stderr, "benchmarks: tmux is not on PATH\n");
    }
    return 1;
  }

  struct Lane {
    std::string mode;
    bool chained;
    std::size_t connections;
  };
  std::vector<Row> rows;
  for (const Lane& lane : std::vector<Lane>{{"process", false, 0U},
                                            {"chained", true, 0U},
                                            {"connection", false, 1U},
                                            {"concurrent x4", false, 4U},
                                            {"chained + connection", true, 1U}}) {
    const std::string& mode = lane.mode;
    auto row = measure(mode, real_tmux, lane.chained, lane.connections);
    if (!row.has_value()) {
      if (want_json) {
        std::printf("{\"error\":\"lane %s did not complete\",\"outcome\":\"failed\","
                    "\"port\":\"cxx\",\"schema\":1}\n",
                    mode.c_str());
      } else {
        std::fprintf(stderr, "benchmarks: lane %s did not complete\n", mode.c_str());
      }
      return 1;
    }
    rows.push_back(*row);
  }

  if (want_check) {
    std::vector<std::string> failures;
    const std::string expected = rows.front().answer.value_or("");
    for (const Row& row : rows) {
      if (row.state != "supported") {
        if (!row.reason.has_value() || row.reason->empty()) {
          failures.push_back(row.mode + " declares no reason");
        }
        continue;
      }
      if (row.answer.value_or("") != expected) {
        failures.push_back(row.mode + " answered " + row.answer.value_or("nothing"));
      }
      if (row.elapsed_ns.value_or(0) <= 0) {
        failures.push_back(row.mode + " reports no duration");
      }
    }
    if (expected.rfind("7 panes on the server [", 0) != 0) {
      failures.push_back("the answer is not the expected topology: " + expected);
    }
    // Exact counts, not an inequality. Invocations are deterministic and the
    // same on every machine, which is what makes them gateable where wall
    // clock is not: a stray launch added to the typed path keeps `chained <
    // process` true and would have gone unnoticed. The expectation is derived
    // from the workload rather than written down, so changing the workload
    // cannot leave a stale number behind.
    const int expected_process_launches = static_cast<int>(workload("@0").size());
    if (rows[0].processes.value_or(0) != expected_process_launches) {
      failures.push_back("the process lane made " +
                         std::to_string(rows[0].processes.value_or(0)) +
                         " tmux invocations, not one per command (" +
                         std::to_string(expected_process_launches) + ")");
    }
    if (rows[1].processes.value_or(0) != 1) {
      failures.push_back("the chained lane made " +
                         std::to_string(rows[1].processes.value_or(0)) +
                         " tmux invocations, not one");
    }
    // Every command in the workload is one tmux answers inside its guarded
    // block, so a control lane that launches anything has fallen back.
    for (std::size_t index = 2; index < rows.size(); ++index) {
      if (rows[index].processes.value_or(-1) != 0) {
        failures.push_back(rows[index].mode + " made " +
                           std::to_string(rows[index].processes.value_or(-1)) +
                           " tmux invocations, not none");
      }
    }
    for (const std::string& failure : failures) {
      std::fprintf(stderr, "FAIL %s\n", failure.c_str());
    }
    std::printf(failures.empty() ? "ok: %zu rows, every claim holds\n"
                                 : "failed: %zu of the table's claims do not hold\n",
                failures.empty() ? rows.size() : failures.size());
    return failures.empty() ? 0 : 1;
  }

  const std::string version = capture("tmux -V | sed 's/^tmux //'");
  const std::string revision = capture("git rev-parse --short HEAD");
  const std::string processor =
      capture("awk -F': ' '/model name/{print $2; exit}' /proc/cpuinfo");
  const std::string threads = capture("nproc");
  const std::string machine = (processor.empty() ? "unnamed processor" : processor) +
                              ", " + threads + " threads, linux, clang";

  if (want_json) {
    std::string entries;
    for (const Row& row : rows) {
      if (!entries.empty()) {
        entries += ',';
      }
      entries +=
          "{\"answer\":" +
          (row.answer ? "\"" + *row.answer + "\"" : std::string{"null"}) +
          ",\"clients\":" +
          (row.clients ? std::to_string(*row.clients) : std::string{"null"}) +
          ",\"elapsed_ns\":" +
          (row.elapsed_ns ? std::to_string(*row.elapsed_ns) : std::string{"null"}) +
          ",\"mode\":\"" + row.mode + "\",\"processes\":" +
          (row.processes ? std::to_string(*row.processes) : std::string{"null"}) +
          ",\"reason\":" +
          (row.reason ? "\"" + *row.reason + "\"" : std::string{"null"}) +
          ",\"state\":\"" + row.state + "\"}";
    }
    std::printf("{\"machine\":\"%s\",\"panes_per_window\":%d,\"port\":\"cxx\","
                "\"port_revision\":\"%s\",\"rows\":[%s],\"schema\":1,"
                "\"tmux_version\":\"%s\",\"unit\":\"ns\"}\n",
                machine.c_str(), kPanesPerWindow, revision.c_str(), entries.c_str(),
                version.c_str());
    return 0;
  }

  std::printf("\nbuilding a %d-pane window, tmux %s\n", kPanesPerWindow,
              version.c_str());
  std::printf("libtmux at %s\n%s\n\n", revision.c_str(), machine.c_str());
  std::printf("%-20s %10s %11s %8s  query answer\n", "path", "wall", "processes",
              "clients");
  std::printf("%s\n", std::string(88, '-').c_str());
  for (const Row& row : rows) {
    if (row.state != "supported") {
      std::printf("%-20s %10s %11s %8s  unimplemented: %s\n", row.mode.c_str(), "-",
                  "-", "-", row.reason.value_or("").c_str());
      continue;
    }
    const std::string wall =
        std::to_string(row.elapsed_ns.value_or(0) / kNanosecondsPerMillisecond) + "ms";
    std::printf("%-20s %10s %11d %8d  %s\n", row.mode.c_str(), wall.c_str(),
                row.processes.value_or(0), row.clients.value_or(0),
                row.answer.value_or("").c_str());
  }
  return 0;
}
