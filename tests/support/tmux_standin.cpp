// A stand-in for the tmux binary, copied under the name a test puts ahead of
// the real tmux, or passes as the executable to run.
//
// What it does is read from `<its own path>.standin`, written by
// support/standin.hpp or by CMake: one `key<TAB>value` per line, with `\n`,
// `\t` and `\\` escaped in the value. `mode` selects the behaviour. Every
// mode but `client` and `reporting` ends by running the real tmux in its place
// or by exiting, so the library under test sees an ordinary tmux.
//
//   client     print `stderr`, leave a `sleep` of `descendant` seconds holding
//              stdout as a pre-3.2 client does, exit with `status`
//   reporting  answer the version probe as a daemon of `release` does, and
//              find no server for anything else
//   log        append each argument and then `--` to `log`; with
//              LIBTMUX_TEST_REWRITE_FENCE set, rewrite the version fence of a
//              format to 9.9z; run `real`
//   canary     append the arguments and whether TMUX, TMUX_PANE and TMUX_TMPDIR
//              arrived to `trace`, as words, never values; `sandbox` is the
//              TMUX_TMPDIR that counts as the sandbox
//   record     append each argument, `END` and whether TMUX arrived to `trace`,
//              each NUL-terminated
//   killserver run `real`, but make `kill-server` do nothing (LIBTMUX_WRAPPER_MODE
//              is `noop`) or happen a second later (`slow`)
//   tee        copy stdin to `record` while running `real` with it
//   count      append a line to `records`, then run `real` found on PATH

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

using Config = std::map<std::string, std::string>;
using Arguments = std::vector<std::string>;

std::string own_path() {
  std::string buffer(4096, '\0');
#if defined(__APPLE__)
  auto size = static_cast<std::uint32_t>(buffer.size());
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  buffer.resize(std::strlen(buffer.c_str()));
  return buffer;
#else
  const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (length < 0) {
    return {};
  }
  buffer.resize(static_cast<std::size_t>(length));
  return buffer;
#endif
}

std::string unescaped(std::string_view text) {
  std::string out;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '\\' || index + 1U == text.size()) {
      out.push_back(text[index]);
      continue;
    }
    const char next = text[++index];
    out.push_back(next == 'n' ? '\n' : next == 't' ? '\t' : next);
  }
  return out;
}

Config read_config(const std::string& path) {
  Config config;
  std::ifstream file{path};
  for (std::string line; std::getline(file, line);) {
    const auto tab = line.find('\t');
    if (tab != std::string::npos) {
      config[line.substr(0, tab)] = unescaped(std::string_view{line}.substr(tab + 1U));
    }
  }
  return config;
}

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "tmux_standin: %s\n", message.c_str());
  _exit(124);
}

const std::string& need(const Config& config, const std::string& key) {
  const auto found = config.find(key);
  if (found == config.end()) {
    fail("missing setting " + key);
  }
  return found->second;
}

std::optional<std::string> environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::nullopt : std::optional<std::string>{value};
}

void append(const std::string& path, std::string_view bytes) {
  const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (file < 0) {
    fail("cannot open " + path);
  }
  std::size_t done = 0;
  while (done < bytes.size()) {
    const auto wrote = ::write(file, bytes.data() + done, bytes.size() - done);
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    if (wrote <= 0) {
      break;
    }
    done += static_cast<std::size_t>(wrote);
  }
  static_cast<void>(::close(file));
}

// Replace this process with `program`, found on PATH when it has no slash.
[[noreturn]] void run(const std::string& program, const Arguments& arguments) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(program.c_str()));
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  ::execvp(program.c_str(), argv.data());
  fail("cannot run " + program + ": " + std::strerror(errno));
}

bool contains(const Arguments& arguments, std::string_view word) {
  for (const auto& argument : arguments) {
    if (argument == word) {
      return true;
    }
  }
  return false;
}

