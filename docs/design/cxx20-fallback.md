# C++20 fallback

The package targets C++23. `LIBTMUX_CXX_STANDARD` selects the language level
and accepts only `20` or `23`; `cmake/ProjectOptions.cmake` rejects anything
else at configure time rather than letting a typo silently pick a default.

## Why a fallback exists at all

One header decision drives it. The core reports recoverable failure through
`std::expected`, which arrived in C++23 and is the only C++23 library facility
the public headers need. Toolchains that ship a C++20 standard library — the
common case on distributions still carrying GCC 12 or libstdc++ 12 — cannot
provide it. Rather than fork the API, the C++20 build substitutes the reference
implementation the standard type was modelled on.

## What the fallback substitutes

`tl::expected` 1.1.0, pinned. It is API-compatible with `std::expected` for
everything the core surface uses: construction, `has_value`, `value`, `error`,
`operator*`, `operator->`, and `unexpected`. The core does not use monadic
operations (`and_then`, `transform`, `or_else`), so the differences between the
two in that area never reach a caller.

The substitution is a type alias in one header. No public signature changes
shape between the two builds, and no caller writes `std::expected` or
`tl::expected` by name — they write the alias.

## Binary identity

The two builds are not ABI-compatible and must never be mixed. A C++20 build
and a C++23 build of the same version differ in the layout of every type that
carries a result, so linking objects from both produces a program that appears
to build and then reads the wrong bytes.

The package therefore gives the C++20 configuration a distinct inline ABI
namespace and a distinct binary identity, so a mismatch is a link error naming
the missing symbol rather than a runtime corruption. Every public header uses
the configured inline ABI namespace from its first committed version; no later
task retrofits ABI selection onto declarations that shipped without it.

## Dependency posture

`tl::expected` is a compatibility source, not a dependency. The C++23 build —
the default, and the only one the project tests as primary — links nothing
outside the standard library. Consumers who do not opt into `LIBTMUX_CXX_STANDARD=20`
never see the pinned source, and the C++23 package config declares no
dependency on it.

## How the substitution is spelled

`libtmux/expected.hpp` selects the implementation and the rest of the package
uses `libtmux::expected` and `libtmux::unexpected`. Neither underlying type is
named anywhere else, in public headers or in the private transport.

`unexpected` is a function rather than an alias template. An alias template
cannot deduce its argument, so spelling it as one would break every
`unexpected(error)` call site; a separate `unexpected_t<E>` alias exists for
the rare declaration that names the type.

## Current state

Both legs build. The C++23 build is the default and links nothing outside the
standard library. `LIBTMUX_CXX_STANDARD=20` fetches the pinned source, defines
`LIBTMUX_USE_TL_EXPECTED`, and produces the same public surface.

Every public header opens `libtmux::v1_cxx23` or `libtmux::v1_cxx20` as an
inline namespace, so mixing objects from the two builds is a link error whose
missing symbol names the ABI it expected. The namespace is inline, so callers
still write `libtmux::Server` and never see it.
