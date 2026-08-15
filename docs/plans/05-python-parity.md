# Python parity implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close every required release and development parity-manifest entry
with a C++ symbol, compile probe, real-tmux behavior test, documentation ID,
executed-example ID, error contract, and applicable version evidence.

**Architecture:** Generated, dependency-ordered parity shards are the finite
work queue. Each slice adds one behavior family to the opaque value entities,
updates only that shard's reviewed classifications, and extends canonical
Python/C++ scenarios without weakening the dependency-free core.

**Tech Stack:** C++23, production `libtmux::libtmux`, GoogleTest, real tmux,
compile contracts, CTest examples, Python parity/differential tooling, and the
tmux capability model.

## Parity Slice Protocol

Every parity behavior task (Tasks 2 and 4 through 23) follows this exact
sequence. Metadata, prerequisite value shells, and final closure use their
explicit task-local gates.

1. Run its `verify --shard` command and observe a nonzero exit listing the
   remaining entry IDs.
2. Add the public declarations, positive and negative compile probes, failing
   real-tmux tests, canonical differential scenario, API prose, executed example
   cases, and a buildable behavior stub named by those entries. Do not add the
   behavior implementation yet.
3. Register the shard source, exact target, and CTest label from the execution
   registry below before implementation. Every behavior shard uses the same
   runtime-behavior red; no task chooses its red mode. Run `run-shard --phase
red`, which reconfigures the named preset, builds the exact target, selects
   exactly the row's red case, and requires its stored diagnostic. A missing
   target, build failure, empty or extra selection, passing stub, wrong
   diagnostic, stale tree, or unrelated failure is not a red result. Implement
   only the named behavior family, then run `run-shard --phase green` to repeat
   the configure, exact build, and strict full-label CTest gate.
4. Change only the task's mapping rows from `pending` to `implemented`,
   `adapted`, or `excluded`. Never regenerate over reviewed classifications.
5. Synchronize reviewed mapping rows and the bound approval and evidence
   sidecars into `manifest.json`:

   ```console
   $ uv run python -m tools.parity \
       sync \
       --release tools/parity/data/release-v0.62.0.json \
       --development tools/parity/data/development.json \
       --mapping tools/parity/data/mapping.json \
       --approvals tools/parity/data/approvals.json \
       --evidence tools/parity/data/evidence.json \
       --output tools/parity/data/manifest.json
   ```

6. Rerun `run-shard --phase green` after synchronization so its immutable CTest
   record binds the current mapping and manifest source, then run the row's
   differential driver. Differential records bind the manifest's
   `semantic_contract_sha256`, which excludes execution evidence, rather than
   the full manifest digest. The raw inventory and JUnit stay in the gate's
   content-addressed ignored leaf.
7. Run `record-evidence` with the task's exact shard name, immutable CTest gate
   record, differential result, and selected tmux binary. Require it to verify
   the current source, registration, executable, result, scenario, adapter, and
   binary digests and update only that shard's records in `evidence.json`;
   mutable CTest `Testing/` state is never evidence. Rerun the identical `sync`
   command from step 5 so `manifest.json` embeds the refreshed evidence
   sidecar before verification.
8. Run the metadata generator and public-umbrella generator in `--check` mode,
   then run structural parity verification; other shards may remain pending.
9. Run the full C++ and Python task gate from `00-program.md`, generate the
   shard's literal staging pathspec, then make the named atomic commit.

When one task contains multiple named commits, repeat steps 4 through 9 for
each commit; no sibling shard's mapping, differential result, or gate may be
deferred to the final commit.

The compact task bodies below do not repeat the wrapper commands. Immediately
after each behavior task's structural `verify --shard` red, execute protocol
Step 3 with that task's exact shard name before its implementation step.
Immediately before any abbreviated final CTest snippet, execute protocol Step 6
with the same name. The direct CTest snippet displays the selector that the
wrapper has just built and recorded; it does not replace the wrapper.

Every behavior slice treats these as shared production/test registries and
stages every path it changes in the same commit:

- `src/CMakeLists.txt`
- `cxx/CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/unit/CMakeLists.txt`
- `tests/integration/CMakeLists.txt`
- `tests/compile/CMakeLists.txt`
- `tests/differential/CMakeLists.txt`
- `examples/CMakeLists.txt`
- `tests/differential/scenario_registry.json`
- `tools/parity/data/shards.json`
- `tools/differential/python_reference.py`
- `tests/differential/cpp_adapter.cpp`
- the matching `tests/cxx/differential/test_*.py` driver

Change `scenario.schema.json` in that commit only when the registered request
or observation shape changes. After synchronization, every slice stages
`mapping.json`, `manifest.json`, and `evidence.json`; an adapted or excluded
row also stages `approvals.json`. Task-specific file lists below are additions
to these inherited paths. Before committing, porcelain status including all
untracked files may contain only the task-specific and inherited slice paths;
a generated but unstaged manifest is a task failure.

`stage-paths` derives a newline-delimited pathspec from the selected shard's
`owned_paths` plus the fixed registries above. The fixed set includes the
top-level, test-root, production, unit, integration, compile, differential, and
example CMake files; `shards.json`; `scenario_registry.json`; both adapters;
the exact driver path from the table; and the mapping, manifest, and evidence
files. It includes `scenario.schema.json` only when the registered wire shape
changes and `approvals.json` only for a changed adapted or excluded row. When
the shard adds or removes a public header, it also includes
`cxx/public-headers.json` and the generated
`include/libtmux/libtmux.hpp`. It rejects directories, globs, pathspec
magic, paths outside the allowed roots, a changed in-scope path absent from the
result, a path owned by another shard, or a pre-staged path outside the exact
result. After staging, the cached path list must equal the generated changed
path set. No behavior-slice staging command may use a directory or wildcard
pathspec.

Generate the exact pathspec for the shard being committed:

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard server-connection \
    --output cxx/build/parity-stage/server-connection.paths
```

Stage only that generated list:

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/server-connection.paths
```

Replace `server-connection` with the exact current row from the execution
registry. The generated pathspec is ignored build output and is never
committed.

For every adapted entry, record the precise semantic delta, oracle IDs, and
explicit approval. For every exclusion, record the Python-runtime
inapplicability proof and approval. A missing functional capability is not an
exclusion.

Every public function and method gets a case ID in the task's explicitly named
source under `examples/parity/`. Each executable dispatches named cases and
is registered with CTest. The mapping uses the exact case ID, not merely the
source filename.

Generated metadata under `tools/parity/data/metadata/` and
`include/libtmux/generated/` is immutable output. Parity slices bind those
files; they never edit them. If a generated value is wrong, stop the slice and
make a separate correction commit that changes `generate_metadata.py` and its
tests, regenerates every affected JSON/header output, and passes `--check` from
two temporary directories. Resume the parity slice only after that commit.

`cxx/public-headers.json` is the sorted authoritative registry of installed
public headers. Each row records `install_path`, `source_path`, `source_kind`
(`committed`, `generated`, or `configured`), owning shard, and its C++23/C++20
variant set. The configured `libtmux/config.hpp` row points to
`include/libtmux/config.hpp.in`; the template itself is not installed.
`generate_umbrella.py` derives `include/libtmux/libtmux.hpp` from it. Every
slice that adds or removes a public header updates the registry and generated
umbrella in the same commit; every slice runs the generator in `--check` mode.
Direct umbrella edits are forbidden.

## Shard Execution Registry

Each row fixes the CMake target, configure/build/test preset, CTest label, and
Python differential driver. `shards.json` stores these fields with `owned_paths`
plus `red_mode`, `red_case`, `red_diagnostic`, `gate_id`, and `gate_record`;
`shard.py --check` plus its tests require the exact row set below. It is the
machine-readable execution registry used by staging, execution, and coverage
tools. The target is registered before its red build. Metadata has
`red_mode=structural` and no differential driver because generated values and
version boundaries are closed by immutable CTest evidence.

