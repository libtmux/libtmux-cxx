# libtmux C++ program implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a dependency-free C++23 `libtmux` with practical Python API
parity, evidence-led transport and query bakeoffs, a clean production rewrite,
and verified downstream packaging.

**Architecture:** Contract artifacts, test infrastructure, and isolated spike
implementations are built first. Measured transport and query decisions are
then frozen, every spike implementation is deleted, and one opaque synchronous
library is implemented against retained acceptance tests and parity manifests.

**Tech Stack:** C++23 and C++20, CMake 3.28.3, CMakePresets, Ninja 1.11.1, GCC
13 with libstdc++ 13, Clang/clang-format/clang-tidy 18.1.3 with libc++ 18.1,
GoogleTest 1.17.0, Python 3.10+ parity tooling, real tmux, ASan, UBSan, TSan,
vcpkg, and `tl::expected` 1.1.0.

## Global Constraints

- The approved design in
  [`cxx/docs/design/bakeoff-and-rewrite.md`](../design/bakeoff-and-rewrite.md)
  is the source of truth.
- Linux is the only blocking platform until equivalent macOS or WSL evidence
  exists.
- The production surface is one compiled target and alias,
  `libtmux::libtmux`.
- C++23 production code has no third-party dependency.
- C++20 uses only the pinned `tl::expected` 1.1.0 compatibility source and a
  distinct ABI namespace and binary identity.
- Every public header uses the configured inline ABI namespace from its first
  committed version; no later package task rewrites public declarations to add
  ABI selection.
- `cxx/VERSION` is the sole C++ package-version source. CMake exports and the
  vcpkg overlay must reproduce it exactly.
- Public entity types are opaque, non-templated, copyable values. Transport
  policy never enters their type.
- The synchronous core executes argv without a shell. Async and control-mode
  APIs remain additive.
- List-shaped accessors are lenient and return an empty snapshot on every tmux
  acquisition failure; checked variants retain the typed failure.
- Snapshot iteration, views, predicates, and cardinality perform no tmux I/O.
- Reference cardinality requires a stable forward range. General input ranges
  use the owning cardinality overloads approved in the design.
- A relation-enrichment failure cannot expose a partial graph.
- A mutation followed by failed hydration reports `MutationApplied` and must
  not be retried as an unapplied request.
- Stable tmux cells are `3.2a`, `3.3a`, `3.4`, `3.5`, `3.6`, exact `3.7`,
  `3.7a`, and `3.7b`. The resolved `master` commit is informational.
- Spike implementations live only under `cxx/spikes/`, are never installed,
  and are deleted before any production source is written.
- Generated manifests and headers must reproduce byte-for-byte from recorded
  source identities.
- No task may weaken the full Python test, lint, type, or documentation gates.
- Do not set `HOME`, create tags, push tags, or push commits.

## Program Order

```text
contract and harness
        |
transport bakeoff
        |
query bakeoff
        |
architecture freeze and spike deletion
        |
clean production rewrite
        |
full parity closure
        |
packaging, matrix, and final audits
```

Each phase has its own plan and independently reviewable exit evidence. A phase
does not start until the preceding plan's final gate and adversarial review are
committed.

## Plan Suite

- [`01-contract-and-harness.md`](01-contract-and-harness.md) establishes the
  build, parity manifests, real-tmux fixture, and differential framework.
- [`02-transport-bakeoff.md`](02-transport-bakeoff.md) compares private backend
  ownership, process behavior, concurrency, control framing, and engine-ops
  compatibility.
- [`03-query-bakeoff.md`](03-query-bakeoff.md) closes lookup semantics, compares
  AST storage, validates relations and ranges, and freezes the shared schema.
- [`04-clean-rewrite.md`](04-clean-rewrite.md) freezes contracts, deletes every
  spike implementation, and writes the production core from an empty source
  tree.
- [`05-python-parity.md`](05-python-parity.md) closes every generated API and
  behavioral parity shard, including docs and executable examples.
- [`06-distribution-and-audit.md`](06-distribution-and-audit.md) proves package
  consumption, the tmux matrix, reproducibility, CI, and final reviews.

## Common Task Gate

Run CMake and CTest commands with `cxx/` as the working directory, where
`CMakePresets.json` lives. Run Git, Python, Ruff, mypy, pytest, and documentation
commands from the repository root. An executing agent must set the tool's
working directory directly; do not add a persistent shell `cd` or change
`HOME`.

