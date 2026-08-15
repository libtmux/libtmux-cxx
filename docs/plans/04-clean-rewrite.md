# Clean production rewrite implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Freeze the measured architecture, delete every contender
implementation, and implement the C++23 production core against retained
black-box contracts with no production dependency on spike artifacts.

**Architecture:** One compiled `libtmux` target exposes copyable value entities
over shared opaque connection state, byte-preserving POSIX execution, immutable
snapshots and relation graphs, and the selected owning query AST. Transport,
process, parsing, and mutable state remain compiled and private.

**Tech Stack:** C++23 `std::expected`, CMake 3.25+, POSIX, standard ranges,
GoogleTest, compile contracts, real tmux, ASan, UBSan, and the approved bakeoff
reports. C++20 packaging is added later without changing these contracts.

## Entry Conditions

Task 1 may begin only when:

- Transport and query decisions name one passing winner each.
- Every query semantic adaptation has explicit approval evidence.
- The relation trigger is resolved as either `not_run` with proof or a selected
  passing layout.
- Control-mode and engine-ops graft reports are complete.
- No production implementation exists under `cxx/include/` or `cxx/src/`.

Task 2 may begin only after Task 1 is committed, its pre-rewrite architecture
review has no unresolved finding, and the recorded decision has explicit user
acceptance. Task 4 may begin only after Tasks 2 and 3 are committed, no tracked
spike implementation remains, and the retained contract and rewrite-boundary
evidence both validate. Execute each task once in numeric order.

## Per-Task Gate

For each task, run the named failing test first. After the focused test passes,
run:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev
```

```console
$ ctest --preset cxx-dev --no-tests=error --output-on-failure
```

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize
```

```console
$ ctest --preset cxx-sanitize --no-tests=error --output-on-failure
```

The sanitizer configure and build must follow the latest source or CMake change;
CTest against an absent or stale sanitizer executable is not evidence. Run the
Python gate in `00-program.md` before committing. Do not weaken or rewrite a
retained assertion to fit the implementation.

### Task 1: Freeze and accept the selected architecture

**Files:**

- Create: `cxx/docs/bakeoffs/architecture-decision.json`
- Create: `cxx/docs/bakeoffs/architecture-decision.md`
- Create: `cxx/docs/reviews/pre-rewrite-architecture.md`
- Create: `cxx/tools/bakeoff/verify_architecture.py`
- Create: `tests/cxx/test_verify_architecture.py`

**Interfaces:**

`verify_architecture.py` accepts `--decision`, `--transport`, `--query`,
`--control`, `--engine`, and `--require-approval`. It rejects a missing
contender, failed hard gate, missing measurement, pending semantic adaptation,
unresolved reviewer finding, absent source identity, unapproved final
architecture, or any unknown classified as material. It also requires a linked
follow-up spike, measurement, and closed review for every unknown that was ever
classified as material.

- [ ] **Step 1: Write the failing completeness test**

Use fixtures missing one contender, one engine source identity, one relation
trigger result, and one review disposition. Require stable violation codes and
repository-relative evidence paths.

- [ ] **Step 2: Run the verifier test before implementation**

```console
$ uv run pytest tests/cxx/test_verify_architecture.py -v
```

Expected: FAIL importing `verify_architecture`.

- [ ] **Step 3: Synthesize the final architecture decision**

The JSON names the selected transport mechanism, AST representation, relation
layout or `not_run` proof, accepted and rejected grafts, public contract hash,
all semantic approvals, hard-gate evidence, rejected trade-offs, and remaining
unknowns with materiality and follow-up dispositions. No material unknown may
remain. The Markdown explains those choices without local paths or
branch-internal narrative.

- [ ] **Step 4: Obtain a fresh pre-rewrite review**

A reviewer who implemented no contender examines lifetime ownership,
transport leakage, exception containment, error attribution, hidden I/O,
query typing, ABI assumptions, package implications, and scope. Fix every
correctness or public-contract finding; record evidence-backed dispositions
for the remainder.

- [ ] **Step 5: Request explicit user acceptance**

Set `final_approval` to `pending`, present the decision and review, and stop.
After explicit approval, record its stable approval ID and exact accepted
decision hash. Do not infer approval from approval of this implementation plan.

- [ ] **Step 6: Verify and commit the architecture**

Rerun the identical focused test after implementation, review, and approval:

```console
$ uv run pytest tests/cxx/test_verify_architecture.py -v
```

Expected: all architecture-verifier tests pass.

```console
$ uv run python cxx/tools/bakeoff/verify_architecture.py \
    --decision cxx/docs/bakeoffs/architecture-decision.json \
    --transport cxx/docs/bakeoffs/transport/decision.json \
    --query cxx/docs/bakeoffs/query/decision.json \
    --control cxx/docs/bakeoffs/grafts/control-mode.json \
    --engine cxx/docs/bakeoffs/grafts/engine-ops.json \
    --require-approval
```

Expected: exit zero.

```console
$ git add \
    cxx/docs/bakeoffs/architecture-decision.json \
    cxx/docs/bakeoffs/architecture-decision.md \
    cxx/docs/reviews/pre-rewrite-architecture.md \
    cxx/tools/bakeoff/verify_architecture.py \
    tests/cxx/test_verify_architecture.py
```

```console
$ git commit -F - <<'EOF'
CXX(docs[design]): Select C++ architecture

why: Freeze the measured public and private contracts before
     implementation.

what:
- Synthesize transport, query, relation, and graft decisions
- Record explicit acceptance and close the pre-rewrite review
EOF
```

### Task 2: Freeze production acceptance tests

**Files:**

- Create: `cxx/tests/acceptance/CMakeLists.txt`
- Move: `cxx/tests/contracts/transport/` to
  `cxx/tests/acceptance/transport/`
- Move: `cxx/tests/contracts/query/` to `cxx/tests/acceptance/query/`
- Move: `cxx/tests/contracts/compile/query/` to
  `cxx/tests/acceptance/compile/query/`
- Create: `cxx/tests/acceptance/public_contract.json`
- Create: `cxx/tools/bakeoff/freeze_contract.py`
- Create: `tests/cxx/test_freeze_contract.py`
- Modify: `cxx/tests/CMakeLists.txt`

- [ ] **Step 1: Test contract hashing and forbidden dependencies**