Every behavior row has `red_mode=runtime`, red CTest name
`parity.<shard>.unimplemented`, diagnostic
`unimplemented parity shard: <shard>`, gate ID `parity-<shard>`, and record path
`cxx/build/evidence/parity-<shard>.json`, with `<shard>` replaced by the literal
first-column value. These values are stored, not inferred at execution time;
the generator rejects any row that differs from the closed formula.

| Shard                     | Target                                | Preset         | CTest label                      | Python driver                                            |
| ------------------------- | ------------------------------------- | -------------- | -------------------------------- | -------------------------------------------------------- |
| `metadata`                | `parity_metadata_test`                | `cxx-dev`      | `parity-metadata`                | Not applicable                                           |
| `server-connection`       | `parity_server_connection_test`       | `cxx-dev`      | `parity-server-connection`       | `tests/cxx/differential/test_server_connection.py`       |
| `server-collections`      | `parity_server_collections_test`      | `cxx-dev`      | `parity-server-collections`      | `tests/cxx/differential/test_server_collections.py`      |
| `server-sessions`         | `parity_server_sessions_test`         | `cxx-dev`      | `parity-server-sessions`         | `tests/cxx/differential/test_server_sessions.py`         |
| `server-shell-buffers`    | `parity_server_shell_buffers_test`    | `cxx-dev`      | `parity-server-shell-buffers`    | `tests/cxx/differential/test_server_shell_buffers.py`    |
| `server-commands`         | `parity_server_commands_test`         | `cxx-dev`      | `parity-server-commands`         | `tests/cxx/differential/test_server_commands.py`         |
| `session-values`          | `parity_session_values_test`          | `cxx-sanitize` | `parity-session-values`          | `tests/cxx/differential/test_session_values.py`          |
| `session-navigation`      | `parity_session_navigation_test`      | `cxx-dev`      | `parity-session-navigation`      | `tests/cxx/differential/test_session_navigation.py`      |
| `session-lifecycle`       | `parity_session_lifecycle_test`       | `cxx-dev`      | `parity-session-lifecycle`       | `tests/cxx/differential/test_session_lifecycle.py`       |
| `window-values`           | `parity_window_values_test`           | `cxx-sanitize` | `parity-window-values`           | `tests/cxx/differential/test_window_values.py`           |
| `window-layout`           | `parity_window_layout_test`           | `cxx-dev`      | `parity-window-layout`           | `tests/cxx/differential/test_window_layout.py`           |
| `window-panes`            | `parity_window_panes_test`            | `cxx-dev`      | `parity-window-panes`            | `tests/cxx/differential/test_window_panes.py`            |
| `window-lifecycle`        | `parity_window_lifecycle_test`        | `cxx-dev`      | `parity-window-lifecycle`        | `tests/cxx/differential/test_window_lifecycle.py`        |
| `pane-values`             | `parity_pane_values_test`             | `cxx-sanitize` | `parity-pane-values`             | `tests/cxx/differential/test_pane_values.py`             |
| `pane-io`                 | `parity_pane_io_test`                 | `cxx-dev`      | `parity-pane-io`                 | `tests/cxx/differential/test_pane_io.py`                 |
| `pane-layout`             | `parity_pane_layout_test`             | `cxx-dev`      | `parity-pane-layout`             | `tests/cxx/differential/test_pane_layout.py`             |
| `pane-modes`              | `parity_pane_modes_test`              | `cxx-dev`      | `parity-pane-modes`              | `tests/cxx/differential/test_pane_modes.py`              |
| `pane-topology`           | `parity_pane_topology_test`           | `cxx-dev`      | `parity-pane-topology`           | `tests/cxx/differential/test_pane_topology.py`           |
| `client`                  | `parity_client_test`                  | `cxx-dev`      | `parity-client`                  | `tests/cxx/differential/test_client.py`                  |
| `environment`             | `parity_environment_test`             | `cxx-dev`      | `parity-environment`             | `tests/cxx/differential/test_environment.py`             |
| `sparse-array`            | `parity_sparse_array_test`            | `cxx-dev`      | `parity-sparse-array`            | `tests/cxx/differential/test_sparse_array.py`            |
| `options`                 | `parity_options_test`                 | `cxx-dev`      | `parity-options`                 | `tests/cxx/differential/test_options.py`                 |
| `hooks`                   | `parity_hooks_test`                   | `cxx-dev`      | `parity-hooks`                   | `tests/cxx/differential/test_hooks.py`                   |
| `query-neo`               | `parity_query_neo_test`               | `cxx-sanitize` | `parity-query`                   | `tests/cxx/differential/test_query.py`                   |
| `common-version`          | `parity_common_version_test`          | `cxx-dev`      | `parity-common-version`          | `tests/cxx/differential/test_common_version.py`          |
| `warnings-errors`         | `parity_warnings_errors_test`         | `cxx-dev`      | `parity-warnings-errors`         | `tests/cxx/differential/test_warnings_errors.py`         |
| `compatibility-protocols` | `parity_compatibility_protocols_test` | `cxx-dev`      | `parity-compatibility-protocols` | `tests/cxx/differential/test_compatibility_protocols.py` |
| `testing-support`         | `parity_testing_support_test`         | `cxx-dev`      | `parity-testing-support`         | `tests/cxx/differential/test_testing_support.py`         |

`run-shard` accepts only `--shard` and `--phase red|green`. It reads the stored
row, invokes CMake and CTest as argv without a shell, and accepts no command,
target, selector, diagnostic, gate ID, or record-path override. The
`server-connection` red is exactly:

```console
$ uv run python -m tools.parity \
    run-shard \
    --shard server-connection \
    --phase red
```

Expected: `cxx-dev` configures, `parity_server_connection_test` builds, exactly
`parity.server-connection.unimplemented` fails with `unimplemented parity
shard: server-connection`, and the command exits zero only because that complete
red contract matched. After implementation, the exact green is:

```console
$ uv run python -m tools.parity \
    run-shard \
    --shard server-connection \
    --phase green
```

Expected: the same preset reconfigures, the same target rebuilds, every case
under anchored label `^parity-server-connection$` passes through
`ctest_gate.py`, and the immutable record is written at
`cxx/build/evidence/parity-server-connection.json` with gate ID
`parity-server-connection`. Every other behavior task changes only the literal
`--shard` value to its first-column registry name.

Creating a scenario includes creating its exact driver from this table. A
driver runs the Python and C++ adapters on separate fixture sockets and compares
only the scenario's declared canonicalization pointers.

## File Path Convention

Later compact file lists use this fixed expansion:

- `name.hpp` is `include/libtmux/name.hpp`; `name.cpp` is
  `src/name.cpp`.
- A shard `foo-bar` owns
  `tests/integration/foo_bar_test.cpp`,
  `tests/differential/scenarios/foo-bar.json`,
  `tests/cxx/differential/test_foo_bar.py`,
  `examples/parity/foo_bar.cpp`, and `docs/api/foo-bar.md`.
- Each task's compile suite uses its exact shard name directly under
  `tests/compile/`.
- `mapping`, `manifest`, `evidence`, and `approvals` mean the corresponding
  JSON files directly under `tools/parity/data/`.

These expansions and the inherited registry/adapter paths are part of each
task's file list and atomic commit even when its local list uses the shorter
name.

## Stable Entity Shape

All parity tasks preserve these rules:

- Entities are non-templated copyable values over shared opaque connection
  identity, validated IDs, immutable cached records, and complete graphs.
- Destruction never issues a tmux command.
- `refresh()` replaces only its receiver after complete successful capture.
- List accessors are lenient on every acquisition error; checked variants
  preserve evidence.
