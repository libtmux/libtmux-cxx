// Read tmuxp documents and report which this parser understands.
//
// The point is somebody else's documents: tests are written against the
// shapes their author already had in mind, and a corpus is not. Refusals are
// the output, not a failure — see docs/design/workspace-corpus.md.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "libtmux_consumers/tmuxp.hpp"

namespace {

// The file name alone, so a report of thirty documents stays readable.
std::string_view leaf(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs("usage: corpus-probe <document.yaml>...\n", stderr);
    return 2;
  }
  int understood = 0;
  int refused = 0;
  for (int index = 1; index < argc; ++index) {
    std::ifstream reading{argv[index]};
    if (!reading) {
      std::printf("  unreadable  %s\n", argv[index]);
      ++refused;
      continue;
    }
    std::ostringstream document;
    document << reading.rdbuf();
    const auto parsed = libtmux::workspace::parse_tmuxp(document.str());
    const std::string name{leaf(argv[index])};
    if (parsed.has_value()) {
      ++understood;
      std::printf("  read     %-38s %zu window(s)\n", name.c_str(),
                  parsed->windows.size());
    } else {
      ++refused;
      std::printf("  refused  %-38s %s: %s\n", name.c_str(),
                  parsed.error().where.c_str(), parsed.error().reason.c_str());
    }
  }
  std::printf("\n%d read, %d refused\n", understood, refused);
  // Refusals are the report. A corpus this parser has not learned yet is not
  // a broken build.
  return 0;
}
