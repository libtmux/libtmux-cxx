#include "progress.hpp"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace libtmux::workspace::cli {
namespace {
std::string environment(const char* name) {
  const auto* value = std::getenv(name);
  return value ? value : "";
}
bool shared_output_terminal() {
  struct stat output {};
  struct stat errors {};
  return ::isatty(STDOUT_FILENO) != 0 && ::fstat(STDOUT_FILENO, &output) == 0 &&
         ::fstat(STDERR_FILENO, &errors) == 0 && output.st_dev == errors.st_dev &&
         output.st_ino == errors.st_ino;
}
std::string fraction(std::size_t done, std::size_t total) {
  return std::to_string(done) + "/" + std::to_string(total);
}
std::string bar(std::size_t done, std::size_t total) {
  const auto filled = total ? std::min<std::size_t>(10, done * 10 / total) : 0;
  return "[" + std::string(filled, '=') + std::string(10 - filled, ' ') + "]";
}
std::string expand(std::string_view format,
                   const std::map<std::string, std::string>& fields) {
  std::string result;
  for (std::size_t index = 0; index < format.size();) {
    const char byte = format[index];
    if ((byte == '{' || byte == '}') && index + 1 < format.size() &&
        format[index + 1] == byte) {
      result += byte;
      index += 2;
    } else if (byte == '{') {
      const auto end = format.find('}', index + 1);
      if (end == std::string_view::npos) {
        result.append(format.substr(index));
        break;
      }
      const auto key = std::string{format.substr(index + 1, end - index - 1)};
      const auto found = fields.find(key);
      result += found == fields.end()
                    ? std::string{format.substr(index, end - index + 1)}
                    : found->second;
      index = end + 1;
    } else {
      result += byte;
      ++index;
    }
  }
  return result;
}
} // namespace