- Complex Python keyword surfaces become named request values; omission uses
  `std::optional<bool>` where omission differs from false.
- Raw target strings remain available beside validated IDs and targets.
- High-level methods never replay mutating requests after uncertain delivery or
  `MutationApplied`.

### Task 1: Map generated metadata to public evidence

**Files:**

- Bind: `tools/codegen/generate_metadata.py`
- Bind: `tools/parity/data/metadata/`
- Bind: `include/libtmux/generated/`
- Bind: `tests/unit/generated_metadata_test.cpp`
- Bind: `tools/evidence/ctest_gate.py`
- Create: `cxx/public-headers.json`
- Create: `tools/headers/generate_umbrella.py`
- Create: `tests/cxx/test_generate_umbrella.py`
- Modify: `include/libtmux/libtmux.hpp`
- Modify: `tools/parity/__main__.py`
- Modify: `tools/parity/shard.py`
- Modify: `tests/cxx/test_parity_manifest.py`
- Create: `examples/CMakeLists.txt`
- Create: `examples/parity/metadata.cpp`
- Create: `docs/api/metadata.md`
- Create: `tests/compile/metadata/`
- Modify: `cxx/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/compile/CMakeLists.txt`
- Modify: `tools/parity/data/mapping.json`
- Modify: `tools/parity/data/shards.json`
- Modify: `tools/parity/data/manifest.json`
- Modify: `tools/parity/data/evidence.json`

- [ ] **Step 1: Run the missing `metadata` shard**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard metadata
```

Expected: nonzero with format, field, option-key, hook-key, enum, and capability
entry IDs still pending.

- [ ] **Step 2: Recheck deterministic generated artifacts**

```console
$ uv run python tools/codegen/generate_metadata.py \
    --observations cxx/parity \
    --output tools/parity/data/metadata \
    --headers include/libtmux/generated \
    --check
```

Expected: fields, formats, options, hooks, enums, and capabilities reproduce
byte-for-byte from the pinned observations.

- [ ] **Step 3: Establish deterministic public-header and staging registries**

```console
$ uv run pytest \
    tests/cxx/test_generate_umbrella.py \
    tests/cxx/test_parity_manifest.py \
    -v
```

Expected: FAIL importing `generate_umbrella`. An unrelated fixture or collection
failure is not the intended red result.

`public-headers.json` lists each installed public header once with its source
kind, owning shard, and nonempty variant set. `generate_umbrella.py` sorts that
registry, rejects a missing or unregistered committed/generated header, a
configured header without its template and active build output, or an unknown
variant, and reproduces `libtmux.hpp` byte-for-byte.
`shard.py` writes the target, preset, CTest label, driver, owned paths,
`red_mode`, `red_case`, `red_diagnostic`, `gate_id`, and `gate_record` for every
row in the execution registry. `stage-paths` reads that immutable shard record
plus the differential registry and emits only literal file paths. Tests reject
an execution-table drift, sibling-shard change, directory, glob, or omitted
adapter/driver path. `run-shard` tests use fake CMake and CTest executables to
prove the exact configure, target build, selector, diagnostic, and gate-record
contract for both phases. They also reject overrides, stale trees, and empty or
extra CTest selections.

Implement the registry-driven umbrella generator and rerun the identical
focused test:

```console
$ uv run pytest \
    tests/cxx/test_generate_umbrella.py \
    tests/cxx/test_parity_manifest.py \
    -v
```

Expected: all umbrella-generator and shard-runner contract tests pass.

```console
$ uv run python tools/headers/generate_umbrella.py \
    --registry cxx/public-headers.json \
    --output include/libtmux/libtmux.hpp \
    --check
```

- [ ] **Step 4: Add public compile, example, and documentation evidence**

Compile every generated public name, execute every mapped
value/scope/version case, and map every generated symbol to the exact compile
case, documentation anchor, and executed example case. Retain the existing
below/at-boundary tests, including exact raw `3.7`, `3.7a`, and `3.7b`.

- [ ] **Step 5: Synchronize the reviewed metadata mapping**

```console
$ uv run python -m tools.parity \
    sync \
    --release tools/parity/data/release-v0.62.0.json \
    --development tools/parity/data/development.json \
    --mapping tools/parity/data/mapping.json \
    --approvals tools/parity/data/approvals.json \
    --evidence tools/parity/data/evidence.json \
    --output tools/parity/data/manifest.json
```

- [ ] **Step 6: Run the exact target and retain immutable CTest evidence**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target parity_metadata_test
```

```console
$ uv run python -m tools.evidence.ctest_gate \
    --source-dir cxx \
    --preset cxx-dev \
    --label '^parity-metadata$' \
    --gate-id parity-metadata \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/parity-metadata.json
```

Expected: the registered target and every metadata case pass. The gate captures
the immutable CTest inventory and JUnit result in a content-addressed leaf and
atomically points the named record at that leaf. A later successful current
run may replace the pointer without mutating either leaf. It rejects an empty
selection, a skipped or unregistered case, stale
CMake/CTest/executable/source digests, or replacement after a failed run.

```console
$ uv run python -m tools.parity \
    record-evidence \
    --shard metadata \
    --ctest-gate cxx/build/evidence/parity-metadata.json \
    --execution-mode generated-metadata \
    --output tools/parity/data/evidence.json
```

`record-evidence` verifies the immutable record, inventory, JUnit, target,
source, and registration digests before changing only metadata-owned evidence.
It never reads CTest's mutable `Testing/` directory.

Resynchronize the manifest after the atomic evidence replacement:

```console
$ uv run python -m tools.parity \
    sync \
    --release tools/parity/data/release-v0.62.0.json \
    --development tools/parity/data/development.json \
    --mapping tools/parity/data/mapping.json \
    --approvals tools/parity/data/approvals.json \
    --evidence tools/parity/data/evidence.json \
    --output tools/parity/data/manifest.json
```

```console
$ uv run pytest tests/cxx/test_generate_metadata.py -v
```

- [ ] **Step 7: Verify, stage, and commit the completed shard**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard metadata \
    --require-evidence
```

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard metadata \
    --output cxx/build/parity-stage/metadata.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/metadata.paths
```

```console
$ git commit -F - <<'EOF'
CXX(test[metadata]): Map generated API

why: Connect generated fields and capability data to public parity
     evidence.

what:
- Compile and execute generated formats, keys, enums, and capability
  names
- Map every generated symbol to documentation and version-boundary
  evidence
EOF
```

### Task 2: Complete Server construction and command behavior

**Files:**

- Modify: `include/libtmux/server.hpp`
- Modify: `src/server.cpp`
- Create: `tests/integration/server_connection_test.cpp`
- Create: `tests/compile/server_connection/`
- Create: `tests/differential/scenarios/server-connection.json`
- Create: `examples/parity/server_connection.cpp`
- Create: `docs/api/server-connection.md`
- Modify: `tools/parity/data/mapping.json`

**Coverage:** Constructors and factories, `from_env`, socket name/path/config
selection, raw `cmd`, equality, representation, capability access,
`is_alive`, `check_alive`, and no-side-effect destruction.

- [ ] **Step 1: Run the shard red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard server-connection
```

Expected: nonzero listing only missing Server connection entries.

- [ ] **Step 2: Implement and prove endpoint identity**

Different selectors, binaries, and config values reaching the same live socket
compare equal; different endpoints compare unequal. Invalid selector
combinations fail validation before dispatch.

- [ ] **Step 3: Run real-tmux, differential, and example tests**

```console
$ ctest \
    --preset cxx-dev \
    -L parity-server-connection \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_server_connection.py -v