The tool hashes every retained source and golden in sorted path order. The
exact output path is excluded from its own input set, and a second invocation
must reproduce the same bytes. It rejects any other exclusion, includes from
`cxx/spikes/`, candidate compile definitions, candidate names, private backend
types, absolute paths, and sources outside the retained
acceptance/data/schema boundary.

- [ ] **Step 2: Run the focused test before implementation**

```console
$ uv run pytest tests/cxx/test_freeze_contract.py -v
```

Expected: FAIL importing `freeze_contract`.

- [ ] **Step 3: Implement and verify deterministic contract freezing**

Implement only the sorted hashing, output exclusion, and forbidden-dependency
checks described in Step 1, then rerun the focused test:

```console
$ uv run pytest tests/cxx/test_freeze_contract.py -v
```

Expected: all contract-freezing unit tests pass.

- [ ] **Step 4: Retarget only include names and bindings**

Change candidate include selection to final `libtmux/...` headers. Preserve all
behavioral assertions, compile-valid cases, compile-invalid cases, schema
events, deadlines, and sanitizer coverage.

- [ ] **Step 5: Generate and verify the frozen contract**

```console
$ uv run python cxx/tools/bakeoff/freeze_contract.py \
    --tests cxx/tests/acceptance \
    --schema cxx/schema \
    --output cxx/tests/acceptance/public_contract.json
```

Expected: a deterministic manifest with no contender dependency.

- [ ] **Step 6: Commit the frozen tests**

```console
$ git add \
    cxx/tests/CMakeLists.txt \
    cxx/tests/acceptance \
    cxx/tests/contracts \
    cxx/tools/bakeoff/freeze_contract.py \
    tests/cxx/test_freeze_contract.py
```

```console
$ git commit -F - <<'EOF'
CXX(test[contract]): Freeze accepted surface

why: Preserve measured behavior while making candidate code disposable.

what:
- Bind black-box transport and query contracts to final include names
- Hash retained tests, schemas, and goldens without spike dependencies
EOF
```

### Task 3: Delete and audit every spike implementation

**Files:**

- Delete: `cxx/spikes/`
- Create: `cxx/tools/bakeoff/verify_rewrite_boundary.py`
- Create: `tests/cxx/test_verify_rewrite_boundary.py`
- Create: `cxx/docs/evidence/rewrite-boundary.json`
- Modify: `.gitignore`
- Modify: `cxx/CMakeLists.txt`

**Interfaces:**

```python
def verify_rewrite_boundary(
    repo: pathlib.Path,
    *,
    retained_manifest: pathlib.Path,
    decision: pathlib.Path,
) -> tuple[Violation, ...]: ...
```

- [ ] **Step 1: Write the verifier against the known spike tree**

The red test requires violations for tracked spike sources, spike symlinks,
submodules, generated binaries, candidate include paths, production
`cxx/include/` or `cxx/src/`, and build output outside ignored directories. It
also requires deterministic capture of the current commit, spike subtree, and
every regular tracked spike blob path, object ID, and mode before deletion.

```console
$ uv run pytest tests/cxx/test_verify_rewrite_boundary.py -v
```

Expected: FAIL importing `verify_rewrite_boundary`. An unrelated fixture or
collection failure is not the intended red result.

- [ ] **Step 2: Run it while spikes still exist**

Implement the verifier and rerun the identical focused command:

```console
$ uv run pytest tests/cxx/test_verify_rewrite_boundary.py -v
```

Expected: all verifier unit tests pass and their pre-deletion fixture reports
`cxx/spikes/` as the blocking production boundary.

- [ ] **Step 3: Remove exactly the disposable subtree**

Confirm the target is `cxx/spikes/`, remove only that subtree, and preserve
`cxx/docs/bakeoffs/`, `cxx/schema/`, `cxx/tests/acceptance/`, and
`cxx/tests/data/`.

Stage only the spike deletion so the verifier can inspect the deletion-ready
index while `HEAD` still names the immutable spike baseline:

```console
$ git add -A cxx/spikes
```

- [ ] **Step 4: Prove the branch is ready for a clean rewrite**

```console
$ uv run python cxx/tools/bakeoff/verify_rewrite_boundary.py \
    --manifest cxx/tests/acceptance/public_contract.json \
    --decision cxx/docs/bakeoffs/architecture-decision.json \
    --spike-revision HEAD \
    --output cxx/docs/evidence/rewrite-boundary.json
```

Expected: exit zero, no indexed file under `cxx/spikes/`, `cxx/include/`, or
`cxx/src/`, and a deterministic baseline for every spike blob reachable from
the recorded revision.

- [ ] **Step 5: Commit deletion separately**

```console
$ git add \
    -A \
    cxx/spikes \
    cxx/CMakeLists.txt \
    cxx/tools/bakeoff/verify_rewrite_boundary.py \
    tests/cxx/test_verify_rewrite_boundary.py \
    cxx/docs/evidence/rewrite-boundary.json \
    .gitignore
```

```console
$ git commit -F - <<'EOF'
CXX(chore[rewrite]): Remove spike sources

why: Make the production implementation independent of contender code.

what:
- Delete all transport, query, relation, and graft implementations
- Add a machine-checked clean-rewrite boundary
EOF
```

### Task 4: Generate production metadata from the parity ledger

**Files:**

- Create: `cxx/tools/codegen/generate_metadata.py`
- Create: `tests/cxx/test_generate_metadata.py`
- Create: `cxx/VERSION`
- Create: `cxx/cmake/Version.cmake`
- Create: `cxx/include/libtmux/config.hpp.in`
- Create: `cxx/parity/metadata/fields.json`
- Create: `cxx/parity/metadata/formats.json`
- Create: `cxx/parity/metadata/options.json`
- Create: `cxx/parity/metadata/hooks.json`
- Create: `cxx/parity/metadata/capabilities.json`
- Create: `cxx/parity/metadata/enums.json`
- Create: `cxx/include/libtmux/generated/fields.hpp`
- Create: `cxx/include/libtmux/generated/formats.hpp`
- Create: `cxx/include/libtmux/generated/options.hpp`
- Create: `cxx/include/libtmux/generated/hooks.hpp`
- Create: `cxx/include/libtmux/generated/capabilities.hpp`
- Create: `cxx/include/libtmux/generated/enums.hpp`
- Create: `cxx/tests/unit/generated_metadata_test.cpp`
- Create: `cxx/tests/unit/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`
- Modify: `cxx/tests/CMakeLists.txt`