ProgressTerminal progress_terminal(const Request& request, std::ostream& output,
                                   std::ostream& errors) {
  const auto term = environment("TERM");
  if (request.machine() || request.command != "load" || request.flag("no-progress") ||
      environment("TMUXP_PROGRESS") == "0" || term.empty() || term == "dumb" ||
      &errors != &std::cerr || ::isatty(STDERR_FILENO) == 0)
    return {};
  return {.geometry =
              [] {
                winsize size{};
                if (::ioctl(STDERR_FILENO, TIOCGWINSZ, &size) != 0)
                  return std::pair{0, 0};
                return std::pair{static_cast<int>(size.ws_col),
                                 static_cast<int>(size.ws_row)};
              },
          .stdout_terminal = &output == &std::cout && shared_output_terminal(),
          .colour =
              environment("NO_COLOR").empty() && request.value("color") != "never"};
}
Progress::Progress(const Request& request, std::ostream& output, std::ostream& errors,
                   ProgressTerminal terminal)
    : request_{request}, output_{output}, errors_{errors},
      terminal_{std::move(terminal)} {
  if (!terminal_.geometry)
    return;
  size_ = terminal_.geometry();
  active_ = size_.first >= 4 && size_.second >= 3;
  if (!active_)
    return;
  lines_ = std::stoi(request.value("progress-lines", "3"));
  format_ = request.value("progress-format", "default");
  const std::map<std::string, std::string> presets{
      {"default", "Loading workspace: {session} {bar} {progress} {window}"},
      {"minimal", "Loading workspace: {session} [{window_progress}]"},
      {"window", "Loading workspace: {session} {window_bar} {window_progress_rel}"},
      {"pane", "Loading workspace: {session} {pane_bar} {session_pane_progress}"},
      {"verbose", "Loading workspace: {session} [window {window_index} of "
                  "{window_total} - pane {pane_index} of {pane_total}] {window}"}};
  if (const auto preset = presets.find(format_); preset != presets.end())
    format_ = preset->second;
  locale_ = ::newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
  if (!locale_)
    locale_ = ::newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
  if (!locale_)
    locale_ = ::newlocale(LC_CTYPE_MASK, "C", nullptr);
}
Progress::~Progress() {
  finish();
  if (locale_)
    ::freelocale(locale_);
}
std::string Progress::clip(std::string_view text, int columns) const {
  struct RestoreLocale {
    locale_t previous;
    ~RestoreLocale() {
      if (previous)
        (void)::uselocale(previous);
    }
  } restore{locale_ ? ::uselocale(locale_) : locale_t{}};
  std::mbstate_t state{};
  std::string result;
  int width{};
  while (!text.empty()) {
    wchar_t character{};
    auto bytes = std::mbrtowc(&character, text.data(), text.size(), &state);
    int cells = -1;
    if (bytes != static_cast<std::size_t>(-1) &&
        bytes != static_cast<std::size_t>(-2) && bytes != 0)
      cells = ::wcwidth(character);
    if (cells < 0) {
      bytes = 1;
      state = {};
      cells = 1;
      if (width + cells > columns)
        break;
      result += '?';
    } else {
      if (width + cells > columns)
        break;
      result.append(text.substr(0, bytes));
    }
    width += cells;
    text.remove_prefix(bytes);
  }
  return result;
}
std::string Progress::heading() const {
  const auto percent = session_panes_ ? session_done_ * 100 / session_panes_ : 0;
  std::map<std::string, std::string> fields{
      {"workspace_path", path_},
      {"session", session_},
      {"window", window_},
      {"window_index", std::to_string(window_index_)},
      {"window_total", std::to_string(windows_)},
      {"window_progress", fraction(window_index_, windows_)},
      {"pane_index", std::to_string(pane_index_)},
      {"pane_total", std::to_string(panes_)},
      {"pane_progress", fraction(pane_index_, panes_)},
      {"progress", std::to_string(percent) + "%"},
      {"windows_done", std::to_string(windows_done_)},
      {"windows_remaining", std::to_string(windows_ - windows_done_)},
      {"window_progress_rel", fraction(windows_done_, windows_)},
      {"pane_done", std::to_string(panes_done_)},
      {"pane_remaining", std::to_string(panes_ - panes_done_)},
      {"pane_progress_rel", fraction(panes_done_, panes_)},
      {"session_pane_total", std::to_string(session_panes_)},
      {"session_panes_done", std::to_string(session_done_)},
      {"session_panes_remaining", std::to_string(session_panes_ - session_done_)},
      {"session_pane_progress", fraction(session_done_, session_panes_)},
      {"overall_percent", std::to_string(percent)},
      {"summary", fraction(windows_done_, windows_) + " windows, " +
                      fraction(session_done_, session_panes_) + " panes"},
      {"bar", bar(session_done_, session_panes_)},
      {"pane_bar", bar(panes_done_, panes_)},
      {"window_bar", bar(windows_done_, windows_)},
      {"status_icon", session_done_ == session_panes_ ? "+" : ">"}};
  return expand(format_, fields);
}
void Progress::append(std::string_view text) {
  for (char byte : text) {
    if (byte == '\n') {
      carriage_return_ = false;
      tail_.emplace_back();
      if (tail_.size() > 256)
        tail_.pop_front();
    } else if (byte == '\r') {
      carriage_return_ = true;
    } else {
      if (carriage_return_) {
        tail_.back().clear();
        carriage_return_ = false;
      }
      if (tail_.back().size() < 4096)
        tail_.back() += byte;
    }
  }
}
void Progress::clear() noexcept {
  if (!drawn_)
    return;
  try {
    errors_ << "\r\033[J" << std::flush;
  } catch (...) {
  }
  drawn_ = false;
}
void Progress::finish(bool retain_script_tail) noexcept {
  clear();
  if (active_ && retain_script_tail) {
    try {
      for (const auto& line : panel())
        errors_ << line << '\n';
      errors_.flush();
    } catch (...) {
    }
  }
  active_ = false;
}
std::vector<std::string> Progress::panel() const {
  std::vector<std::string> result;
  const auto rows = static_cast<std::size_t>(
      std::clamp(lines_ == -1 ? size_.second - 2 : lines_, 0, size_.second - 2));
  const auto end = tail_.size() - (tail_.back().empty() ? 1U : 0U);
  for (auto index = end - std::min(end, rows); index < end; ++index)
    result.push_back(clip(tail_[index], size_.first - 1));
  return result;
}
void Progress::draw(bool force) {
  if (!active_)
    return;
  if (terminal_.geometry() != size_) {
    finish();
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - rendered_ < std::chrono::milliseconds{50})
    return;
  rendered_ = now;
  clear();
  auto title = clip(heading(), size_.first - 1);
  if (terminal_.colour)
    title = "\033[36m" + title + "\033[0m";
  std::vector<std::string> frame{std::move(title)};
  const auto script = panel();
  frame.insert(frame.end(), script.begin(), script.end());
  try {
    drawn_ = true;
    for (const auto& line : frame)
      errors_ << line << '\n';
    errors_ << "\033[" << frame.size() << "A" << std::flush;
    if (!errors_)
      finish();
  } catch (...) {
    finish();
  }
}
void Progress::event(const std::string& name, const Json& data) {
  if (request_.machine() || request_.command != "load")
    return;
  if (name == "script-output") {
    if (active_) {
      append(data.at("text").get_ref<const std::string&>());
      draw();
    }
    if (!active_ || (data.at("stream") == "stdout" && !terminal_.stdout_terminal)) {
      auto& destination = data.at("stream") == "stdout" ? output_ : errors_;
      destination << data.at("text").get_ref<const std::string&>() << std::flush;
      if (!destination)
        throw Failure{1, "OUTPUT_CLOSED", "script output stream closed"};
    }
    return;
  }
  if (!active_)
    return;
  if (name == "workspace-started") {
    session_ = data.at("session_name");
    path_ = data.at("input");
    windows_ = data.at("window_total");
    session_panes_ = data.at("session_pane_total");
    windows_done_ = session_done_ = window_index_ = pane_index_ = panes_done_ = panes_ =
        0;
    window_.clear();
    tail_.assign(1, "");
    carriage_return_ = false;
  } else if (name == "build-progress") {
    window_ = data.at("window_name");
    window_index_ = data.at("window_index");
    pane_index_ = data.at("pane_index");
    panes_ = data.at("pane_total");
    const auto& phase = data.at("phase");
    if (phase == "window-started")
      panes_done_ = 0;
    else if (phase == "pane-completed") {
      ++panes_done_;
      ++session_done_;
    } else if (phase == "window-completed")
      ++windows_done_;
  } else if (name == "workspace-completed") {
    windows_done_ = windows_;
    session_done_ = session_panes_;
  } else if (name != "script-started" && name != "script-completed") {
    return;
  }
  draw(true);
}
} // namespace libtmux::workspace::cli