```

- [ ] **Step 4: Commit the shard**

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard server-connection \
    --output cxx/build/parity-stage/server-connection.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/server-connection.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[server]): Add connection facade

why: Match Python server construction while hardening live-endpoint
     identity.

what:
- Add environment, socket, command, liveness, representation, and
  equality APIs
- Map compile, real-tmux, differential, documentation, and example
  evidence
EOF
```

### Task 3: Establish complete facade value shells

**Files:**

- Bind: `include/libtmux/session.hpp`
- Bind: `include/libtmux/window.hpp`
- Bind: `include/libtmux/pane.hpp`
- Bind: `include/libtmux/client.hpp`
- Bind: `src/session.cpp`
- Bind: `src/window.cpp`
- Bind: `src/pane.cpp`
- Bind: `src/client.cpp`
- Bind: `tests/unit/entity_value_shells_test.cpp`
- Bind: `tests/compile/entity_value_shells/`
- Create: `include/libtmux/environment.hpp`
- Create: `include/libtmux/options.hpp`
- Create: `include/libtmux/hooks.hpp`
- Create: `src/environment.cpp`
- Create: `src/options.cpp`
- Create: `src/hooks.cpp`
- Create: `tests/unit/facade_value_shells_test.cpp`
- Create: `tests/compile/facade_value_shells/`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `tests/compile/CMakeLists.txt`
- Modify: `cxx/public-headers.json`
- Modify: `include/libtmux/libtmux.hpp`

- [ ] **Step 1: Preserve entity shells and compile facade signatures red**

First rerun the existing entity value-shell target and tests unchanged. They
must prove complete `Session`, `Window`, `Pane`, and `Client` values before this
task begins; this task may not recreate or edit those types. Then add positive
probes that instantiate the `Environment`, `Options`, and `Hooks` return values
used by the entity accessors in Tasks 8 and 10.

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target entity_value_shells
```

```console
$ cmake --build --preset cxx-dev --target facade_value_shells
```

Expected: the entity target passes and the registered facade target fails only
because the three complete facade types do not exist.

- [ ] **Step 2: Add only the shared value representation**

The three facade shells own only connection identity, target, and typed scope.
They are copyable, issue no command during construction or destruction, and
declare no environment, option, or hook behavior yet. Tasks 15, 17, and 18 fill
those existing types rather than introducing them after entity accessors.

- [ ] **Step 3: Register, build, and run the shell tests**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target entity_value_shells
```

```console
$ cmake --build --preset cxx-dev --target facade_value_shells
```

```console
$ ctest \
    --preset cxx-dev \
    -R '^(entity|facade)\.value_shells$' \
    --no-tests=error \
    --output-on-failure
```

Expected: complete entity values compose in Server return types without a
query, mutation, or destructor dispatch, the preexisting entity-shell digests
are unchanged, and every entity accessor can name a complete facade return type.

```console
$ uv run python tools/headers/generate_umbrella.py \
    --registry cxx/public-headers.json \
    --output include/libtmux/libtmux.hpp \
    --check
```

- [ ] **Step 4: Commit the prerequisite values**

```console
$ git add \
    include/libtmux/environment.hpp \
    include/libtmux/options.hpp \
    include/libtmux/hooks.hpp \
    cxx/public-headers.json \
    include/libtmux/libtmux.hpp \
    src/environment.cpp \
    src/options.cpp \
    src/hooks.cpp \
    src/CMakeLists.txt \
    tests/unit/facade_value_shells_test.cpp \
    tests/unit/CMakeLists.txt \
    tests/compile/facade_value_shells \
    tests/compile/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(feat[facades]): Add value shells

why: Make hierarchy and inherited facade return types complete before
     their parity work.

what:
- Add opaque Environment, Options, and Hooks values beside existing
  entities
- Prove copy, move, equality, and no-side-effect destruction
EOF
```

### Task 4: Complete Server collections and search

**Files:** Modify `server.hpp`, `server.cpp`, mapping; create
`server_collections_test.cpp`, `server-collections.json`,
`server_collections.cpp`, and `server-collections.md` under the corresponding
integration, differential, examples, and API directories.

**Coverage:** Sessions, windows, panes, clients, attached sessions, Python
index/get/search behavior, lenient empty-on-every-tmux-error accessors, checked
variants, and loud raw tmux-format search errors.

- [ ] **Step 1: Run `server-collections` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard server-collections
```

Expected: missing collection and search entry IDs.

- [ ] **Step 2: Force no-daemon, missing-socket, permission, subprocess, parse,
      and relation-enrichment errors**

Every lenient accessor returns an empty complete snapshot. Every checked
accessor returns the typed cause and successful command evidence where present.

- [ ] **Step 3: Run and commit**

```console
$ ctest \
    --preset cxx-dev \
    -L parity-server-collections \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_server_collections.py -v
```

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard server-collections \
    --output cxx/build/parity-stage/server-collections.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/server-collections.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[server]): Add collections

why: Preserve list-shaped Python behavior while exposing checked
     diagnostics.

what:
- Add hierarchy, client, attached-session, and search accessors
- Prove lenient empty results and checked evidence for every failure
  class
EOF
```

### Task 5: Complete Server lifecycle and session creation

**Files:** Modify `server.hpp`, `server.cpp`, mapping; create
`server_sessions_test.cpp`, `server-sessions.json`, `server_sessions.cpp`, and
`server-sessions.md`.

**Coverage:** `new_session`, `has_session`, session-name/command/environment
request fields, `kill_server`, server start behavior, duplicate names,
hydration, and `MutationApplied` with retry prohibition.

- [ ] **Step 1: Run `server-sessions` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard server-sessions
```

- [ ] **Step 2: Force applied-but-unhydrated and uncertain-delivery cases**

Assert no incomplete Session escapes and the caller can inspect raw result,
known ID, hydration error, and delivery state.

- [ ] **Step 3: Run and commit**

```console
$ ctest --preset cxx-dev -L parity-server-sessions --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_server_sessions.py -v
```

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard server-sessions \
    --output cxx/build/parity-stage/server-sessions.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/server-sessions.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[server]): Manage sessions

why: Preserve mutation outcomes without unsafe automatic retry.

what:
- Add session creation, lookup, and server lifecycle requests
- Report applied hydration failures and uncertain delivery explicitly
EOF
```

### Task 6: Complete Server shell and buffer operations

**Files:** Modify `server.hpp`, `server.cpp`, mapping; create
`server_shell_buffers_test.cpp`, `server-shell-buffers.json`,
`server_shell_buffers.cpp`, and `server-shell-buffers.md`.

**Coverage:** Run-shell, wait-for, source-file, if-shell, buffer list/show/set/
load/save/paste/delete, named requests, raw bytes where Python preserves them,
and sensitive argument marking.

- [ ] **Step 1: Run `server-shell-buffers` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard server-shell-buffers
```

- [ ] **Step 2: Run real-tmux and differential cases**

```console
$ ctest \
    --preset cxx-dev \
    -L parity-server-shell-buffers \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_server_shell_buffers.py -v
```

- [ ] **Step 3: Commit the shard**

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard server-shell-buffers \
    --output cxx/build/parity-stage/server-shell-buffers.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/server-shell-buffers.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[server]): Add shell and buffers

why: Cover server-wide execution and paste-buffer behavior.

what:
- Add named shell, wait, source, condition, and buffer request values
- Preserve results, byte handling, failures, and sensitive diagnostics
EOF
```

### Task 7: Complete Server command, key, prompt, and client surfaces

**Files:** Modify `server.hpp`, `server.cpp`, mapping; create
`server_commands_test.cpp`, `scoped_control_client.{hpp,cpp}` in test support,
`server-commands.json`, `server_commands.cpp`, and `server-commands.md`.