- [ ] **Step 1: Run the missing metadata generator test**

```console
$ uv run pytest tests/cxx/test_generate_metadata.py -v
```

Expected: FAIL importing `cxx.tools.codegen.generate_metadata`.

- [ ] **Step 2: Freeze package version and ABI namespace configuration**

`cxx/VERSION` contains exactly `0.62.0` and one trailing newline. It is the sole
source for the CMake project version, configured public version constants, and
the later package-version file. `Version.cmake` validates and exposes that
value plus `LIBTMUX_PACKAGE_VERSION_COMPATIBILITY=SameMinorVersion`; later
install rules consume both variables rather than duplicating either policy.

`config.hpp.in` selects `LIBTMUX_CXX_STANDARD`, one token-valued
`LIBTMUX_ABI_NAMESPACE`, and one expected-backend branch. The initial C++23
configuration selects `abi_v23` and `std::expected`; the dormant, separately
built C++20 configuration selects `abi_v20_tl` and the public compatibility
header `<libtmux/compat/expected.hpp>`. Plan 06 supplies that pinned header and
enables the already-defined branch; it does not rewrite a public declaration.
Every public header, including all six generated headers, includes
`<libtmux/config.hpp>` and declares every public type and function inside:

```cpp
namespace libtmux {
inline namespace LIBTMUX_ABI_NAMESPACE {
class Server;
}
}
```

The generator test rejects a public declaration outside that inline namespace,
a generated header that omits the configuration include, and a CMake project
version that differs from `cxx/VERSION`. It scans the complete public include
tree, so the common Python gate repeats this check after every later header
task. It also inspects both configured expected branches and rejects a C++20
branch that includes `<expected>` or a C++23 branch that includes the
compatibility facade.

- [ ] **Step 3: Generate deterministic strongly typed metadata**

Read only the pinned parity observations and literal AST value shapes. Generate
stable field/format/option/hook/enum IDs, value types, operation sets, tmux
tokens, scopes, raw version text, and capability boundaries. The committed JSON
is the language-neutral input; the six headers are derived production output.
`--config-template` defaults to `cxx/include/libtmux/config.hpp.in`, so later
reproducibility checks may omit the argument without selecting another ABI.

- [ ] **Step 4: Prove reproducibility and version boundaries**

Generate twice in distinct temporary directories and require byte-identical
JSON and headers with no timestamp or path. Compile below/at-boundary cases,
including exact raw `3.7`, `3.7a`, and `3.7b`; numeric ordering alone cannot
select capabilities.

```console
$ uv run python cxx/tools/codegen/generate_metadata.py \
    --observations cxx/parity \
    --config-template cxx/include/libtmux/config.hpp.in \
    --output cxx/parity/metadata \
    --headers cxx/include/libtmux/generated \
    --check
```

Expected: exit zero with all six committed headers current.

Rerun the identical focused test after generation:

```console
$ uv run pytest tests/cxx/test_generate_metadata.py -v
```

Expected: all metadata-generator tests pass.

- [ ] **Step 5: Commit generated production inputs**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/VERSION \
    cxx/cmake/Version.cmake \
    cxx/include/libtmux/config.hpp.in \
    cxx/tests/CMakeLists.txt \
    cxx/tests/unit/CMakeLists.txt \
    cxx/tools/codegen \
    tests/cxx/test_generate_metadata.py \
    cxx/parity/metadata \
    cxx/include/libtmux/generated \
    cxx/tests/unit/generated_metadata_test.cpp
```

```console
$ git commit -F - <<'EOF'
CXX(chore[codegen]): Generate API metadata

why: Give parsing, acquisition, queries, options, and hooks one pinned
     schema.

what:
- Fix version 0.62.0 and the configured inline ABI namespace
- Generate typed fields, formats, options, hooks, and capability
  boundaries
- Prove byte-for-byte output and exact tmux version behavior
EOF
```

### Task 5: Create the compiled package skeleton and core values

**Files:**

- Modify: `cxx/CMakeLists.txt`
- Modify: `cxx/CMakePresets.json`
- Create: `cxx/src/CMakeLists.txt`
- Create: `cxx/tests/integration/CMakeLists.txt`
- Create: `cxx/tests/compile/CMakeLists.txt`
- Modify: `cxx/tests/unit/CMakeLists.txt`
- Modify: `cxx/tests/CMakeLists.txt`
- Create: `cxx/cmake/Warnings.cmake`
- Bind: `cxx/VERSION`
- Bind: `cxx/cmake/Version.cmake`
- Bind: `cxx/include/libtmux/config.hpp.in`
- Create: `cxx/include/libtmux/libtmux.hpp`
- Create: `cxx/include/libtmux/error.hpp`
- Create: `cxx/include/libtmux/command.hpp`
- Create: `cxx/include/libtmux/ids.hpp`
- Create: `cxx/include/libtmux/capabilities.hpp`
- Create: `cxx/include/libtmux/version.hpp`
- Create: `cxx/src/error.cpp`
- Create: `cxx/src/command.cpp`
- Create: `cxx/src/ids.cpp`
- Create: `cxx/src/capabilities.cpp`
- Create: `cxx/src/version.cpp`
- Create: `cxx/tests/unit/command_test.cpp`
- Create: `cxx/tests/unit/ids_version_test.cpp`
- Create: `cxx/tests/compile/umbrella_only.cpp`

**Public contract:**

```cpp
namespace libtmux {
inline namespace LIBTMUX_ABI_NAMESPACE {

template<class T>
using result = detail::configured_expected<T, Error>;

struct CommandArgument {
  std::string value;
  bool sensitive{false};
};

class CommandRequest final {
 public:
  static result<CommandRequest> create(
      std::string subcommand,
      std::vector<CommandArgument> arguments = {},
      std::optional<std::chrono::milliseconds> timeout = std::nullopt,
      bool mutating = false);
};

struct Exited { int code; };
struct Signaled { int signal; };
using ProcessTermination = std::variant<Exited, Signaled>;

struct CommandResult {
  std::vector<CommandArgument> argv;
  ProcessTermination termination;
  std::vector<std::string> stdout_lines;
  std::vector<std::string> stderr_lines;
};

enum class DeliveryState { not_dispatched, unknown, applied };

}
}
```

`error.hpp` is the sole expected-selection facade. Its undocumented
`detail::configured_expected<T, E>` alias selects the backend, and the public
surface exposes `result<T>`. Under the configured C++23 branch it includes
`<expected>` and selects `std::expected`; under the dormant C++20 branch it
includes `<libtmux/compat/expected.hpp>` and selects `tl::expected`. No other
public header names either backend directly, and no extra top-level expected
header expands the approved public tree.

`Error` is a closed variant covering validation, spawn, pre-exec, pipe,
timeout, tmux command, protocol, decode, missing object, server connection,
unsupported capability, query parsing, cardinality, and `MutationApplied`.
`MutationApplied` retains the `CommandResult`, optional stable identity,
hydration error, and `DeliveryState::applied`.

- [ ] **Step 1: Add failing value and CMake target tests**

Test invalid empty subcommands, negative timeouts, embedded NUL in command
arguments, ID sigils, malformed IDs, raw `3.7` versus `3.7a`, capability
fingerprints, closed error visitation, umbrella-only header consumption, and
`MutationApplied` delivery.

Register the aggregate target `core_value_contracts` before creating the
production headers, then use the compile/link-red sequence:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target core_value_contracts
```

