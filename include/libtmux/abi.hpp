#pragma once

// Binary identity.
//
// The C++20 and C++23 builds are not ABI-compatible: every type that carries a
// result differs in layout because the underlying expected differs. Linking
// objects from both produces a program that appears to build and then reads the
// wrong bytes.
//
// An inline namespace whose name encodes the choice makes that a link error
// naming the missing symbol instead. Callers still write `libtmux::Server`; the
// namespace is inline, so it is invisible in source and decisive in the mangled
// name.

#include "libtmux/expected.hpp"

#if defined(LIBTMUX_USE_TL_EXPECTED)
#define LIBTMUX_ABI_NAMESPACE v1_cxx20
#else
#define LIBTMUX_ABI_NAMESPACE v1_cxx23
#endif

#define LIBTMUX_NAMESPACE_BEGIN                                                        \
  namespace libtmux {                                                                  \
  inline namespace LIBTMUX_ABI_NAMESPACE {

#define LIBTMUX_NAMESPACE_END                                                          \
  }                                                                                    \
  }
