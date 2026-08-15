#include "libtmux/version.hpp"

LIBTMUX_NAMESPACE_BEGIN

// Defined out of line so the package ships a compiled artifact rather than
// headers alone: consumers link one target and get one ABI to match.
std::string_view library_version() noexcept { return LIBTMUX_VERSION; }

LIBTMUX_NAMESPACE_END
