#include "workspace_cli.hpp"

#include <csignal>
#include <iostream>

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  return libtmux::workspace::cli::run({argv + 1, argv + argc}, std::cin, std::cout,
                                      std::cerr);
}
