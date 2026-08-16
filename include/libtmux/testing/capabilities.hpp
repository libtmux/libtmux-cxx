#pragma once

// GoogleTest skips for behaviour that belongs to tmux rather than to this
// library, so a release that cannot provide a capability is reported as
// skipped rather than failing or going untested.
//
// `LIBTMUX_REQUIRES_TMUX` is for a capability present from some release
// onwards. `LIBTMUX_SKIP_TMUX_DEFECT` is for a defect fixed in a later
// release, where a minimum would wrongly excuse everything above it — tmux 3.4
// escapes a round-tripped option that 3.3a and 3.5 both get right.
//
// The only header here that names a test framework. Suites using another one
// take `tmux_version.hpp` and write their own skip.

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
    const ::libtmux::Version running = ::libtmux::test::running_tmux();                \
    if (running >= first_affected && running < after_affected) {                       \
      GTEST_SKIP() << (description) << " (tmux " << (first_major) << '.'               \
                   << (first_minor) << " through " << (last_major) << '.'              \
                   << (last_minor) << "; running "                                     \
                   << ::libtmux::test::describe_running_tmux() << ')';                 \
    }                                                                                  \
  } while (false)