**Coverage:** Key tables/bindings, command listing, messages, prompts, menus,
histories, display paths, refresh/suspend/lock/detach/switch client operations,
and non-hanging attach behavior through a test-only PTY/control client.

- [ ] **Step 1: Run `server-commands` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard server-commands
```

- [ ] **Step 2: Exercise interactive operations with bounded real clients**

No test uses a sleep as readiness. Every attach, prompt, and client action has
an absolute deadline and deterministic fixture-owned teardown.

- [ ] **Step 3: Run and commit**

```console
$ ctest --preset cxx-dev -L parity-server-commands --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_server_commands.py -v
```

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard server-commands \
    --output cxx/build/parity-stage/server-commands.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/server-commands.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[server]): Add command surfaces

why: Cover interactive and server command APIs with bounded real
     clients.

what:
- Add keys, prompts, menus, messages, histories, and client operations
- Extend test support for deterministic attached-client behavior
EOF
```

### Task 8: Complete Session values and relations

**Files:** Modify `session.hpp`, `session.cpp`, mapping; create
`session_values_test.cpp`, `session-values.json`, `session_values.cpp`, and
`session-values.md`.

**Coverage:** Factories, validated identity, cached fields, equality,
representation, refresh, window/pane snapshots, active window/pane, attached
clients, groups, environment/options/hooks access, and stale-state behavior.

- [ ] **Step 1: Run `session-values` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard session-values
```

- [ ] **Step 2: Prove copy and refresh isolation**

Refresh failure keeps old state. Refresh success changes only its receiver;
copies and prior snapshots retain their complete graphs.

- [ ] **Step 3: Run and commit**

```console
$ ctest \
    --preset cxx-sanitize \
    -L parity-session-values \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_session_values.py -v
```

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard session-values \
    --output cxx/build/parity-stage/session-values.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/session-values.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[session]): Add session values

why: Expose stable cached session state and complete hierarchy
     relations.

what:
- Add factories, fields, equality, refresh, and related snapshots
- Prove stale-state and copy isolation under real tmux mutation
EOF
```

### Task 9: Complete Session navigation and lifecycle

**Files:** Modify `session.hpp`, `session.cpp`, mapping; create
`session_navigation_test.cpp`, `session_lifecycle_test.cpp`,
`session-navigation.json`, `session-lifecycle.json`, two matching examples, and
two matching API pages.

**Coverage:** Last/next/previous/select/switch window, attach/detach, rename,
kill, group, new/kill window, optional flags, targets, and mutation hydration.

`attach` is the sole inherited-terminal operation. Its real-tmux case runs
inside the fixture-owned PTY, sends the bounded detach sequence, expects empty
captured output from the inner runner, and verifies the process exits. A caller
without a terminal receives a typed validation error before dispatch; ordinary
commands continue to use captured stdio.

- [ ] **Step 1: Run both Session shards red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard session-navigation \
    --shard session-lifecycle
```

- [ ] **Step 2: Run and commit navigation**

```console
$ ctest \
    --preset cxx-dev \
    -L parity-session-navigation \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_session_navigation.py -v
```

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard session-navigation \
    --output cxx/build/parity-stage/session-navigation.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/session-navigation.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[session]): Navigate windows

why: Match Python active, next, previous, last, and selection behavior.

what:
- Add typed navigation and switching requests
- Verify targets, return values, and stale-state refresh behavior
EOF
```

- [ ] **Step 3: Run and commit lifecycle**

```console
$ ctest \
    --preset cxx-dev \
    -L parity-session-lifecycle \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_session_lifecycle.py -v
```

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard session-lifecycle \
    --output cxx/build/parity-stage/session-lifecycle.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/session-lifecycle.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[session]): Add lifecycle

why: Cover session attachment, naming, grouping, windows, and teardown.

what:
- Add named lifecycle and window-management requests
- Preserve mutation delivery, hydration, and version-gated behavior
EOF
```

### Task 10: Complete Window values and relation topology

**Files:** Modify `window.hpp`, `window.cpp`, and mapping; create
`window_values_test.cpp`, `window-values.json`, `window_values.cpp`, and
`window-values.md`.

**Coverage:** Factories, identity, cached fields, equality, representation,
refresh, current session, linked sessions/winlinks, pane snapshot, active pane,
environment/options/hooks, and linked-window deduplication.

- [ ] **Step 1: Run `window-values` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard window-values
```

- [ ] **Step 2: Run sanitizer and differential evidence**

```console
$ ctest \
    --preset cxx-sanitize \
    -L parity-window-values \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_window_values.py -v
```

- [ ] **Step 3: Commit the shard**

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard window-values \
    --output cxx/build/parity-stage/window-values.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/window-values.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[window]): Add window values

why: Preserve window identity across linked-session relation graphs.

what:
- Add fields, refresh, session, winlink, pane, and active-pane relations
- Prove linked-window deduplication and immutable snapshot behavior
EOF
```

### Task 11: Complete Window layout, pane creation, and lifecycle

**Files:** Modify `window.hpp`, `window.cpp`, mapping; create tests, scenarios,
examples, and docs for shards `window-layout`, `window-panes`, and
`window-lifecycle`.

**Coverage:** Select, resize, rotate, layout select/set, swap; split/new/
floating panes and version-gated request fields; link/unlink/move/rename/
respawn/kill.

- [ ] **Step 1: Run all three Window shards red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard window-layout \
    --shard window-panes \
    --shard window-lifecycle
```

- [ ] **Step 2: Implement and commit layout behavior**

```console
$ ctest --preset cxx-dev -L parity-window-layout --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_window_layout.py -v
```

Commit as `CXX(feat[window]): Manage layouts` with its tests, scenario,
example, docs, and only `window-layout` mapping rows.

- [ ] **Step 3: Implement and commit pane creation**

```console
$ ctest --preset cxx-dev -L parity-window-panes --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_window_panes.py -v
```

Commit as `CXX(feat[window]): Create panes`; force post-split hydration failure
and below/at capability boundaries.

- [ ] **Step 4: Implement and commit lifecycle behavior**

```console
$ ctest --preset cxx-dev -L parity-window-lifecycle --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_window_lifecycle.py -v
```

Commit as `CXX(feat[window]): Add lifecycle` with its complete evidence set.

### Task 12: Complete Pane values and terminal I/O

**Files:** Modify `pane.hpp`, `pane.cpp`, and mapping; create tests,
scenarios, examples, and docs for `pane-values` and `pane-io`.

**Coverage:** Factories, identity, cached format fields, equality,
representation, refresh, parent window/session, geometry/edge predicates;
send-keys, capture, display, clear/reset/history, bytes/text normalization, and
sensitive commands.

- [ ] **Step 1: Run both Pane shards red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard pane-values \
    --shard pane-io
```

- [ ] **Step 2: Implement and commit values**

```console
$ ctest --preset cxx-sanitize -L parity-pane-values --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_pane_values.py -v
```

Commit as `CXX(feat[pane]): Add pane values` with relation, stale-state, and
copy-isolation evidence.

- [ ] **Step 3: Implement and commit I/O**

```console
$ ctest --preset cxx-dev -L parity-pane-io --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_pane_io.py -v
```

Commit as `CXX(feat[pane]): Add terminal I/O` with target lowering, return
shape, normalization, and timeout evidence.

### Task 13: Complete Pane layout, modes, and topology

**Files:** Modify `pane.hpp`, `pane.cpp`, mapping; create tests, scenarios,
examples, and docs for `pane-layout`, `pane-modes`, and `pane-topology`.

**Coverage:** Resize/select/split/new/floating; popup, paste, pipe, copy, clock,
choose, customize, find, prefix; respawn, move, join, break, swap, and the exact
tmux `3.7` behavior boundary.

- [ ] **Step 1: Run all three shards red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard pane-layout \
    --shard pane-modes \
    --shard pane-topology
```