Expected: FAIL for the missing public core declarations, not for an unknown
target.

- [ ] **Step 2: Configure the single compiled target**

Create `libtmux` and alias `libtmux::libtmux`. Validate
`LIBTMUX_CXX_STANDARD` as exactly `20` or `23`, map `20` to
`target_compile_features(libtmux PUBLIC cxx_std_20)`, and map `23` to
`target_compile_features(libtmux PUBLIC cxx_std_23)`. Set
`CXX_STANDARD_REQUIRED ON` and `CXX_EXTENSIONS OFF`; the default is `23`, while
an explicit `20` must never be promoted to C++23. Keep warning and sanitizer
flags private, and expose only source/build include interfaces.

Read the project version from `cxx/VERSION`, configure `config.hpp` with
`abi_v23`, and compile probes that refer to public declarations through both
`libtmux::Name` and `libtmux::abi_v23::Name`. Assert the active facade resolves
to `std::expected`, the compile command selects the requested language mode,
and dependency scanning sees no compatibility or third-party header. Do not
add install rules yet.

- [ ] **Step 3: Run the focused unit suite**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target core_value_contracts
```

```console
$ ctest \
    --preset cxx-dev \
    -R '^(command|ids|version|error)\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: all value tests pass and no public header needs a third-party include.

- [ ] **Step 4: Commit the package core**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/CMakePresets.json \
    cxx/cmake/Warnings.cmake \
    cxx/include/libtmux \
    cxx/src \
    cxx/tests/CMakeLists.txt \
    cxx/tests/unit \
    cxx/tests/integration/CMakeLists.txt \
    cxx/tests/compile
```

```console
$ git commit -F - <<'EOF'
CXX(feat[core]): Add command value types

why: Establish the stable C++23 ABI and typed recoverable result
     contract.

what:
- Add command, termination, delivery, error, ID, and version values
- Build one compiled target with private first-party options
EOF
```

### Task 6: Write the production POSIX runner

**Files:**

- Create: `cxx/src/detail/process_runner.hpp`
- Create: `cxx/src/process_runner.cpp`
- Bind: `cxx/tests/support/process_probe.cpp`
- Create: `cxx/tests/unit/process_runner_test.cpp`
- Modify: `cxx/src/CMakeLists.txt`
- Modify: `cxx/tests/unit/CMakeLists.txt`

- [ ] **Step 1: Bind retained process acceptance cases to production**

Add launch, pre-exec, nonzero exit, signal, byte preservation, large dual-pipe
output, timeout-before-dispatch, uncertain timeout, escaped pipe holder,
redaction, descriptor, zombie, and teardown cases. Production sources, build
rules, generated inputs, and tests must not include, link, copy, or execute a
deleted spike artifact.

- [ ] **Step 2: Run the process target compile/link red**

Register `process_runner_test` before implementation, then use the
compile/link-red sequence:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target process_runner_test
```

Expected: FAIL because the runner cannot link, not because the target is
unknown.

- [ ] **Step 3: Implement from specification and tests**

Use argv plus `posix_spawnp`, an exec-error channel where required by the
platform path, nonblocking `poll`, separate raw-byte buffers, isolated process
groups, monotonic deadlines, bounded post-kill draining, direct-child reaping,
and typed delivery certainty. Compatibility decoding remains outside the raw
runner.

- [ ] **Step 4: Run sanitizer and leak tests**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target process_runner_test
```

```console
$ ctest --preset cxx-sanitize -R '^process\.' --no-tests=error --output-on-failure
```

Expected: all cases pass without a spike include, source, build, or runtime
dependency.

- [ ] **Step 5: Commit the runner**

```console
$ git add \
    cxx/src/detail/process_runner.hpp \
    cxx/src/process_runner.cpp \
    cxx/src/CMakeLists.txt \
    cxx/tests/unit/process_runner_test.cpp \
    cxx/tests/unit/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(feat[process]): Run argv commands

why: Execute tmux with bounded byte-preserving POSIX process semantics.

what:
- Drain separate pipes and distinguish exit, signal, launch, and timeout
- Isolate, terminate, reap, truncate, and redact without shell execution
EOF
```

### Task 7: Add Server configuration, connection state, and diagnostics

**Files:**

- Create: `cxx/include/libtmux/server.hpp`
- Create: `cxx/src/detail/backend.hpp`
- Create: `cxx/src/detail/connection_state.hpp`
- Create: `cxx/src/connection.cpp`
- Create: `cxx/tests/unit/connection_test.cpp`
- Create: `cxx/tests/integration/connection_tmux_test.cpp`
- Modify: `cxx/src/CMakeLists.txt`
- Modify: `cxx/tests/unit/CMakeLists.txt`
- Modify: `cxx/tests/integration/CMakeLists.txt`

**Public contract:**

Within the configured inline ABI namespace in `server.hpp`:

```cpp
enum class BackendChoice { subprocess };

