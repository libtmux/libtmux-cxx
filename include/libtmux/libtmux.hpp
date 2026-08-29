#pragma once

// libtmux: a typed C++ interface to tmux.
//
// This umbrella header pulls in the dependency-free core: value types for the
// things tmux prints, snapshots that own what a command returned, entities
// projected from those snapshots, and expressions for selecting among them.
// Nothing here spawns a process; execution belongs to the connection type.

#include "libtmux/abi.hpp"
#include "libtmux/batch.hpp"
#include "libtmux/capabilities.hpp"
#include "libtmux/capture.hpp"
#include "libtmux/cardinality.hpp"
#include "libtmux/chain.hpp"
#include "libtmux/command.hpp"
#include "libtmux/control.hpp"
#include "libtmux/delivery.hpp"
#include "libtmux/entities.hpp"
#include "libtmux/expected.hpp"
#include "libtmux/filter_expr.hpp"
#include "libtmux/format.hpp"
#include "libtmux/keys.hpp"
#include "libtmux/legacy_lookup.hpp"
#include "libtmux/lowering.hpp"
#include "libtmux/options.hpp"
#include "libtmux/relations.hpp"
#include "libtmux/server.hpp"
#include "libtmux/snapshot.hpp"
#include "libtmux/socket.hpp"
#include "libtmux/target.hpp"
#include "libtmux/version.hpp"