Run the task-specific failing test before implementation and confirm that it
fails for the stated reason. After the minimal implementation passes its
targeted test, run the affected plan's prescribed CMake preset and CTest label.
Every focused C++ red uses one of two explicit sequences:

- A compile/link red adds the test source and named CMake target, reruns the
  configure preset, and requires that target's build to fail for the stated
  missing declaration or definition. Do not invoke CTest for an unbuilt target.
- A runtime-behavior red adds a deliberately buildable implementation shell and
  registered test, reruns the configure preset, requires the named target to
  build successfully, and then requires CTest with `--no-tests=error` to fail
  on the stated assertion.

Each task names its sequence, target, and expected diagnostic. After
implementation, reconfigure, rebuild the same target, and run its strict CTest
selection; never treat a stale build tree or an empty selection as evidence.

This applies independently to every build tree. Before invoking CTest for a
preset, configure that preset and build its affected target. A `cxx-dev` build
never prepares `cxx-sanitize` or `cxx-tsan`.

Evidence-producing CTest runs use the harness from Plan 01. It records a fresh
`ctest --show-only=json-v1` registration snapshot, a uniquely named JUnit
result, their digests, the selected preset, compiler, source identity, and
build identity. A mutable `Testing/` directory or prior `LastTest.log` is not
accepted as completion evidence.

Before each task commit, run the staged-scope check:

```console
$ git diff --cached --check
```

Inspect the exact staged paths:

```console
$ git diff --cached --name-only
```

Every phase exit runs the C++ gate:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev
```

```console
$ ctest --preset cxx-dev --no-tests=error --output-on-failure
```

Every phase exit also runs the Python gate without changing `HOME`:

```console
$ uv run ruff format .
```

```console
$ env -u __MISE_ZSH_ACTIVATE_PATH -u __MISE_ORIG_PATH uv run pytest
```

```console
$ uv run ruff check . --fix --show-fixes
```

```console
$ uv run mypy .
```

```console
$ uv run pytest --doctest-modules cxx/tools cxx/tests/differential
```

```console
$ just build-docs
```

```console
$ env -u __MISE_ZSH_ACTIVATE_PATH -u __MISE_ORIG_PATH uv run pytest
```

## Task 1: Establish the contract and harness

**Files:**

- Execute: `cxx/docs/plans/01-contract-and-harness.md`
- Produce: `cxx/parity/`, `cxx/tests/support/`, `cxx/tools/differential/`
- Produce: `cxx/docs/evidence/contract-and-harness.json`
- Produce: `cxx/docs/evidence/contract-and-harness-review.md`

**Interfaces:**

- Consumes: approved design commit and Python source objects.
- Produces: reproducible parity observations, manifest validation, an isolated
  real-tmux server fixture, and canonical differential records.

- [ ] **Step 1: Execute every task in the phase plan in order**

Use the phase plan's failing-test, implementation, verification, and commit
steps without combining commits.

- [ ] **Step 2: Run the common phase gate**

Expected: every C++ and Python command in `Common Task Gate` exits zero.

- [ ] **Step 3: Verify the phase evidence is complete**

```console
$ uv run python -m cxx.tools.parity \
    verify \
    --manifest cxx/parity/manifest.json \
    --mode structural \
    --allow-pending
```

Expected: source provenance, all observed entries, and only explicitly pending
C++ mappings are valid.

```console
$ ctest --preset cxx-dev -L real-tmux --no-tests=error --output-on-failure
```

Expected: both `-L` and `-S` fixture modes pass without leaked tmux processes
or socket paths.

## Task 2: Select the transport design

**Files:**

- Execute: `cxx/docs/plans/02-transport-bakeoff.md`
- Produce: `cxx/docs/bakeoffs/transport/`

**Interfaces:**

- Consumes: `ScopedTmuxServer`, canonical command records, and the common
  CMake/GoogleTest harness.
- Produces: one accepted private backend ownership mechanism and documented
  grafts, with no public transport hierarchy.

- [ ] **Step 1: Execute every task in the phase plan in order**

- [ ] **Step 2: Run the complete transport gate**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize
```

```console
$ ctest --preset cxx-sanitize -L transport --no-tests=error --output-on-failure
```

Expected: all three contenders pass the semantic and sanitizer gates.

- [ ] **Step 3: Require an independent architecture disposition**

Expected: `cxx/docs/bakeoffs/transport/review.md` contains no unresolved
finding and `decision.json` identifies one winner plus any measured grafts.

## Task 3: Select the query design

**Files:**