struct ConnectionConfig {
  std::filesystem::path tmux_binary{"tmux"};
  std::optional<std::string> socket_name;
  std::optional<std::filesystem::path> socket_path;
  std::optional<std::filesystem::path> configuration_file;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment;
  std::optional<std::chrono::milliseconds> default_timeout;
  BackendChoice backend{BackendChoice::subprocess};
  DiagnosticSink diagnostic_sink;
};

struct ServerEndpointKey {
  std::filesystem::path resolved_socket;
};

struct ServerIncarnation {
  ServerEndpointKey endpoint;
  std::uint64_t socket_device;
  std::uint64_t socket_inode;
  std::uint64_t pid;
  std::string start_time;
};
```

- [ ] **Step 1: Write the selected-backend contract red**

Test invalid environment names and embedded NUL before dispatch, independently
created configurations resolving to the same endpoint, same-connection
concurrent reads and mutations, exact result attribution, FIFO diagnostics,
reentrant callback enqueueing, a throwing callback that is caught and disabled,
nonthrowing status inspection, `BackendChoice::subprocess` selection, and no
callback from destructors. Restart a
fixture server on the same resolved socket path and require equal endpoint keys
but different device/inode/pid/start-time incarnations. Register every
concurrent or reentrant case with the `concurrency` CTest label for the separate
TSan workflow.

Register the aggregate target `connection_contracts`, configure, and run its
compile/link red before implementation:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target connection_contracts
```

Expected: FAIL for missing selected-backend definitions, not for an unknown
target.

- [ ] **Step 2: Implement only the selected private ownership mechanism**

Keep the backend base/table/variant entirely in private compiled headers. Run
callbacks after locks are released and make diagnostic failure independent of
operation results. Validate `ConnectionConfig::environment` before connection
state or process construction. Preserve the configured backend choice inside
opaque connection state without adding it to an entity type. `ServerEndpointKey`
canonicalizes only the
resolved socket path; socket-file device/inode and handshake pid/start time are
incarnation data used by child equality, not Server endpoint equality.

- [ ] **Step 3: Run production connection tests**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target connection_contracts
```

```console
$ ctest --preset cxx-sanitize -R '^connection\.' --no-tests=error --output-on-failure
```

```console
$ cmake --preset cxx-tsan
```

```console
$ cmake --build --preset cxx-tsan
```

```console
$ ctest --preset cxx-tsan -L concurrency --no-tests=error --output-on-failure
```

Expected: all unit and real-tmux tests pass, and the connection concurrency
contract reports no data race under standalone TSan.

- [ ] **Step 4: Commit opaque connection state**

```console
$ git add \
    cxx/include/libtmux/server.hpp \
    cxx/src/detail \
    cxx/src/connection.cpp \
    cxx/src/CMakeLists.txt \
    cxx/tests/unit/connection_test.cpp \
    cxx/tests/unit/CMakeLists.txt \
    cxx/tests/integration/connection_tmux_test.cpp \
    cxx/tests/integration/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(feat[connection]): Add sync backend

why: Share thread-safe tmux execution without exposing transport policy.

what:
- Separate endpoint identity from server incarnation
- Add validated configuration and selected private backend ownership
- Contain FIFO diagnostic reentrancy and callback exceptions
EOF
```

### Task 8: Establish complete entity value shells

**Files:**

- Create: `cxx/include/libtmux/session.hpp`
- Create: `cxx/include/libtmux/window.hpp`
- Create: `cxx/include/libtmux/pane.hpp`
- Create: `cxx/include/libtmux/client.hpp`
- Modify: `cxx/include/libtmux/libtmux.hpp`
- Create: `cxx/src/detail/entity_record.hpp`
- Create: `cxx/src/detail/entity_factory.hpp`
- Create: `cxx/src/session.cpp`
- Create: `cxx/src/window.cpp`
- Create: `cxx/src/pane.cpp`
- Create: `cxx/src/client.cpp`
- Create: `cxx/tests/unit/entity_value_shells_test.cpp`
- Create: `cxx/tests/compile/entity_value_shells/`
- Modify: `cxx/src/CMakeLists.txt`
- Modify: `cxx/tests/unit/CMakeLists.txt`
- Modify: `cxx/tests/compile/CMakeLists.txt`

- [ ] **Step 1: Register and build the incomplete-value red**

Positive compile probes return and store each entity by value, instantiate
`std::vector<Session>`, `std::vector<Window>`, `std::vector<Pane>`, and
`std::vector<Client>`, and check copy construction, copy assignment, move
construction, move assignment, and destruction. Negative probes reject public
construction from an unchecked ID or private record handle.

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target entity_value_shells
```

Expected: FAIL because the four complete public value types do not exist.

- [ ] **Step 2: Implement the complete shared value representation**

Each type is an opaque, non-templated, complete value containing shared private
connection state, a validated strong ID, a `ServerIncarnation`, and an immutable
cached-record handle. Only the private acquisition factory constructs values.
Copy, move, assignment, equality, and destruction are final here; destruction
performs no tmux operation. Equal child values require the same canonical
endpoint, socket-file device/inode, handshake pid/start time, and strong tmux
ID. Cached properties, relation accessors, refresh, and mutations are added by
later acquisition and parity tasks without replacing this representation.

- [ ] **Step 3: Run value and equality tests**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target entity_value_shells
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^entity\.value_shells$' \
    --no-tests=error \
    --output-on-failure
```

Expected: values compose in ordinary owning containers, copies retain immutable
records, different incarnations with a reused child ID compare unequal, and no
copy, move, assignment, or destructor dispatches tmux I/O.

- [ ] **Step 4: Commit the prerequisite entity values**

```console
$ git add \
    cxx/include/libtmux/libtmux.hpp \
    cxx/include/libtmux/session.hpp \
    cxx/include/libtmux/window.hpp \
    cxx/include/libtmux/pane.hpp \
    cxx/include/libtmux/client.hpp \
    cxx/src/detail/entity_record.hpp \
    cxx/src/detail/entity_factory.hpp \
    cxx/src/session.cpp \
    cxx/src/window.cpp \
    cxx/src/pane.cpp \
    cxx/src/client.cpp \
    cxx/src/CMakeLists.txt \
    cxx/tests/unit/entity_value_shells_test.cpp \
    cxx/tests/unit/CMakeLists.txt \
    cxx/tests/compile/entity_value_shells \
    cxx/tests/compile/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(feat[entities]): Add value shells

