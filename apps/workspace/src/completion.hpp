#pragma once

#include <iosfwd>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CLI {
class App;
class Option;
} // namespace CLI

namespace libtmux::workspace::cli {
using CompletionChoices = std::map<const CLI::Option*, std::vector<std::string>>;
void complete(const CLI::App& root, const CompletionChoices& choices,
              std::span<const std::string> words, std::ostream& output);
std::string completion_script(std::string_view shell);
} // namespace libtmux::workspace::cli