- Execute: `cxx/docs/plans/03-query-bakeoff.md`
- Produce: `cxx/docs/bakeoffs/query/`
- Produce: `cxx/schema/filter-expression-v1.schema.json`

**Interfaces:**

- Consumes: accepted transport seam, Python lookup oracle, and generated field
  observations.
- Produces: one AST storage design, approved lookup semantics, relation
  ownership, range/cardinality contracts, and schema goldens.

- [ ] **Step 1: Execute every task in the phase plan in order**

- [ ] **Step 2: Stop on an unapproved semantic delta**

Expected: any regex, Unicode, membership, or missing-value divergence is
recorded as pending and presented for explicit approval before selection.

- [ ] **Step 3: Run the complete query gate**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize
```

```console
$ ctest --preset cxx-sanitize -L query --no-tests=error --output-on-failure
```

Expected: all three contenders pass runtime, compile-fail, lifetime, schema,
and zero-I/O tests.

## Task 4: Perform the clean production rewrite

**Files:**

- Execute: `cxx/docs/plans/04-clean-rewrite.md`
- Delete: `cxx/spikes/`
- Produce: `cxx/include/libtmux/`, `cxx/src/`

**Interfaces:**

- Consumes: frozen transport/query decisions and retained acceptance tests.
- Produces: the dependency-free production target through command execution,
  connection state, errors, snapshots, and the selected query core.

- [ ] **Step 1: Execute Task 1 of the clean-rewrite plan**

Freeze, review, and obtain explicit acceptance of the selected architecture.

- [ ] **Step 2: Execute Tasks 2 and 3 of the clean-rewrite plan**

Freeze the retained acceptance contract, then delete and audit the spike
implementations exactly once.

- [ ] **Step 3: Execute Tasks 4 through 14 of the clean-rewrite plan**

Write production metadata and sources only after the Task 3 boundary evidence
is committed.

- [ ] **Step 4: Run the common phase gate from a fresh build directory**

Expected: the production target passes without including or linking any spike
artifact.

## Task 5: Close Python feature parity

**Files:**

- Execute: `cxx/docs/plans/05-python-parity.md`
- Modify: `cxx/parity/mapping.json`
- Produce: public headers, sources, tests, docs, and examples named by each
  parity shard.

**Interfaces:**

- Consumes: production core and generated parity shards.
- Produces: zero pending required entries and passing differential scenarios
  for the explicitly selected local tmux binary. The next plan proves the full
  stable range.

- [ ] **Step 1: Execute every parity shard in dependency order**

- [ ] **Step 2: Reject missing API, behavior, docs, or examples**

```console
$ uv run python -m cxx.tools.parity \
    verify \
    --manifest cxx/parity/manifest.json \
    --mode complete
```

Expected: no pending, unimplemented, or unapproved entry.

- [ ] **Step 3: Run the common phase gate**

Expected: every public example is an executed CTest and every manifest row has
its unique documentation and example mapping. Header self-containment is the
first distribution gate.

## Task 6: Prove distribution and completion

**Files:**

- Execute: `cxx/docs/plans/06-distribution-and-audit.md`
- Produce: package config, vcpkg overlay, CI workflows, audit reports, and the
  completion ledger.

**Interfaces:**

- Consumes: parity-complete production library.
- Produces: relocatable static/shared C++23 and C++20 packages, stable tmux
  matrix evidence, reproducible artifacts, and closed independent reviews.

- [ ] **Step 1: Execute every task in the phase plan in order**

- [ ] **Step 2: Revalidate the committed completion evidence**

```console
$ uv run python cxx/tools/audit/completion.py \
    --design cxx/docs/design/bakeoff-and-rewrite.md \
    --manifest cxx/parity/manifest.json \
    --owned-paths cxx/tools/audit/completion-owned-paths.json \
    --run-record cxx/build/complete-run.json \
    --output cxx/docs/evidence/completion.json \
    --check \
    --require-clean
```

Expected: the Task 14 live run covering isolated build, test, ASan/UBSan, TSan,
package, install, relocation, FetchContent, vcpkg, dependency, documentation,
and reproducibility remains valid after its byte-identical baseline files are
committed, and the checkout is clean.

- [ ] **Step 3: Run the final Python gate**

Run every command under `Common Task Gate`; the completion auditor records
normalized current results in `cxx/docs/evidence/completion.json`.

- [ ] **Step 4: Close both independent reviews**

Expected: architecture and line-review reports contain no unresolved finding,
and the worktree is clean after two fresh out-of-tree builds.