why: Make every entity type complete before snapshot and query contracts
     instantiate it.

what:
- Add opaque Session, Window, Pane, and Client value storage
- Prove incarnation equality and no-side-effect value operations
EOF
```

### Task 9: Parse compatibility rows and immutable relations

**Files:**

- Create: `cxx/src/detail/compat_decode.hpp`
- Create: `cxx/src/compat_decode.cpp`
- Create: `cxx/src/detail/format_parser.hpp`
- Create: `cxx/src/format_parser.cpp`
- Create: `cxx/src/detail/relation_graph.hpp`
- Create: `cxx/src/relation_graph.cpp`
- Create: `cxx/include/libtmux/snapshot.hpp`
- Bind: `cxx/include/libtmux/session.hpp`
- Bind: `cxx/include/libtmux/window.hpp`
- Bind: `cxx/include/libtmux/pane.hpp`
- Bind: `cxx/include/libtmux/client.hpp`
- Modify: `cxx/src/detail/entity_factory.hpp`
- Create: `cxx/tests/unit/compat_decode_test.cpp`
- Create: `cxx/tests/unit/format_parser_test.cpp`
- Create: `cxx/tests/unit/snapshot_test.cpp`
- Modify: `cxx/src/CMakeLists.txt`
- Modify: `cxx/tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write literal parser and snapshot tests**

Cover Python UTF-8/backslash replacement, line normalization, `has-session`,
malformed columns, optional fields, sigiled IDs, winlink edges, loaded empty
relations, table capture provenance, copy/move construction, assignment
invalidation, and unchanged moved-from snapshots.

- [ ] **Step 2: Run the parser/storage target compile/link red**

Register aggregate target `snapshot_contracts` for the three test executables,
then use the compile/link-red sequence:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target snapshot_contracts
```

Expected: FAIL for missing parser and storage symbols, not for an unknown
target.

- [ ] **Step 3: Implement normalized internal records and graph ownership**

Public snapshots own immutable heap-stable row storage and a graph of internal
records keyed by strong IDs. Public entity objects are never stored in the
graph. Iteration materializes the complete Task 8 value shells through the
private factory and exposes const iteration only; it does not redefine an
entity type or its representation.

- [ ] **Step 4: Run lifetime tests under sanitizers**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target snapshot_contracts
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^(compat|format|snapshot)\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: all parser and lifetime tests pass.

- [ ] **Step 5: Commit parsing and snapshots**

```console
$ git add cxx/include/libtmux/snapshot.hpp cxx/src cxx/tests/unit
```

```console
$ git commit -F - <<'EOF'
CXX(feat[snapshot]): Own parsed relations

why: Separate tmux acquisition from stable zero-I/O entity traversal.

what:
- Normalize command bytes into typed rows and winlink edges
- Share immutable heap-stable records and complete relation adjacency
EOF
```

### Task 10: Implement the selected typed query AST

**Files:**

- Create: `cxx/include/libtmux/query/field.hpp`
- Create: `cxx/include/libtmux/query/expr.hpp`
- Create: `cxx/include/libtmux/query/visitor.hpp`
- Bind: `cxx/include/libtmux/generated/fields.hpp`
- Create: `cxx/src/query_evaluate.cpp`
- Create: `cxx/tests/unit/query_expr_test.cpp`
- Bind: `cxx/tests/acceptance/query/`
- Modify: `cxx/src/CMakeLists.txt`
- Modify: `cxx/tests/unit/CMakeLists.txt`
- Modify: `cxx/tests/acceptance/CMakeLists.txt`

- [ ] **Step 1: Run retained query contracts against the empty surface**

Register aggregate target `query_ast_contracts` for the runtime and compile
contract executables, then use the compile/link-red sequence:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target query_ast_contracts
```

Expected: FAIL because the query headers and evaluator are absent; the complete
entity value headers from Task 8 remain unchanged. An unknown target is not the
intended red result.

- [ ] **Step 2: Implement the selected representation**

Use the accepted AST representation and relation layout with no include, source,
generated-input, build, or runtime dependency on deleted spike artifacts. Own
all literals, use closed node variants, preserve short circuiting, implement the
approved lookup semantics, and expose a read-only visitor. To-many relations
provide `any_of`, `all_of`, and `none_of`; to-one relations provide `is`.
`FilterExpr<Entity>::operator()(const Entity&) const` performs local Boolean
evaluation so the value meets `std::predicate` and composes directly with
`std::views::filter`.
Generated field and relation handles are `inline constexpr` objects carrying
type, operation set, stable ID, format token, scope, and minimum capability.
Snapshot filtering evaluates locally; this task adds no tmux pushdown or
hidden acquisition hook. The read-only visitor preserves enough field,
relation, and literal data for a future `Server::query_sessions(expr)` tmux
`-f` translator without adding that API now.

- [ ] **Step 3: Run compile and sanitizer gates**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_ast_contracts
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^query\.(expr|compile|lifetime|relation)\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: valid syntax compiles, invalid syntax fails for the intended reason,
and all lifetime and relation cases pass.

- [ ] **Step 4: Commit the AST**

```console
$ git add \
    cxx/include/libtmux/query \
    cxx/src/query_evaluate.cpp \
    cxx/src/CMakeLists.txt \
    cxx/tests/unit/query_expr_test.cpp \
    cxx/tests/unit/CMakeLists.txt \
    cxx/tests/acceptance/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(feat[query]): Add typed expressions

why: Provide owned composable predicates over immutable entity
     snapshots.

what:
- Implement selected AST storage and generated typed field handles
- Preserve Boolean, relation, lookup, lifetime, and compile-time
  contracts
EOF
```

### Task 11: Add standard-range algorithms and edge parsing

**Files:**

- Create: `cxx/include/libtmux/query/algorithms.hpp`
- Create: `cxx/include/libtmux/query/parser.hpp`
- Create: `cxx/src/query_parser.cpp`
- Create: `cxx/tests/unit/query_algorithms_test.cpp`
- Create: `cxx/tests/unit/query_parser_test.cpp`
- Bind: `cxx/tests/acceptance/compile/query/`
- Modify: `cxx/src/CMakeLists.txt`
- Modify: `cxx/tests/unit/CMakeLists.txt`
- Modify: `cxx/tests/acceptance/CMakeLists.txt`

