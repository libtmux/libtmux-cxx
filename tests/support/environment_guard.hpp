#pragma once

// Set a variable for the length of a scope, and put back exactly what was
// there.
//
// A socket name is not a location: tmux resolves it under `$TMUX_TMPDIR`, so a
// test that starts a server under one directory and then asks the library for
// the same name has to say which directory it means. Exporting it is what a
// caller of `tmux -L` does, and this is that, scoped.
//
// Process-global, so it is a fixture-setup tool and not something to hold
// while another thread is spawning: `setenv` racing `getenv` is undefined
// however careful the caller is.

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace libtmux::test {

class EnvironmentGuard final {
public:
  EnvironmentGuard(std::string name, std::string_view value) : name_{std::move(name)} {
    if (const char* const existing = std::getenv(name_.c_str()); existing != nullptr) {
      previous_ = existing;
    }
    ::setenv(name_.c_str(), std::string{value}.c_str(), 1);
  }

  ~EnvironmentGuard() {
    if (previous_.has_value()) {
      ::setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

  EnvironmentGuard(const EnvironmentGuard&) = delete;
  EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;
  EnvironmentGuard(EnvironmentGuard&&) = delete;
  EnvironmentGuard& operator=(EnvironmentGuard&&) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

} // namespace libtmux::test
