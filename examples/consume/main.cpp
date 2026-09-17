// The smallest complete consumer of the installed package.
//
// It is deliberately runnable without a tmux server: continuous integration
// uses it to prove the package installs, exports, and links, which is a
// different question from whether the library talks to tmux correctly.
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <libtmux/libtmux.hpp>

// The value this example records for one pane field, by name. A name it does
// not know answers "0", which reads as an empty string, a zero, or a false
// flag depending on the field -- so a field added to the entity needs no edit
// here unless the filter below is meant to see it.
constexpr std::string_view recorded_pane_value(std::string_view field) {
  if (field == "pane_id") {
    return "%0";
  }
  if (field == "pane_current_command") {
    return "nvim";
  }
  if (field == "pane_active") {
    return "1";
  }
  if (field == "window_id") {
    return "@0";
  }
  if (field == "session_id") {
    return "$0";
  }
  if (field == "pane_title") {
    return "editor";
  }
  if (field == "pane_pid") {
    return "4210";
  }
  if (field == "pane_tty") {
    return "/dev/pts/3";
  }
  if (field == "pane_current_path") {
    return "/home";
  }
  if (field == "pane_width") {
    return "80";
  }
  if (field == "pane_height") {
    return "24";
  }
  return "0";
}

int main() {
  const auto version = libtmux::parse_version("tmux 3.7a");
  if (!version.has_value() || !libtmux::is_supported(*version)) {
    std::fputs("version parsing failed\n", stderr);
    return 1;
  }

  // A filter over recorded output, which reaches no tmux at all.
  //
  // One value per field of `Pane::kFields`, derived from the field names
  // rather than written out in their order. Two lanes build this same file
  // against different header sets -- the compile lane against this checkout,
  // the registry lane against the last released port, which vcpkg resolves
  // through `versions/` -- so a fixed row can only ever match one of them:
  // adding a pane field breaks the first, and matching the new count breaks
  // the second until a release is cut. Deriving it builds against both, and
  // a field this does not name records "0", which every field reads.
  static constexpr auto kRecordedPane = [] {
    std::array<std::string_view, libtmux::Pane::kFields.size()> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
      values[index] = recorded_pane_value(libtmux::Pane::kFields[index]);
    }
    return values;
  }();

  std::string row;
  for (const std::string_view value : kRecordedPane) {
    row += value;
    row += libtmux::kFormatSeparator;
  }
  const auto recorded =
      libtmux::Snapshot::from_recording(libtmux::Pane::kFields, row + "\n");
  if (recorded == nullptr) {
    std::fputs("the recording was refused\n", stderr);
    return 2;
  }
  const auto editing =
      libtmux::pane::command.starts_with("nv") && libtmux::pane::active;
  if (!editing(libtmux::Pane{recorded, 0})) {
    std::fputs("filtering failed\n", stderr);
    return 2;
  }

  // A chain refuses a target it cannot address, before reaching tmux.
  libtmux::Chain chain;
  chain.new_window("a:b", "unreachable");
  if (chain.valid()) {
    std::fputs("chain accepted an unaddressable session\n", stderr);
    return 3;
  }

  std::printf("libtmux %s consumed\n", std::string{libtmux::library_version()}.c_str());
  return 0;
}