- [ ] **Step 1: Run cardinality and parser contracts red**

Register aggregate target `query_range_contracts`, then use the
compile/link-red sequence:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target query_range_contracts
```

Expected: FAIL for absent helpers and parser, not for an unknown target.

- [ ] **Step 2: Implement safe reference and owning overloads**

Reference helpers require stable `const T&` forward ranges, accept lvalues and
borrowed rvalues, reject temporary owners, and examine at most two matching
elements. An lvalue owning helper participates only when its value type is
constructible from `range_reference_t`; it constructs from `*iterator` and
never moves from the source. An rvalue owning helper participates only when its
value type is constructible from `range_rvalue_reference_t`; it consumes the
selected element with `std::ranges::iter_move`. For a single-pass range,
`exactly_one_value` must materialize the first value before probing the second:
an lvalue is copied, while an rvalue may be left with its first element moved
from even when the result is `multiple`. This documented consumption is tested.

Retained positive compile probes cover copyable single-pass lvalues,
materializable proxy references, and rvalue single-pass ranges of move-only
values. Negative probes reject move-only lvalues and proxies whose value cannot
be constructed from the relevant reference category. Runtime counters prove
lvalue copy and rvalue move behavior. `CardinalityError` distinguishes
`no_match` and `multiple`.

- [ ] **Step 3: Implement the compatibility edge parser**

Split on the first `=`, resolve only the final `__` suffix through generated
metadata, own the right-hand side, and preserve explicit empty strings.
Missing `=`, empty field, unknown field/suffix, and repeated malformed suffixes
return typed parse errors. A golden parses `name__contains=libtmux`; lookup
strings remain confined to this compatibility edge parser and do not become
entity-method parameters or a second query API.

- [ ] **Step 4: Run all focused tests and commit**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_range_contracts
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^query\.(ranges|cardinality|parser)\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: all range, single-pass, compile-fail, and parser cases pass.

```console
$ git add \
    cxx/include/libtmux/query \
    cxx/src/query_parser.cpp \
    cxx/src/CMakeLists.txt \
    cxx/tests/unit/query_algorithms_test.cpp \
    cxx/tests/unit/query_parser_test.cpp \
    cxx/tests/unit/CMakeLists.txt \
    cxx/tests/acceptance/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(feat[query]): Add range algorithms

why: Compose with standard ranges without unsafe reference lifetimes.

what:
- Add reference-safe and owning cardinality helpers
- Parse explicit edge lookup strings through generated metadata
EOF
```

### Task 12: Lower ASTs through the opt-in serializer seam

**Files:**

- Create: `cxx/include/libtmux/serialization/query_json.hpp`
- Create: `cxx/tests/unit/query_serialization_test.cpp`
- Bind: `cxx/schema/filter-expression-v1.schema.json`
- Bind: `cxx/tests/data/query/filter-expression-events-v1.json`
- Modify: `cxx/tests/unit/CMakeLists.txt`

- [ ] **Step 1: Run serializer event and failure tests red**

Register `query_serialization_test`, then use the compile/link-red sequence:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target query_serialization_test
```

Expected: FAIL because the opt-in encoder is absent, not because the target is
unknown.

- [ ] **Step 2: Implement a serializer concept, not a JSON dependency**

Walk the read-only AST visitor and emit schema-version, entity, node, field,
relation, scalar, and regex events to a caller-supplied serializer. Return the
serializer's own result type. Do not add a decoder or round-trip claim.

- [ ] **Step 3: Validate schema and golden events**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target query_serialization_test
```

```console
$ ctest \
    --preset cxx-dev \
    -R '^query\.serialization\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: every node matches schema v1 events, unknown versions fail in the
cross-language oracle, and injected serializer errors propagate unchanged.

- [ ] **Step 4: Commit the serializer seam**

```console
$ git add \
    cxx/include/libtmux/serialization/query_json.hpp \
    cxx/tests/unit/query_serialization_test.cpp \
    cxx/tests/unit/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(feat[query]): Lower versioned events

why: Support canonical query JSON without adding a core JSON dependency.

what:
- Expose schema-versioned AST lowering through a serializer concept
- Verify event goldens, unknown versions, and serializer failures
EOF
```

### Task 13: Complete the production acquisition vertical slice

**Files:**

- Modify: `cxx/include/libtmux/server.hpp`
- Modify: `cxx/include/libtmux/session.hpp`
- Create: `cxx/src/server.cpp`
- Modify: `cxx/src/session.cpp`
- Modify: `cxx/src/detail/entity_factory.hpp`
- Create: `cxx/src/acquisition.cpp`
- Create: `cxx/tests/integration/server_session_test.cpp`
- Create: `cxx/tests/differential/cpp_adapter.cpp`
- Modify: `cxx/tests/differential/CMakeLists.txt`
- Modify: `cxx/tests/differential/scenario_registry.json`
- Modify: `cxx/tools/differential/python_reference.py`
- Modify: `cxx/src/CMakeLists.txt`
- Modify: `cxx/tests/integration/CMakeLists.txt`
- Create: `tests/cxx/test_server_session_differential.py`

**Public slice:** `Server::create`, `cmd`, `new_session`, lenient `sessions`,
checked `sessions_checked`, `is_alive`, `check_alive`, endpoint equality,
`Session` cached name and refresh on the existing identity/equality value, and
no-side-effect destruction.

- [ ] **Step 1: Run the retained vertical and differential tests red**

Register aggregate target `server_session_vertical_contracts`, then use the
compile/link-red sequence:

```console
$ uv run pytest tests/cxx/test_server_session_differential.py -v
```

