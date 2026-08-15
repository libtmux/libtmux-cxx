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

int main() {
  const auto version = libtmux::parse_version("tmux 3.7a");
  if (!version.has_value() || !libtmux::is_supported(*version)) {
    std::fputs("version parsing failed\n", stderr);
    return 1;
  }

  // A filter over recorded output, which reaches no tmux at all.
  //
  // One value per field of `Pane::kFields`, in that order. The assertion below
  // is the point: a field added to the entity leaves this row one column short,
  // and `from_recording` would answer with nothing at run time in a program
  // whose whole job is to be built. Better to not compile.
  static constexpr std::array kRecordedPane{
      std::string_view{"%0"},         std::string_view{"nvim"},
      std::string_view{"1"},          std::string_view{"@0"},
      std::string_view{"$0"},         std::string_view{"0"},
      std::string_view{"editor"},     std::string_view{"4210"},
      std::string_view{"/dev/pts/3"}, std::string_view{"/home"},
      std::string_view{"80"},         std::string_view{"24"},
      std::string_view{"0"},          std::string_view{"0"},
      std::string_view{"1"},          std::string_view{"0"},
      std::string_view{"1"},          std::string_view{"1"},
      std::string_view{"0"}};
  static_assert(kRecordedPane.size() == libtmux::Pane::kFields.size(),
                "one recorded value per pane field, in the order kFields lists them");

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