- [ ] **Step 2: Implement and commit layout**

```console
$ ctest --preset cxx-dev -L parity-pane-layout --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_pane_layout.py -v
```

Commit as `CXX(feat[pane]): Manage layout`.

- [ ] **Step 3: Implement and commit modes**

```console
$ ctest --preset cxx-dev -L parity-pane-modes --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_pane_modes.py -v
```

Commit as `CXX(feat[pane]): Add tmux modes`; interactive cases use the bounded
test-only client fixture.

- [ ] **Step 4: Implement and commit topology**

```console
$ ctest --preset cxx-dev -L parity-pane-topology --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_pane_topology.py -v
```

Commit as `CXX(feat[pane]): Change topology`; include below/exact/above `3.7`
capability cases and applied hydration failure.

### Task 14: Complete Client values and attachment resolution

**Files:**

- Modify: `include/libtmux/client.hpp`
- Modify: `src/client.cpp`
- Create: `tests/integration/client_test.cpp`
- Create: `tests/differential/scenarios/client.json`
- Create: `examples/parity/client.cpp`
- Create: `docs/api/client.md`
- Modify: `tools/parity/data/mapping.json`

**Coverage:** Identity, fields, equality, representation, refresh, attached
session/window/pane resolution, stale attachment, and client operations exposed
from both Server and Client.

- [ ] **Step 1: Run `client` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard client
```

- [ ] **Step 2: Run bounded attached-client tests and commit**

```console
$ ctest --preset cxx-dev -L parity-client --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_client.py -v
```

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard client \
    --output cxx/build/parity-stage/client.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/client.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[client]): Add client values

why: Resolve live client attachment without weakening stale-object
     behavior.

what:
- Add client fields, refresh, equality, and hierarchy relations
- Verify attached and stale states through a bounded real client
EOF
```

### Task 15: Complete environment operations

**Files:** Modify `environment.hpp`, `environment.cpp`; create integration tests,
`environment.json`, `environment.cpp` example, `environment.md`, and modify the
mapping.

**Coverage:** Show/get/set/unset/remove, scope and target selection, removed
variable representation, iteration/indexing, missing values, errors, and
sensitive value redaction.

- [ ] **Step 1: Run `environment` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard environment
```

- [ ] **Step 2: Run, compare, and commit**

```console
$ ctest --preset cxx-dev -L parity-environment --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_environment.py -v
```

Commit the exact environment paths as
`CXX(feat[environment]): Manage variables`.

### Task 16: Complete sparse arrays

**Files:** Create `sparse_array.hpp`, unit and compile tests,
`sparse-array.json`, `sparse_array.cpp` example, and `sparse-array.md`; modify
the mapping.

**Coverage:** Sparse add, append, iteration, lookup, dense conversion, missing
indices, invalid indices, and deterministic ordering.

- [ ] **Step 1: Run `sparse-array` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard sparse-array
```

- [ ] **Step 2: Run runtime, compile, and differential cases**

```console
$ ctest --preset cxx-dev -L parity-sparse-array --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_sparse_array.py -v
```

- [ ] **Step 3: Commit the prerequisite type**

Commit the exact sparse-array and inherited slice paths as
`CXX(feat[types]): Add sparse arrays`.

### Task 17: Complete typed options

**Files:** Modify `options.hpp`, `options.cpp`; create integration and compile
tests, `options.json`, `options.cpp` example, and `options.md`; bind
`tools/parity/data/metadata/options.json` and
`include/libtmux/generated/options.hpp`; modify the mapping.

**Coverage:** Typed generated keys, server/session/window/pane scope validation,
get/set/unset/show, sparse arrays, conversion, defaults, invalid value/category
errors, and inherited option behavior.

- [ ] **Step 1: Run `options` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard options
```

- [ ] **Step 2: Run positive, negative, and differential cases**

```console
$ ctest --preset cxx-dev -L parity-options --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_options.py -v
```

- [ ] **Step 3: Commit**

Commit the exact option and inherited slice paths as
`CXX(feat[options]): Add typed options`.

### Task 18: Complete hooks

**Files:** Modify `hooks.hpp`, `hooks.cpp`; create integration and compile
tests, `hooks.json`, `hooks.cpp` example, and `hooks.md`; bind
`tools/parity/data/metadata/hooks.json` and
`include/libtmux/generated/hooks.hpp`; modify the mapping.

**Coverage:** Typed hook keys, scope, show/set/unset/run, sparse tmux command
indices, bulk set, invalid indices, and inherited hook behavior.

- [ ] **Step 1: Run `hooks` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard hooks
```

- [ ] **Step 2: Run runtime, compile, and differential cases**

```console
$ ctest --preset cxx-dev -L parity-hooks --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_hooks.py -v
```

- [ ] **Step 3: Commit**

Commit the exact hook and inherited slice paths as
`CXX(feat[hooks]): Add typed hooks`.

### Task 19: Consolidate `neo` and complete query parity

**Files:**

- Bind: `include/libtmux/generated/fields.hpp`
- Bind: `tools/parity/data/metadata/fields.json`
- Modify: `include/libtmux/query/`
- Create: `tests/integration/query_parity_test.cpp`
- Create: `tests/compile/query_parity/`
- Create: `tests/differential/scenarios/query.json`
- Create: `examples/parity/query.cpp`
- Create: `docs/api/query.md`
- Modify: `tools/parity/data/mapping.json`

**Coverage:** Every `neo.Obj` field and lookup capability, generated fields,
scalar and relation filters, all operators, direct invocation, standard views,
`matching`, cardinality helpers, edge parser, optional JSON lowering,
collection/query return shapes, and approved semantics. The module name
`neo.py` is excluded as a Python import artifact only after every capability is
mapped elsewhere.

- [ ] **Step 1: Run `query-neo` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard query-neo
```

Expected: nonzero listing every unmapped query or `neo` capability.

- [ ] **Step 2: Run complete query and oracle gates**

```console
$ ctest --preset cxx-sanitize -L parity-query --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_query.py tests/cxx/test_query_oracle.py -v
```

- [ ] **Step 3: Verify no functional `neo` exclusion**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard query-neo \
    --reject-functional-exclusions
```

Expected: exit zero.

- [ ] **Step 4: Commit the shard**

```console
$ uv run python -m tools.parity \
    stage-paths \
    --shard query-neo \
    --output cxx/build/parity-stage/query-neo.paths
```

```console
$ git add --pathspec-from-file=cxx/build/parity-stage/query-neo.paths
```

```console
$ git commit -F - <<'EOF'
CXX(feat[query]): Consolidate query API

why: Preserve modern Python query capabilities without a duplicate
     entity tree.

what:
- Map every neo field, lookup, relation, range, parser, and serializer
  behavior
- Exclude only the Python module boundary after capability closure
EOF
```

### Task 20: Complete common values and version surfaces

**Files:** Modify public version and capability headers; bind
`tools/parity/data/metadata/enums.json` and
`include/libtmux/generated/enums.hpp`; create unit and compile tests,
`common-version.json`, `common_version.cpp`, and `common-version.md`; modify the
mapping.

**Coverage:** Public constants and enums, version parsing/comparison, capability
lookups, return-value containers, and below/at-boundary behavior.

- [ ] **Step 1: Run `common-version` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard common-version
```

- [ ] **Step 2: Run compile, boundary, and differential cases**

```console
$ ctest --preset cxx-dev -L parity-common-version --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_common_version.py -v
```

- [ ] **Step 3: Commit the shard**

Commit the exact common/version and inherited slice paths as
`CXX(feat[common]): Add version values`.

### Task 21: Complete errors and warnings

**Files:** Modify error and diagnostic headers; create unit and integration
tests, `warnings-errors.json`, `warnings_errors.cpp`, and `warnings-errors.md`;
modify the mapping.

**Coverage:** Python exception hierarchy mapping, command and lookup errors,
warning categories, warn-and-continue behavior, diagnostic sink containment,
and stable error inspection.

- [ ] **Step 1: Run `warnings-errors` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard warnings-errors
```