Expected: FAIL because the C++ differential adapter does not implement the
server/session lifecycle. An unrelated fixture or collection failure is not the
intended Python red.

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target server_session_vertical_contracts
```

Expected: FAIL because acquisition and hydration methods are absent; the
complete Server configuration and Session value type already compile. An
unknown target is not the intended red result.

- [ ] **Step 2: Implement complete capture before return**

Acquire required rows and relation enrichment before exposing a snapshot or
entity. Lenient access returns an empty snapshot on every acquisition error.
Checked access preserves base-command and enrichment evidence. Post-mutation
hydration failure returns `MutationApplied` with identity when known.

- [ ] **Step 3: Implement endpoint and incarnation equality**

`Server` compares only the canonical resolved socket path, independent of
binary, configuration, selector form, or server incarnation. Test `-L` and
resolved `-S` handles to one live server, then restart tmux on the same path:
the Server values remain endpoint-equal, while an old Session and a new Session
with a reused strong ID compare unequal because device/inode/pid/start time
differ. Refresh replaces only the receiver after full successful capture.

- [ ] **Step 4: Run real-tmux and differential gates**

The Python supervisor validates lifecycle JSON and sends only the retained
version-one binary frames to `cpp_adapter`; the adapter emits framed typed
observations. It does not parse or generate JSON and links no JSON library.
Register target `libtmux_differential_adapter` with output name `cpp_adapter`
under `${CMAKE_BINARY_DIR}/tests/differential`; its test-only
`--list-operations` mode emits operation tags and the registry digest through
the same binary frames.

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target server_session_vertical_contracts
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^(transport\.production|differential\.server_session)' \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/test_server_session_differential.py -v
```

Expected: the Python and C++ canonical scenario records match, including argv,
return shapes, and errors.

- [ ] **Step 5: Commit the vertical slice**

```console
$ git add \
    cxx/include/libtmux/server.hpp \
    cxx/include/libtmux/session.hpp \
    cxx/src \
    cxx/tests/integration \
    cxx/tests/differential \
    cxx/tools/differential/python_reference.py \
    tests/cxx/test_server_session_differential.py
```

```console
$ git commit -F - <<'EOF'
CXX(feat[server]): Add acquisition slice

why: Prove the rewritten transport and query core against real tmux
     behavior.

what:
- Add endpoint-aware Server and relation-complete Session snapshots
- Match the first Python differential lifecycle scenario
EOF
```

### Task 14: Audit the clean rewrite before parity expansion

**Files:**

- Extend: `cxx/tools/bakeoff/verify_rewrite_boundary.py`
- Bind: `cxx/docs/evidence/rewrite-boundary.json`
- Create: `cxx/docs/evidence/clean-rewrite.json`
- Create: `cxx/docs/reviews/clean-rewrite.md`
- Create: `tests/cxx/test_clean_rewrite_audit.py`

- [ ] **Step 1: Add provenance and boundary failures**

The audit rejects spike paths, candidate names, symlinks, submodules, vendored
binaries, untracked generated sources, experimental include directories,
production CMake references to deleted paths, generated inputs derived from
deleted paths, and runtime loading of a spike artifact. It records the current
source identity plus the frozen decision, contract, and generator-input digests.
It requires those dependencies and provenance records to be closed and the
frozen contract hash to remain unchanged.

```console
$ uv run pytest tests/cxx/test_clean_rewrite_audit.py -v
```

Expected: FAIL because the production audit and evidence fixture do not exist.
An unrelated fixture or collection failure is not the intended red result.

The extended verifier accepts `--spike-baseline` and validates its recorded
commit, tree, blob IDs, modes, and complete path set through Git object APIs. It
rejects an exact production blob match and any `C` or `R` record emitted by
`git diff --find-copies-harder --find-renames=50% --diff-filter=CR` between the
recorded revision and the path set containing `cxx/spikes/`, current
`cxx/include/`, `cxx/src/`, `cxx/cmake/`, and the root CMake source.
Disposable-repository tests cover an exact copy, exact rename, and an edited
copy above the fixed similarity threshold, plus a forged or unreachable
baseline. It does not claim or attempt to prove that an implementer never
inspected repository history; human reading history is not a mechanically
enforceable production boundary.

- [ ] **Step 2: Build twice out of tree**

Extend the verifier with `--two-builds`. It creates two directories with
`tempfile.TemporaryDirectory`, configures each with Ninja, builds, runs CTest,
and compares generated headers. For each build it captures the registered-test
inventory with `ctest --show-only=json-v1` and a fresh JUnit result with
`--output-junit`; it records normalized digests rather than reading the mutable
CTest `Testing/` directory. Public evidence records only symbolic labels.

Before either build, the verifier captures Git status as its baseline. It writes
the declared `--output` only after both builds and the post-run status check.
The check requires status to equal the baseline after excluding exactly that
declared output path; no caller-supplied exclusion or other changed/untracked
path is allowed.

```console
$ uv run python cxx/tools/bakeoff/verify_rewrite_boundary.py \
    --production \
    --two-builds \
    --manifest cxx/tests/acceptance/public_contract.json \
    --decision cxx/docs/bakeoffs/architecture-decision.json \
    --spike-baseline cxx/docs/evidence/rewrite-boundary.json \
    --output cxx/docs/evidence/clean-rewrite.json
```

Expected: both builds and CTests pass, generated headers and normalized test
inventories match, no production blob has exact or Git-detected copy/rename
provenance from a tracked spike implementation, and no path other than the
intended evidence output differs from the captured checkout baseline.

- [ ] **Step 3: Obtain an independent clean-rewrite review**

The reviewer checks every production line for ownership, UB, exception safety,
process correctness, synchronization, hidden I/O, public templates, dependency
leakage, and unnecessary surface area. Fix every finding before closing the
report.

Commit each review-driven production, test, or CMake fix as a separate
issue-family change before closing the report. After the last fix, repeat Step 2
in full so both clean builds, fresh CTest inventories, JUnit results, generated
headers, provenance scan, and `clean-rewrite.json` bind the reviewed current
source. Before Step 4, status may contain only the Task 14 audit tool, focused
test, regenerated evidence, and review report; an unstaged source or
registration fix blocks closeout.

- [ ] **Step 4: Run and commit the audit**

```console
$ uv run pytest tests/cxx/test_clean_rewrite_audit.py -v
```

Expected: exit zero with an unchanged contract, no spike dependency, two
reproducible builds, immutable test-result digests, and no checkout change
beyond the declared evidence output.

```console
$ git add \
    cxx/tools/bakeoff/verify_rewrite_boundary.py \
    tests/cxx/test_clean_rewrite_audit.py \
    cxx/docs/evidence/clean-rewrite.json \
    cxx/docs/reviews/clean-rewrite.md
```

```console
$ git commit -F - <<'EOF'
CXX(test[rewrite]): Prove clean core

why: Close the rewrite boundary before expanding the public API.

what:
- Verify production provenance, retained contracts, and reproducible
  builds
- Close an independent ownership, process, query, and scope review
EOF
```
