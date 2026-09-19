#pragma once

#include "services.hpp"

#include <deque>
#include <locale.h>
#include <utility>

namespace libtmux::workspace::cli {
struct ProgressTerminal {
  std::function<std::pair<int, int>()> geometry;
  bool stdout_terminal{};
  bool colour{};
};
ProgressTerminal progress_terminal(const Request&, std::ostream&, std::ostream&);

class Progress {
  const Request& request_;
  std::ostream& output_;
  std::ostream& errors_;
  ProgressTerminal terminal_;
  std::pair<int, int> size_{};
  std::deque<std::string> tail_{1};
  locale_t locale_{};
  bool active_{}, drawn_{}, carriage_return_{};
  int lines_{3};
  std::string format_, session_, window_, path_;
  std::size_t windows_{}, window_index_{}, windows_done_{}, panes_{}, pane_index_{},
      panes_done_{}, session_panes_{}, session_done_{};
  std::chrono::steady_clock::time_point rendered_{};

  std::string clip(std::string_view, int) const;
  std::string heading() const;
  std::vector<std::string> panel() const;
  void append(std::string_view);
  void clear() noexcept;
  void draw(bool force = false);

public:
  Progress(const Request&, std::ostream&, std::ostream&, ProgressTerminal);
  ~Progress();
  Progress(const Progress&) = delete;
  Progress& operator=(const Progress&) = delete;
  void event(const std::string&, const Json&);
  void finish(bool retain_script_tail = false) noexcept;
};
} // namespace libtmux::workspace::cli