- [ ] **Step 2: Prove diagnostic containment**

Warn-and-continue behavior does not write global stderr or configure logging.
A throwing sink is caught, disabled, and inspectable without changing the
operation result.

- [ ] **Step 3: Run and commit**

```console
$ ctest --preset cxx-dev -L parity-warnings-errors --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_warnings_errors.py -v
```

Commit the exact error/warning and inherited slice paths as
`CXX(feat[errors]): Map diagnostics`.

### Task 22: Complete compatibility protocols

**Files:** Modify entity and query headers; create integration and compile
tests, `compatibility-protocols.json`, `compatibility_protocols.cpp`, and
`compatibility-protocols.md`; modify mapping and approvals.

**Coverage:** Deprecated aliases, indexing and `get` conveniences,
context-management adaptation, Python protocol return shapes, and approved
nonfunctional exclusions for import and typing mechanics.

- [ ] **Step 1: Run `compatibility-protocols` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard compatibility-protocols
```

- [ ] **Step 2: Reject functional exclusions**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard compatibility-protocols \
    --reject-functional-exclusions
```

- [ ] **Step 3: Run and commit**

```console
$ ctest \
    --preset cxx-dev \
    -L parity-compatibility-protocols \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_compatibility_protocols.py -v
```

Commit the exact compatibility and inherited slice paths as
`CXX(feat[compat]): Map Python protocols`.

### Task 23: Complete testing support

**Files:** Modify `ScopedTmuxServer` support only where a mapped capability
requires it; create fixture unit/integration/compile tests,
`testing-support.json`, `testing_support.cpp`, and `testing-support.md`; modify
mapping and approvals.

**Coverage:** Python testing helper capabilities, isolated server/session
construction, socket selectors, cleanup, failure injection, and approved
exclusions for pytest registration mechanics rather than runtime behavior.

- [ ] **Step 1: Run `testing-support` red**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard testing-support
```

- [ ] **Step 2: Prove fixture behavior and exclusions**

```console
$ ctest --preset cxx-dev -L parity-testing-support --no-tests=error --output-on-failure
```

```console
$ uv run pytest tests/cxx/differential/test_testing_support.py -v
```

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode structural \
    --shard testing-support \
    --reject-functional-exclusions
```

- [ ] **Step 3: Commit the shard**

Commit the exact fixture and inherited slice paths as
`CXX(test[fixtures]): Map testing support`.

### Task 24: Close the full parity ledger

**Files:**

- Bind: `cxx/public-headers.json`
- Bind: `tools/parity/data/shards.json`
- Modify: `tools/parity/data/mapping.json`
- Modify: `tools/parity/data/approvals.json`
- Modify: `tools/parity/data/evidence.json`
- Modify: `tools/parity/data/manifest.json`
- Modify: `tools/parity/__main__.py`
- Create: `tools/headers/inventory_public_api.py`
- Create: `tools/parity/check_api_coverage.py`
- Create: `tools/parity/refresh_evidence.py`
- Create: `tools/parity/write_completion_evidence.py`
- Create: `tools/differential/check_coverage.py`
- Create: `tools/evidence/parity_gates.py`
- Create: `tests/cxx/test_api_coverage.py`
- Create: `tests/cxx/test_public_api_inventory.py`
- Create: `tests/cxx/test_parity_completion_evidence.py`
- Create: `tests/cxx/test_differential_coverage.py`
- Create: `tests/cxx/test_parity_ctest_gates.py`
- Create: `tests/cxx/test_refresh_evidence.py`
- Create: `docs/evidence/parity-completion.schema.json`
- Create: `docs/evidence/parity-completion.json`
- Create: `docs/reviews/parity.md`

- [ ] **Step 1: Write and run the closure-tool tests red**

Tests cover deterministic public-declaration inventory, an exported declaration
missing from the parity manifest, duplicate canonical ownership, invalid alias
ownership, stale CTest evidence, incomplete differential dispatch, and stale
completion inputs. Bulk-refresh tests cover missing, extra, duplicate, and
stale shard results, a differential result for metadata, missing differential
results for behavior shards, cross-index tmux-binary disagreement,
deterministic output, and no partial replacement after validation failure.

```console
$ uv run pytest \
    tests/cxx/test_public_api_inventory.py \
    tests/cxx/test_api_coverage.py \
    tests/cxx/test_parity_completion_evidence.py \
    tests/cxx/test_differential_coverage.py \
    tests/cxx/test_parity_ctest_gates.py \
    tests/cxx/test_refresh_evidence.py \
    -v
```

Expected: FAIL importing the public-inventory, coverage, completion, gate, or
bulk-refresh tools. An unrelated fixture or collection error is not the
intended red result.

- [ ] **Step 2: Implement and verify the closure tools**

`inventory_public_api.py` discovers the actual public-header set independently
of the parity manifest, requires it to match `public-headers.json`, and invokes
the pinned Clang AST JSON dump on one generated translation unit per header. It
records only externally usable declarations in the configured `libtmux` ABI
namespace, excluding implicit declarations, private/protected members, and the
private `detail` namespace. Its stable `cpp_api_id` includes declaration kind,
fully qualified name, normalized parameter/result types, cv/ref/noexcept
qualifiers, and template arity. The inventory command accepts no parity
manifest. It records the header registry, compiler, configuration, and source
digests with repository-relative paths.

Coverage requires a bijection: every inventoried declaration has exactly one
canonical manifest owner and every canonical `cpp_api_id` resolves to exactly
one inventoried declaration. Python alias rows resolve through `cpp_alias_of`
and own no second compile, documentation, or example mapping. A fixture adds a
public declaration without a manifest row and requires an `unmapped_cpp_api`
violation.

`refresh_evidence.py` joins the machine shard registry, current manifest,
current-source immutable CTest index, and current-source differential result
index. It requires exactly one successful CTest gate for every shard, exactly
one successful differential result for every non-metadata behavior shard, and
no differential result for metadata. It verifies the owned source,
registration, executable, scenario, adapter, selected tmux binary, inventory,
JUnit digests, and the manifest's evidence-independent
`semantic_contract_sha256` before replacing the execution fields of existing
shard-owned evidence IDs. It cannot create, remove, or reassign an evidence ID
or change mapping and approval decisions. Missing, extra, duplicate, stale, or
cross-index records reject the entire update; only a fully validated,
deterministically sorted sidecar replaces `--evidence` atomically. Tests prove
that an evidence-only refresh and resynchronization preserve the semantic
contract digest, while any observation, reviewed mapping, approval, or shard
ownership change replaces it and invalidates older differential results.

Implement the remaining current-source CTest, differential, bulk-refresh, and
completion checks, then rerun the exact Step 1 command unchanged:

```console
$ uv run pytest \
    tests/cxx/test_public_api_inventory.py \
    tests/cxx/test_api_coverage.py \
    tests/cxx/test_parity_completion_evidence.py \
    tests/cxx/test_differential_coverage.py \
    tests/cxx/test_parity_ctest_gates.py \
    tests/cxx/test_refresh_evidence.py \
    -v
```

Expected: all focused unit tests pass.

