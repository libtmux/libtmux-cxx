// The smallest complete consumer of the installed package.
//
// It is deliberately runnable without a tmux server: continuous integration
// uses it to prove the package installs, exports, and links, which is a
// different question from whether the library talks to tmux correctly.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <libtmux/libtmux.hpp>

int main() {
  const auto version = libtmux::parse_version("tmux 3.7a");
  if (!version.has_value() || !libtmux::is_supported(*version)) {
    std::fputs("version parsing failed\n", stderr);
    return 1;
  }

  // A filter over recorded output, which reaches no tmux at all.
  std::string row;
  for (const std::string_view value :
       {"%0", "nvim", "1", "@0", "$0", "0", "editor", "4210", "/dev/pts/3", "/home",
        "80", "24", "0", "0", "1", "0", "1", "1"}) {
    row += value;
    row += libtmux::kFormatSeparator;
  }
  const auto recorded =
      libtmux::Snapshot::from_recording(libtmux::Pane::kFields, row + "\n");
  const auto editing =
      libtmux::pane::command.starts_with("nv") && libtmux::pane::active;
  if (recorded == nullptr || !editing(libtmux::Pane{recorded, 0})) {
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