[[noreturn]] void client(const Config& config) {
  std::fprintf(stderr, "%s\n", need(config, "stderr").c_str());
  const auto seconds = config.contains("descendant") ? config.at("descendant") : "60";
  if (::fork() == 0) {
    const int null = ::open("/dev/null", O_WRONLY);
    static_cast<void>(::dup2(null, STDERR_FILENO));
    ::execlp("sleep", "sleep", seconds.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  _exit(std::atoi(need(config, "status").c_str()));
}

[[noreturn]] void reporting(const Config& config, const Arguments& arguments) {
  const auto probing = contains(arguments, "run-shell");
  if (!probing) {
    std::fprintf(stderr, "no server running\n");
    _exit(1);
  }
  std::string last = arguments.empty() ? std::string{} : arguments.back();
  constexpr std::string_view suffix = "#{version}";
  if (last.size() >= suffix.size() &&
      std::string_view{last}.substr(last.size() - suffix.size()) == suffix) {
    last.resize(last.size() - suffix.size());
  }
  std::fprintf(stderr, "unknown command: %s%s\n", last.c_str(),
               need(config, "release").c_str());
  _exit(1);
}

[[noreturn]] void log(const Config& config, Arguments arguments) {
  std::string entry;
  for (const auto& argument : arguments) {
    entry += argument + "\n";
  }
  append(need(config, "log"), entry + "--\n");
  const auto rewrite = environment("LIBTMUX_TEST_REWRITE_FENCE");
  if (rewrite.has_value() && !rewrite->empty()) {
    constexpr std::string_view fence = "#{==:#{version},";
    for (auto& argument : arguments) {
      if (std::string_view{argument}.starts_with(fence)) {
        argument = "#{==:#{version},9.9z}";
      }
    }
  }
  run(need(config, "real"), arguments);
}

std::string presence(const char* name) {
  return std::string{name} + (std::getenv(name) != nullptr ? "=set" : "=unset");
}

[[noreturn]] void canary(const Config& config, const Arguments& arguments) {
  std::string out = "ARGV";
  for (const auto& argument : arguments) {
    out += " [" + argument + "]";
  }
  out += "\n" + presence("TMUX") + "\n" + presence("TMUX_PANE") + "\n";
  const auto tmpdir = environment("TMUX_TMPDIR");
  out += "TMUX_TMPDIR=";
  out += !tmpdir.has_value()                  ? "unset"
         : *tmpdir == need(config, "sandbox") ? "sandbox"
                                              : "other";
  out += "\n";
  append(need(config, "trace"), out);
  _exit(0);
}

[[noreturn]] void record(const Config& config, const Arguments& arguments) {
  std::string out;
  for (const auto& argument : arguments) {
    out.append(argument).push_back('\0');
  }
  out.append("END").push_back('\0');
  out.append(presence("TMUX")).push_back('\0');
  append(need(config, "trace"), out);
  _exit(0);
}

[[noreturn]] void kill_server(const Config& config, const Arguments& arguments) {
  const auto mode = environment("LIBTMUX_WRAPPER_MODE").value_or("");
  if (contains(arguments, "kill-server")) {
    if (mode == "noop") {
      _exit(0);
    }
    if (mode == "slow") {
      if (::fork() == 0) {
        const int null = ::open("/dev/null", O_RDWR);
        static_cast<void>(::dup2(null, STDOUT_FILENO));
        static_cast<void>(::dup2(null, STDERR_FILENO));
        ::sleep(1);
        run(need(config, "real"), arguments);
      }
      _exit(0);
    }
  }
  run(need(config, "real"), arguments);
}

[[noreturn]] void tee(const Config& config, const Arguments& arguments) {
  // The real tmux may exit first; a write to it then fails with EPIPE.
  static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    fail("cannot make a pipe");
  }
  const pid_t child = ::fork();
  if (child == 0) {
    static_cast<void>(::dup2(pipe_fds[0], STDIN_FILENO));
    static_cast<void>(::close(pipe_fds[0]));
    static_cast<void>(::close(pipe_fds[1]));
    run(need(config, "real"), arguments);
  }
  static_cast<void>(::close(pipe_fds[0]));
  const std::string record_path = need(config, "record");
  std::thread copier{[&] {
    char buffer[4096];
    for (;;) {
      const auto got = ::read(STDIN_FILENO, buffer, sizeof buffer);
      if (got < 0 && errno == EINTR) {
        continue;
      }
      if (got <= 0) {
        break;
      }
      append(record_path, std::string_view{buffer, static_cast<std::size_t>(got)});
      if (::write(pipe_fds[1], buffer, static_cast<std::size_t>(got)) < 0) {
        break;
      }
    }
    static_cast<void>(::close(pipe_fds[1]));
  }};
  copier.detach();
  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

[[noreturn]] void count(const Config& config, const Arguments& arguments) {
  const std::string records = need(config, "records");
  const int file = ::open(records.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (file < 0 || ::write(file, "1\n", 2) != 2) {
    _exit(125);
  }
  static_cast<void>(::close(file));
  run(need(config, "real"), arguments);
}

} // namespace

int main(int argc, char** argv) {
  const Arguments arguments{argv + 1, argv + argc};
  const auto config = read_config(own_path() + ".standin");
  const std::string mode = need(config, "mode");
  if (mode == "client") {
    client(config);
  } else if (mode == "reporting") {
    reporting(config, arguments);
  } else if (mode == "log") {
    log(config, arguments);
  } else if (mode == "canary") {
    canary(config, arguments);
  } else if (mode == "record") {
    record(config, arguments);
  } else if (mode == "killserver") {
    kill_server(config, arguments);
  } else if (mode == "tee") {
    tee(config, arguments);
  } else if (mode == "count") {
    count(config, arguments);
  }
  fail("unknown mode " + mode);
}
