#pragma once

// GoogleTest skips for behaviour that belongs to tmux rather than to this
// library, so a release that cannot provide a capability is reported as
// skipped rather than failing or going untested.
//
// `LIBTMUX_REQUIRES_TMUX` is for a capability present from some release
// onwards, and `LIBTMUX_REQUIRES_TMUX_CANDIDATE` for one a release candidate
// already has. `LIBTMUX_SKIP_TMUX_DEFECT` is for a defect fixed in a later
// release, where a minimum would wrongly excuse everything above it — tmux 3.4
// escapes a round-tripped option that 3.3a and 3.5 both get right.
//
// The only header here that names a test framework. Suites using another one
// take `tmux_version.hpp` and write their own skip.

#include "libtmux/testing/tmux_version.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace libtmux::test {

/// Whether `running` ranks below `major.minor`, or below its candidate number
/// `candidate` when that is not 0. A `next-` build and a release candidate
/// rank below the release they lead to.
[[nodiscard]] constexpr bool lacks_tmux(const Version& running, std::uint32_t major,
                                        std::uint32_t minor,
                                        std::uint16_t candidate = 0) noexcept {
  return running < Version{.major = major, .minor = minor, .candidate = candidate};
}

} // namespace libtmux::test

/// Skip unless tmux is at least the release `major.minor`, naming what is
/// missing below it. A release candidate or `next-` build of `major.minor`
/// ranks below it and is skipped; use `LIBTMUX_REQUIRES_TMUX_CANDIDATE` for a
/// capability that candidates already have.
#define LIBTMUX_REQUIRES_TMUX(major_version, minor_version, capability)                \
  do {                                                                                 \
    if (::libtmux::test::lacks_tmux(::libtmux::test::running_tmux(), (major_version),  \
                                    (minor_version))) {                                \
      GTEST_SKIP() << (capability) << " arrived in tmux " << (major_version) << '.'    \
                   << (minor_version) << "; running "                                  \
                   << ::libtmux::test::describe_running_tmux();                        \
    }                                                                                  \
  } while (false)

/// Skip unless tmux is at least `major.minor-rc<candidate>`, the earliest build
/// measured to have the capability. Earlier candidates and `next-` builds are
/// skipped, and every later candidate and the release run.
#define LIBTMUX_REQUIRES_TMUX_CANDIDATE(major_version, minor_version,                  \
                                        candidate_number, capability)                  \
  do {                                                                                 \
    if (::libtmux::test::lacks_tmux(::libtmux::test::running_tmux(), (major_version),  \
                                    (minor_version), (candidate_number))) {            \
      GTEST_SKIP() << (capability) << " arrived in tmux " << (major_version) << '.'    \
                   << (minor_version) << "-rc" << (candidate_number) << "; running "   \
                   << ::libtmux::test::describe_running_tmux();                        \
    }                                                                                  \
  } while (false)

/// Skip while tmux is inside a closed range of releases with a known defect.
/// Both ends are inclusive, and `last` is the newest release still affected —
/// so a fix in 3.6 is written as a window ending at 3.5.
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
