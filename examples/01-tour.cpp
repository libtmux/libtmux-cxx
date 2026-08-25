// Five minutes with the library: connect, look around, act, read the result.

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <libtmux/libtmux.hpp>

#include "scratch_server.hpp"

namespace {

void append_json_string(std::string& output, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  output.push_back('\"');
  for (const char value_character : value) {
    const unsigned char character = static_cast<unsigned char>(value_character);
    switch (character) {
    case '\"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U || character >= 0x80U) {
        output += "\\u00";
        output.push_back(hex[character >> 4U]);
        output.push_back(hex[character & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('\"');
}

int print_arena_evidence(const example::ScratchServer& scratch,
                         const libtmux::Server& server) {
  const auto identity = server.expand("#{pid}\t#{socket_path}");
  if (!identity.has_value()) {
    std::fprintf(stderr, "%s\n", identity.error().diagnostic.c_str());
    return 1;
  }

  const std::size_t separator = identity->find('\t');
  if (separator == std::string::npos) {
    std::fprintf(stderr, "invalid arena server identity\n");
    return 1;
  }
  int server_pid = 0;
  const char* const pid_end = identity->data() + separator;
  const auto parsed = std::from_chars(identity->data(), pid_end, server_pid);
  const std::string socket_path = identity->substr(separator + 1U);
  if (parsed.ec != std::errc{} || parsed.ptr != pid_end || server_pid <= 0 ||
      socket_path != scratch.socket_path()) {
    std::fprintf(stderr, "invalid arena server identity\n");
    return 1;
  }

  const auto global_options = server.global_options();
  if (!global_options.has_value()) {
    std::fprintf(stderr, "%s\n", global_options.error().diagnostic.c_str());
    return 1;
  }
  std::string_view challenge;
  for (const libtmux::OptionEntry& option : *global_options) {
    if (option.name == "@libtmux_arena_challenge" && !option.index.has_value()) {
      challenge = option.value;
      break;
    }
  }
  if (challenge.empty()) {
    std::fprintf(stderr, "arena challenge is missing\n");
    return 1;
  }

  std::string evidence{"LIBTMUX_ARENA_EVIDENCE={\"schema\":1,\"server_pid\":"};
  evidence += std::to_string(server_pid);
  evidence += ",\"socket_path\":";
  append_json_string(evidence, socket_path);
  evidence += ",\"challenge\":";
  append_json_string(evidence, challenge);
  evidence += ",\"artifact\":\"libtmux_example_01_tour\"}\n";
  std::fputs(evidence.c_str(), stdout);
  return 0;
}

} // namespace

int main() {
  const example::ScratchServer scratch =
      example::ScratchServer::open_or_borrow_arena("libtmux_example_01_tour");
  const libtmux::Server& server = scratch.get();

  // Which tmux is on the other end. Nothing throws; every call reports failure
  // as a value, so every result is checked.
  const auto version = server.tmux_version();
  if (!version.has_value()) {
    std::fprintf(stderr, "%s\n", version.error().diagnostic.c_str());
    return 1;
  }
  std::printf("tmux %u.%u\n", version->major, version->minor);

  const auto sessions = server.sessions();
  if (!sessions.has_value()) {
    std::fprintf(stderr, "%s\n", sessions.error().diagnostic.c_str());
    return 1;
  }
  for (const libtmux::Session& session : *sessions) {
    std::printf("session %s with %lld window(s)\n", std::string{session.name()}.c_str(),
                session.window_count());
  }

  // Traversal: a session knows its windows, a window its panes, and a pane the
  // way back up. None of it needs a target string.
  const libtmux::Session& session = sessions->at(0);
  const auto window = session.new_window({.name = "tour"});
  if (!window.has_value()) {
    std::fprintf(stderr, "%s\n", window.error().diagnostic.c_str());
    return 1;
  }

  const auto panes = window->panes();
  if (!panes.has_value()) {
    std::fprintf(stderr, "%s\n", panes.error().diagnostic.c_str());
    return 1;
  }
  const libtmux::Pane& pane = panes->at(0);
  std::printf("made %s, holding %s\n", std::string{window->name()}.c_str(),
              std::string{pane.id()}.c_str());

  // A pane is a value: it prints, compares and can be stored.
  const auto owner = pane.window();
  if (owner.has_value()) {
    std::printf("its window is %s, the one just made: %s\n",
                std::string{owner->id()}.c_str(), *owner == *window ? "yes" : "no");
  }

  // An entity reads the moment it was listed. Ask again for the present.
  if (const auto renamed = window->rename("toured"); !renamed.has_value()) {
    std::fprintf(stderr, "%s\n", renamed.error().diagnostic.c_str());
    return 1;
  }
  const auto current = window->refresh();
  std::printf("was %s, now %s\n", std::string{window->name()}.c_str(),
              current.has_value() ? std::string{current->name()}.c_str() : "?");

  // Moving the selection is tmux's job: next and previous wrap the window
  // list, and "last" is the window that was selected before this one, which
  // nothing in a listing tells you.
  const auto next = session.select_next_window();
  const auto previous = session.select_previous_window();
  const auto last = session.select_last_window();
  std::printf("next %s, previous %s, last %s\n",
              next.has_value() ? std::string{next->name()}.c_str() : "refused",
              previous.has_value() ? std::string{previous->name()}.c_str() : "refused",
              last.has_value() ? std::string{last->name()}.c_str() : "refused");
  // Copy mode is pane state, so a program can enter it with nobody
  // attached — which is how scrollback gets read without a person.
  if (const auto entered = pane.enter_copy_mode(); !entered.has_value()) {
    std::fprintf(stderr, "%s\n", entered.error().diagnostic.c_str());
    return 1;
  }
  std::printf("in a mode: %s\n", pane.refresh()
                                     .transform([](const libtmux::Pane& p) {
                                       return p.in_mode() ? "yes" : "no";
                                     })
                                     .value_or("?"));
  if (const auto left = pane.leave_mode(); !left.has_value()) {
    std::fprintf(stderr, "%s\n", left.error().diagnostic.c_str());
    return 1;
  }

  // A pane's output can be copied to a command while it runs, which is how
  // a program watches a pane without polling it.
  if (const auto piping = pane.pipe_to("cat > /dev/null"); !piping.has_value()) {
    std::fprintf(stderr, "%s\n", piping.error().diagnostic.c_str());
    return 1;
  }
  std::printf("piping: %s\n", pane.refresh()
                                  .transform([](const libtmux::Pane& p) {
                                    return p.piping() ? "yes" : "no";
                                  })
                                  .value_or("?"));
  if (const auto stopped = pane.stop_piping(); !stopped.has_value()) {
    std::fprintf(stderr, "%s\n", stopped.error().diagnostic.c_str());
    return 1;
  }

  // A pane can be named, and the name outlives whatever runs in it.
  if (const auto named = pane.set_title("toured pane"); !named.has_value()) {
    std::fprintf(stderr, "%s\n", named.error().diagnostic.c_str());
    return 1;
  }

  // Scrollback is the pane's memory, and dropping it is a separate act
  // from clearing what is on screen.
  if (const auto dropped = pane.clear_history(); !dropped.has_value()) {
    std::fprintf(stderr, "%s\n", dropped.error().diagnostic.c_str());
    return 1;
  }

  // What this tmux can do, asked rather than deduced from its version.
  const auto known = server.commands();
  std::printf("commands understood: %zu\n", known.has_value() ? known->size() : 0U);

  // Anything tmux knows and this library does not name, asked for directly.
  // The pane's running command changes under a value that does not.
  if (const auto running = pane.expand("#{pane_current_command}");
      running.has_value()) {
    std::printf("the pane is running %s\n", running->c_str());
  }

  // A buffer is the server's clipboard: named text that outlives the pane
  // it came from.
  if (const auto put = server.set_buffer("tour", "copied text"); !put.has_value()) {
    std::fprintf(stderr, "%s\n", put.error().diagnostic.c_str());
    return 1;
  }
  const auto buffers = server.buffers();
  std::printf("buffers held: %zu\n", buffers.has_value() ? buffers->size() : 0U);
  if (buffers.has_value() && !buffers->empty()) {
    // A buffer is a thing, not a string the server hands out: it reads its
    // own contents and takes itself away.
    if (const auto held = buffers->front().contents(); held.has_value()) {
      std::printf("the buffer holds %zu byte(s)\n", held->size());
    }
    // Pasting puts the text on the pane's command line without running it.
    if (const auto delivered = pane.paste(buffers->front()); !delivered.has_value()) {
      std::fprintf(stderr, "%s\n", delivered.error().diagnostic.c_str());
      return 1;
    }
  }

  // A buffer can come from, and go back to, a file the server can reach.
  const auto buffer_file = std::filesystem::temp_directory_path() / "libtmux-tour.txt";
  if (const auto saved = server.save_buffer("tour", buffer_file); saved.has_value()) {
    std::printf("saved the buffer to %s\n", buffer_file.c_str());
    // And back again, into a buffer of its own.
    if (const auto reloaded = server.load_buffer("reloaded", buffer_file);
        !reloaded.has_value()) {
      std::fprintf(stderr, "%s\n", reloaded.error().diagnostic.c_str());
      return 1;
    }
    std::error_code ignored;
    std::filesystem::remove(buffer_file, ignored);
  }

  // Buffers are named, so one can be dropped without touching the others.
  if (const auto held = server.buffers(); held.has_value()) {
    for (const libtmux::Buffer& buffer : *held) {
      if (buffer.name() == "reloaded" && !buffer.remove().has_value()) {
        std::fprintf(stderr, "could not drop the reloaded buffer\n");
        return 1;
      }
    }
  }

  // Attaching needs a terminal this library does not have, so it hands back
  // the command to spawn rather than a call that could only fail.
  const auto attach = session.attach_command();
  if (attach.has_value()) {
    std::printf("attach with:");
    for (const std::string& argument : attach->argv()) {
      std::printf(" %s", argument.c_str());
    }
    std::printf("\n");
  }

  // And the other direction, which is an ordinary call. On a scratch server
  // nobody is attached, and tmux refuses rather than treating that as done:
  // "no current client" is the answer, not silence.
  if (const auto sent = session.detach_clients(); !sent.has_value()) {
    std::printf("nobody to send away: %s\n", sent.error().diagnostic.c_str());
  }

  // A client is whoever is attached. A detached scratch server has none,
  // and an empty listing is the answer rather than a failure.
  const auto clients = server.clients();
  std::printf("clients attached: %zu\n", clients.has_value() ? clients->size() : 0U);
  if (clients.has_value()) {
    for (const libtmux::Client& client : *clients) {
      std::printf("  %s on %s\n", std::string{client.name()}.c_str(),
                  std::string{client.tty()}.c_str());
    }
  }

  // Replacing what runs in a pane. tmux refuses while a process is still
  // there unless told plainly, which is why this asks.
  if (const auto replaced = pane.respawn(/*replace_running=*/true);
      !replaced.has_value()) {
    std::fprintf(stderr, "%s\n", replaced.error().diagnostic.c_str());
    return 1;
  }
  if (scratch.borrows_server()) {
    return print_arena_evidence(scratch, server);
  }
  return 0;
}
