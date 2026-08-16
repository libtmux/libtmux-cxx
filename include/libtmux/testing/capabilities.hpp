#pragma once

// Behaviour that belongs to tmux rather than to this library, as GoogleTest
// skips.
//
// On a release that cannot provide a capability the honest outcome is a skip
// that names the release and the reason — not a failure that reads like a bug
// here, and not a silent deletion that would stop covering the versions that
// do provide it.
//
// Two shapes, and the difference matters. `LIBTMUX_REQUIRES_TMUX` is for a
// capability that arrived in some release and has been there since. Some
// defects are not that shape: tmux 3.4 answered a round-tripped option with an
// escape that 3.3a and 3.5 both get right. A minimum would wrongly excuse
// every release above it, so those get a closed window instead.
//
// This header is the only part of `libtmux::testing` that knows what a test
// framework is, which is why it is a separate target: link
// `libtmux::testing_gtest` to get it, or use `tmux_version.hpp` directly and
// write the skip in whatever framework you actually use.

#include "libtmux/testing/tmux_version.hpp"

#include <gtest/gtest.h>

// Skip unless tmux is at least `major.minor`, naming what is missing below it.
#define LIBTMUX_REQUIRES_TMUX(major_version, minor_version, capability)                \
  do {                                                                                 \
    const ::libtmux::Version required{.major = (major_version),                        \
                                      .minor = (minor_version)};                       \
    if (::libtmux::test::running_tmux() < required) {                                  \
      GTEST_SKIP() << (capability) << " arrived in tmux " << (major_version) << '.'    \
                   << (minor_version) << "; running "                                  \
                   << ::libtmux::test::describe_running_tmux();                        \
    }                                                                                  \
  } while (false)

// Skip while tmux is inside a closed range of releases with a known defect.
// Both ends are inclusive, and `last` is the newest release still affected —
// so a fix in 3.6 is written as a window ending at 3.5.
#define LIBTMUX_SKIP_TMUX_DEFECT(first_major, first_minor, last_major, last_minor,     \
                                 description)                                          \
  do {                                                                                 \
    const ::libtmux::Version first_affected{.major = (first_major),                    \
                                            .minor = (first_minor)};                   \
    const ::libtmux::Version after_affected{.major = (last_major),                     \
                                            .minor = (last_minor) + 1U};               \
    const auto& running = ::libtmux::test::running_tmux();                             \
    if (running >= first_affected && running < after_affected) {                       \
      GTEST_SKIP() << (description) << " (tmux " << (first_major) << '.'               \
                   << (first_minor) << " through " << (last_major) << '.'              \
                   << (last_minor) << "; running "                                     \
                   << ::libtmux::test::describe_running_tmux() << ')';                 \
    }                                                                                  \
  } while (false)
