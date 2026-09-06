#include "mcp_swap.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  try {
    auto runtime = libtmux::mcp_swap::system_runtime();
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    return libtmux::mcp_swap::execute(arguments, runtime, std::cout, std::cerr);
  } catch (const std::exception& failure) {
    std::cerr << "mcp-swap: " << failure.what() << '\n';
    return 1;
  }
}
