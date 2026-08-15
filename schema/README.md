# Schema

One file: [`filter-expression-v1.schema.json`](filter-expression-v1.schema.json).

It describes the **lowered** form of a
[`FilterExpr`](../include/libtmux/filter_expr.hpp) — the shape a filter takes
once it has been turned from C++ types into data that another process could
evaluate.

## Why it is here and not in the library

The core ships **no serializer**. Lowering produces a
[`LoweredNode`](../include/libtmux/lowered_node.hpp) tree; turning that into
JSON is something an integration does, with whatever JSON library it already
has. The library having an opinion about that would mean the library having a
JSON dependency, and it has none.

This schema is the contract for anyone who does that work: it says what the
lowered form looks like, so two integrations that never met produce the same
document.

## Related

- [`include/libtmux/lowering.hpp`](../include/libtmux/lowering.hpp) — producing the tree
- [`include/libtmux/filter_expr.hpp`](../include/libtmux/filter_expr.hpp) — the expressions being lowered
- [The library](../README.md#query-with-typed-filters) — what a filter is for
