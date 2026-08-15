// What failure looks like. Nothing throws: every call hands back a value that
// says whether tmux ran, what it said, and whether trying again is safe.

#include <chrono>
#include <cstdio>
#include <string>

#include <libtmux/libtmux.hpp>

#include "scratch_server.hpp"

namespace {

void report(const char* attempt, const libtmux::CommandFailure& failure) {
  std::printf("%-28s %-34s dispatched=%s exit=%d\n", attempt,
              std::string{libtmux::to_string(failure.kind)}.c_str(),
              failure.dispatched ? "yes" : "no", failure.exit_code);
  std::printf("%-28s %s\n", "", failure.diagnostic.c_str());
}

} // namespace

int main() {
  const example::ScratchServer scratch = example::ScratchServer::open();
  const libtmux::Server& server = scratch.get();

  // Rejected here, before tmux ran: nothing happened, so retrying is free.
  if (const auto empty = server.new_session(""); !empty.has_value()) {
    report("an empty session name", empty.error());
  }

  // tmux ran and said no. The diagnostic names the command as well as the
  // complaint.
  if (const auto absent = server.run({"kill-session", "-t", "absent"});
      !absent.has_value()) {
    report("killing what is not there", absent.error());
  }

  // tmux ran, said yes, and there was no such object. Distinct from a refusal
  // because tmux answers a question about a missing object with a zero exit
  // status and empty fields.
  if (const auto gone = server.window("@999"); !gone.has_value()) {
    report("a window that never was", gone.error());
  }

  // An answer larger than the caller is prepared to hold is reported rather
  // than cut, because a cut answer reads exactly like a complete one.
  if (const auto big = server.run({"list-panes", "-a", "-F", "#{pane_id}"}, {}, 1U);
      !big.has_value()) {
    report("an answer that did not fit", big.error());
  }

  // A socket nobody is serving. `is_alive` is the same question without the
  // reason, and is bounded so it cannot hang.
  const auto elsewhere = libtmux::Server::at_socket_path("/nonexistent/socket");
  if (elsewhere.has_value()) {
    std::printf("%-28s alive=%s\n", "a socket with no server",
                elsewhere->is_alive() ? "yes" : "no");
    if (const auto dead = elsewhere->check_alive(); !dead.has_value()) {
      report("and why not", dead.error());
    }
  }
  // A channel nobody signals is a timeout, not a hang: the deadline rides
  // on the call.
  if (const auto waited =
          server.wait_for("nobody-signals-this", std::chrono::milliseconds{300});
      !waited.has_value()) {
    std::printf("waiting timed out, as it should: %s\n",
                waited.error().diagnostic.c_str());
  }

  // Ending the server, and what every call reports afterwards. This is the
  // one failure a program causes on purpose, and it looks like all the
  // others: a value, not an exception.
  if (const auto ended = server.kill(); !ended.has_value()) {
    report("ending the server", ended.error());
  }
  if (const auto after = server.session("any"); !after.has_value()) {
    report("asking a server that has gone", after.error());
  }
  return 0;
}