- [ ] **Step 3: Run completion mode before closure**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode complete
```

Expected before final corrections: nonzero with exact unclassified,
unimplemented, stale, unapproved, or missing-evidence IDs. Do not suppress them.

- [ ] **Step 4: Verify unique compile, runtime, docs, and example evidence**

`parity_gates.py` reads the machine shard registry, configures each distinct
preset, builds every exact target, and invokes `ctest_gate` once per anchored
label. It rejects a row/table mismatch, absent target, empty or extra CTest
selection, failed/skipped case, stale source/configuration/executable, or a gate
record that does not match the row. It writes a normalized current-source index
of immutable gate-record digests; it never reads `Testing/` or reuses an older
source record.

```console
$ uv run python -m tools.evidence.parity_gates \
    --source-dir cxx \
    --shards tools/parity/data/shards.json \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/parity-ctest-index.json
```

`check_api_coverage.py` rejects unknown IDs, missing IDs, duplicate ownership,
unregistered examples, examples that compile but are not executed, documentation
anchors that do not exist, and version-gated entries without below/at-boundary
tests.

Generate the independent C++ declaration inventory after the parity gate has
configured and built the current development tree:

```console
$ uv run python tools/headers/inventory_public_api.py \
    --registry cxx/public-headers.json \
    --include-root include \
    --build-dir cxx/build/cxx-dev \
    --output cxx/build/public-api-inventory.json
```

```console
$ uv run python tools/parity/check_api_coverage.py \
    --manifest tools/parity/data/manifest.json \
    --public-api cxx/build/public-api-inventory.json \
    --ctest-index cxx/build/evidence/parity-ctest-index.json \
    --output cxx/build/api-coverage.json
```

Expected: exit zero.

- [ ] **Step 5: Run every differential scenario locally**

```console
$ uv run pytest tests/cxx/differential -v
```

Expected: Python and C++ canonical results match for the locally selected tmux
binary, and the supervisor writes the normalized current-source index at
`cxx/build/differential-results.json`. Each result records
`semantic_contract_sha256`; it does not record the full manifest or evidence
sidecar digest.

`check_coverage.py` joins `manifest.json`, `shards.json`,
`scenario_registry.json`, both adapters' executed `--list-operations` output,
the execution-registry driver paths, and the result index. For every
non-metadata behavior shard it
requires at least one owned scenario, the exact driver, and both a Python and a
C++ handler for every operation tag. It rejects duplicate or orphaned
scenarios, tags, handlers, or drivers and requires a successful local result
whose source, semantic contract, scenario-registry, tmux binary, and adapter
digests are current. An evidence-only manifest refresh does not invalidate that
semantic identity.

```console
$ uv run python tools/differential/check_coverage.py \
    --mode local \
    --manifest tools/parity/data/manifest.json \
    --shards tools/parity/data/shards.json \
    --registry tests/differential/scenario_registry.json \
    --python-adapter tools/differential/python_reference.py \
    --cpp-adapter cxx/build/cxx-dev/tests/differential/cpp_adapter \
    --drivers tests/cxx/differential \
    --results cxx/build/differential-results.json \
    --output cxx/build/differential-coverage.json
```

Expected: exit zero with every behavior shard, scenario, handler, driver, and
current local result accounted for. Stable-version matrix proof remains the
next plan.

- [ ] **Step 6: Refresh every shard's durable evidence and manifest binding**

Refresh the durable evidence sidecar only from the current-source indexes
produced in Steps 4 and 5:

```console
$ uv run python -m tools.parity \
    refresh-evidence \
    --manifest tools/parity/data/manifest.json \
    --shards tools/parity/data/shards.json \
    --ctest-index cxx/build/evidence/parity-ctest-index.json \
    --differential-index cxx/build/differential-results.json \
    --evidence tools/parity/data/evidence.json
```

Expected: every existing shard-owned execution record is refreshed in one
atomic replacement. A stale or missing record leaves `evidence.json`
byte-for-byte unchanged.

Resynchronize the reviewed mapping with both durable sidecars:

```console
$ uv run python -m tools.parity \
    sync \
    --release tools/parity/data/release-v0.62.0.json \
    --development tools/parity/data/development.json \
    --mapping tools/parity/data/mapping.json \
    --approvals tools/parity/data/approvals.json \
    --evidence tools/parity/data/evidence.json \
    --output tools/parity/data/manifest.json
```

Expected: `manifest.json` deterministically embeds the current approval and
evidence sidecars without changing reviewed classifications.

- [ ] **Step 7: Generate schema-validated completion evidence**

`write_completion_evidence.py` consumes the manifest digest, validator report,
refreshed durable evidence sidecar, immutable CTest gate records, differential
result index, differential coverage report, independent public-declaration
inventory, documentation/example coverage report, and exact source/tool
identities. It rejects a sidecar that differs from the manifest binding, a
missing or nonzero gate, incomplete handler or driver coverage, an unregistered
test, a differential semantic-contract digest that differs from the manifest,
stale input digest, absolute path, or unknown evidence ID. The full manifest
digest binds the refreshed evidence; differential artifacts bind only its
embedded semantic digest, so the dependency graph is acyclic. It validates the
output against
`parity-completion.schema.json` and supports `--check` byte-for-byte
regeneration.

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode complete \
    --report cxx/build/parity-verify.json
```

```console
$ uv run pytest tests/cxx/test_parity_completion_evidence.py -v
```

```console
$ uv run python tools/parity/write_completion_evidence.py \
    --manifest tools/parity/data/manifest.json \
    --evidence tools/parity/data/evidence.json \
    --public-api cxx/build/public-api-inventory.json \
    --validator cxx/build/parity-verify.json \
    --ctest-index cxx/build/evidence/parity-ctest-index.json \
    --differential cxx/build/differential-results.json \
    --differential-coverage cxx/build/differential-coverage.json \
    --coverage cxx/build/api-coverage.json \
    --output docs/evidence/parity-completion.json
```

Expected: the committed evidence validates and names only successful current
inputs.

- [ ] **Step 8: Obtain an independent parity review**

The reviewer samples generated observations against both Git source revisions,
checks inherited and testing surfaces, challenges every adaptation/exclusion,
and traces every manifest row and independently inventoried C++ declaration to
live evidence. Resolve every finding, repeat the CTest and differential runs in
Steps 4 and 5, rerun the exact bulk refresh and sidecar-bound synchronization
commands in Step 6, and regenerate completion evidence in Step 7. The reviewed
source, durable evidence, manifest binding, and completion artifact must all
describe the same current run.

- [ ] **Step 9: Run completion mode and commit**

```console
$ uv run python -m tools.parity \
    verify \
    --manifest tools/parity/data/manifest.json \
    --mode complete
```

Expected: exit zero with no missing required entry, unreviewed drift, pending
approval, stale classification, duplicate evidence, or functional exclusion.

```console
$ git add \
    tools/parity/data/mapping.json \
    tools/parity/data/approvals.json \
    tools/parity/data/evidence.json \
    tools/parity/data/manifest.json \
    tools/parity/__main__.py \
    tools/headers/inventory_public_api.py \
    tools/parity/check_api_coverage.py \
    tools/parity/refresh_evidence.py \
    tools/parity/write_completion_evidence.py \
    tools/differential/check_coverage.py \
    tools/evidence/parity_gates.py \
    tests/cxx/test_api_coverage.py \
    tests/cxx/test_public_api_inventory.py \
    tests/cxx/test_parity_completion_evidence.py \
    tests/cxx/test_differential_coverage.py \
    tests/cxx/test_parity_ctest_gates.py \
    tests/cxx/test_refresh_evidence.py \
    docs/evidence/parity-completion.schema.json \
    docs/evidence/parity-completion.json \
    docs/reviews/parity.md
```

```console
$ git commit -F - <<'EOF'
CXX(test[parity]): Close API ledger

why: Require complete source, behavior, documentation, and example
     evidence.

what:
- Reject every missing, stale, duplicate, unapproved, or unmapped entry
- Close an independent source-to-C++ parity review
EOF
```
