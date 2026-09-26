#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace libtmux::workspace::cli {

// Construct a fresh command tree and execute it without terminating the caller.
int run(std::vector<std::string> arguments, std::istream& input, std::ostream& output,
        std::ostream& errors);

} // namespace libtmux::workspace::cli
