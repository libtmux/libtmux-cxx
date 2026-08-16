# Library review

Two passes: an internal one, then an independent adversarial review that read
only the code. The independent pass found defects the internal one missed,
including a build configuration the internal pass had claimed worked.

An adversarial pass over the public headers and their implementation, looking
for lifetime hazards, needless machinery, and places where a caller can be
wrong without being told.

## Fixed

**A snapshot could be reparsed underneath its entities.** `Snapshot::parse`
was public and callable at any time. Entities are views into `rows_`, so a
second call cleared and rebuilt the vector every existing entity pointed into,
turning a live `Pane` into a dangling read with no diagnostic. Parsing now
happens once, inside a factory that returns `nullptr` on a malformed row; the
constructor and `parse` are private, so the second call is unspellable rather
than merely discouraged.

## Considered and left alone

**`Snapshot` is neither copyable nor movable.** That looks restrictive until
you notice the rows are `string_view`s into the owned `output_` string. Moving
the snapshot moves that string, and every row view keeps pointing at the
moved-from buffer. `EntityList` holds it by `unique_ptr` for exactly this
reason, so the address is stable and the list itself moves freely.

**`index_of` returns `fields_.size()` for an unknown field** rather than an
optional. It is the end-index convention the standard containers use, and the
only caller compares against `size()`. An optional here would be ceremony.

**Entity field tables are duplicated per entity.** `Pane`, `Session`,
`Window`, and `Client` each declare their own `kFields`. Factoring them into a
shared list would couple four unrelated tmux vocabularies together to save
twelve lines, and every future field would have to be filed against the right
entity anyway.

**`unexpected` is a function, not an alias.** An alias template cannot deduce
its argument, so the alias form breaks every call site. The asymmetry with
`expected` is deliberate and documented where it is defined.

## Fixed after the independent review

**Every ordinary tmux failure carried an empty diagnostic.** `Server::run`
built its message from stdout, and tmux writes why it refused to stderr. So
`kill-session -t nothing` returned `dispatched=true, exit_code=1,
diagnostic=""` — the field the type exists to carry was blank in exactly the
case it is for. No test asserted on diagnostic content, which is how it
survived. Now bound by a test that requires the refused name to appear.

**The C++20 leg did not compile.** `control.hpp` named `std::expected`
directly, bypassing the alias that exists precisely so the C++20 build can
substitute `tl::expected`. The C++20 build had been verified before that header
was added and not re-verified after, so the regression shipped behind a claim
that the configuration worked. Both legs are now built after every change.

**`is_function_key` overflowed a signed accumulator.** `"F99999999999"` reached
`value * 10 + digit` past `INT_MAX` before the range check ran — undefined
behaviour on a public entry point documented as validating. Names longer than
two digits are now rejected before accumulating, into an unsigned counter.

**A dead transport layer shipped in every build.** `transport.cpp` and its
header defined a parallel request/validation/argv layer referenced only by
itself; `server.cpp` goes through the process kernel. Removed.

**`panes_of` duplicated `entities_of<Pane>` verbatim.** Two public functions
with different names and identical bodies invite drift. `pane.hpp` now uses the
generic one.

## Open

**Cardinality could dangle on a temporary pipeline.** Fixed by taking an
lvalue. `first(...)` and `exactly_one(...)` return a reference into the range
they were given, so an owning temporary left that reference dangling at the
semicolon. `borrowed_range` was the wrong tool: `filter_view` is not borrowed,
so it would have rejected the safe named-list case too.

Requiring an lvalue costs one named variable per call and rejects the whole
class at compile time, including the safe uses. That trade was taken
deliberately: the storage a caller must keep alive is now visible in their own
code. A compile-fail check pins it.

**The C++20 package could not be installed.** Fixed. `tl::expected` was linked
`PUBLIC`, so `install(EXPORT)` emitted a target the consumer could not resolve.
Moving it to `PRIVATE` was not enough: a static library still exports its
private dependencies as `$<LINK_ONLY:...>`. Since the substitute is header-only
the link contributes nothing, so it is not linked at all — only its include
directory propagates, and its header installs beside ours. Verified by
installing the C++20 build and consuming it from an outside project.


**Relation nodes carry an opaque child.** ~~A relation crosses entity types, so
its child expression cannot live in `FilterExpr<Entity>`'s variant. The node
keeps the relation name and quantifier, and evaluation by value, but the child
does not lower. That is fine for in-memory filtering and blocks a relation from
ever compiling to a tmux `-f` expression.~~

Superseded. The child cannot live in the parent's variant, but its *lowered*
form can, and does: a relation lowers its child when it is built and replays it
between `begin_relation` and `end_relation`. An emitted document shows it —

```json
{"version": 1, "nodes": [
  {"kind": "begin_relation", "name": "panes", "quantifier": 0},
  {"kind": "bool_test", "name": "pane_active", "expected": true},
  {"kind": "end_relation"}]}
```

— and `apps/mcp/tests/filter_json_test.cpp` writes exactly that for
`tools/schema` to validate. Nothing about a relation blocks a `-f` compiler.

**Nothing verifies that a view outlives nothing.** ~~The lifetime rule — views
never outlive snapshot storage — is enforced by ownership and stated in
comments, not by a test that would fail if it were violated. A sanitizer run
over a deliberately dangling use would close that.~~

Partly superseded, and by something better than a sanitizer run: the cases
that could dangle are refused at compile time, and `tests/compile/` proves the
refusals stay refusals — `cardinality_refuses_a_temporary`,
`cardinality_refuses_a_produced_element`, `capture_lines_refuses_a_temporary`
and `relation_join_refuses_a_temporary`. A test that has to run to find a
dangle is a test that can miss it; one that fails to build cannot.

Still open for what the type system does not reach: an entity outliving the
`shared_ptr` chain is impossible by construction, but a caller who copies a
`string_view` out of one and keeps it is not stopped by anything.
