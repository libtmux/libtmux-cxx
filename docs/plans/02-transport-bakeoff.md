# Transport bakeoff implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Select the private synchronous transport ownership mechanism and
prove the process, concurrency, diagnostic, control-mode, and engine-adapter
contracts without exposing transport policy in public entity types.

**Architecture:** One tested POSIX kernel and one retained black-box contract
exercise three separately linked contenders: an abstract backend, a manual
function table, and a closed variant. Contender sources remain under
`cxx/spikes/transport/`; normalized reports and reusable acceptance tests live
outside that disposable subtree.

**Tech Stack:** C++23, POSIX `posix_spawnp`/`poll`/`waitpid`, GoogleTest 1.17.0,
real tmux, ASan, UBSan, TSan, CMake, Python measurement and evidence tooling,
control mode, and the pinned engine-ops source named by the approved design.

## Global Constraints

- All three contenders expose the same exercise header and namespace from
  different include directories and link into different executables. They are
  never linked into one binary.
- Only private backend ownership changes between contenders. The POSIX runner,
  semantic expectations, workload, compiler flags, and measurements are
  identical.
- Public virtual bases, templated entity policies, shell execution, and an
  executor member on entity values are disqualifying.
- A successful process launch returns a command result even when tmux exits
  nonzero. Spawn, pre-exec, pipe, timeout, protocol, and decode failures remain
  distinct.
- Independent requests are not tmux semicolon groups. Group attribution uses
  only `exact`, `skipped`, or `unknown`; a missing block cannot be fabricated.
- Raw timing traces and build products remain ignored. Retained JSON contains
  repository-relative paths, immutable source identities, and no hostname.
- Each contender must pass hard gates before its measurements are considered.

## Retained and Disposable Paths

Retain these artifacts through the production rewrite:

```text
cxx/tests/contracts/transport/
cxx/tests/data/transport/
cxx/docs/bakeoffs/transport/
cxx/docs/bakeoffs/grafts/
```

Delete these implementations before production work:

```text
cxx/spikes/transport/
cxx/spikes/grafts/control_mode/
cxx/spikes/grafts/engine_ops/
```

### Task 1: Prove the shared POSIX process kernel

**Files:**

- Create: `cxx/spikes/transport/kernel/include/transport/process.hpp`
- Create: `cxx/spikes/transport/kernel/src/process.cpp`
- Create: `cxx/tests/contracts/transport/process_contract.cpp`
- Create: `cxx/tests/support/process_probe.cpp`
- Create: `cxx/tests/data/transport/process-goldens.json`
- Create: `cxx/spikes/transport/kernel/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`

**Interfaces:**

```cpp
namespace libtmux::spike {

enum class Sensitivity { public_value, secret };
enum class DispatchPhase { not_dispatched, dispatch_uncertain };
enum class StdioPolicy { capture, inherit_terminal };

struct Argument {
  std::string value;
  Sensitivity sensitivity{Sensitivity::public_value};
};

struct Exited { int code; };
struct Signaled { int signal; };
using Termination = std::variant<Exited, Signaled>;

struct ProcessRequest {
  std::filesystem::path executable;
  std::vector<Argument> arguments;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment;
  std::chrono::milliseconds timeout;
  StdioPolicy stdio{StdioPolicy::capture};
};

struct ProcessReply {
  Termination termination;
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool output_truncated;
};

struct ProcessError {
  enum class Kind { spawn, pre_exec, pipe, timeout } kind;
  DispatchPhase dispatch_phase;
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool output_truncated;
};

std::expected<ProcessReply, ProcessError> run_posix(
    const ProcessRequest& request);

}  // namespace libtmux::spike
```

- [ ] **Step 1: Write the failure-first process tests**

Tests cover an absent executable, an executable file with invalid contents,
normal exit, nonzero exit, signal termination, embedded non-UTF-8 bytes,
simultaneous stdout/stderr larger than both pipe buffers, a deadline, a
TERM-resistant child, and an escaped descendant retaining a pipe descriptor.
The last three use 10-second outer CTest timeouts so a broken runner fails
boundedly.

Validation tests reject embedded NUL in the executable, any argument, or an
environment value before dispatch. They also reject empty environment names,
names containing `=`, and names containing NUL, while allowing an explicit
unset value.

The retained contract also allocates a test PTY, runs one bounded inherited-
terminal child, detaches it, and verifies that capture pipes were not installed.
Inherited mode returns no captured bytes and fails validation without a
terminal; capture remains the default for every ordinary tmux command.

- [ ] **Step 2: Run the focused test and observe missing symbols**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target transport_process_test
```

Expected: FAIL because `run_posix` is undefined.

- [ ] **Step 3: Implement the minimal byte-preserving runner**

Use argv execution without a shell, an exec-error pipe, nonblocking stdout and
stderr pipes, `poll` against one absolute `steady_clock` deadline, a new process
group, and direct-child `waitpid`. On timeout, send TERM and then KILL to the
owned group, bound the post-kill drain separately, close escaped descriptors,
mark truncation, and reap the direct child.

- [ ] **Step 4: Add redaction and descriptor-leak assertions**

The test renders a request containing `Argument{"token", Sensitivity::secret}`
and asserts that diagnostics contain `[REDACTED]` but never `token`. Run 128
short child processes and compare open descriptors before and after.

All detailed cases live in retained `process_contract.cpp`, `process_probe.cpp`,
and `process-goldens.json`. The spike kernel only binds that contract and owns
no test source that will disappear with `cxx/spikes/`.

- [ ] **Step 5: Run normal and sanitizer gates**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target transport_process_test
```

```console
$ ctest --preset cxx-dev -R '^transport\.process\.' --no-tests=error --output-on-failure
```

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target transport_process_test
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^transport\.process\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: every process test passes without a hang, zombie, leak, or secret in
diagnostics.

- [ ] **Step 6: Commit the process kernel**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/spikes/transport/kernel \
    cxx/tests/contracts/transport/process_contract.cpp \
    cxx/tests/support/process_probe.cpp \
    cxx/tests/data/transport/process-goldens.json
```

```console
$ git commit -F - <<'EOF'
CXX(spike[runner]): Prove POSIX semantics

why: Give every transport contender identical process behavior and
     failures.

what:
- Preserve process bytes and separate termination from launch failures
- Bound pipe draining, process-group teardown, reaping, and redaction
EOF
```

### Task 2: Freeze the shared transport exercise

**Files:**

- Create: `cxx/tests/contracts/transport/exercise.hpp`
- Create: `cxx/tests/contracts/transport/exercise.cpp`
- Create: `cxx/tests/contracts/transport/harness_self_test.cpp`
- Create: `cxx/tests/contracts/transport/CMakeLists.txt`
- Create: `cxx/tests/contracts/transport/vertical_slice.cpp`
- Create: `cxx/tests/data/transport/expected-errors.json`
- Create: `cxx/spikes/transport/common/include/libtmux_spike/transport.hpp`
- Create: `cxx/spikes/transport/common/src/transport.cpp`
- Create: `cxx/spikes/transport/common/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`
- Modify: `cxx/tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace libtmux::spike {

enum class DeliveryState { not_dispatched, unknown, applied };
enum class Attribution { exact, skipped, unknown };
enum class BackendChoice { subprocess };

struct CommandRequest {
  std::string subcommand;
  std::vector<Argument> arguments;
  std::optional<std::chrono::milliseconds> timeout;
  bool mutating;
};

struct CommandResult {
  std::vector<Argument> argv;
  Termination termination;
  std::vector<std::string> stdout_lines;
  std::vector<std::string> stderr_lines;
};

struct ConnectionConfig {
  std::filesystem::path tmux_binary{"tmux"};
  std::optional<std::string> socket_name;
  std::optional<std::filesystem::path> socket_path;
  std::optional<std::filesystem::path> configuration_file;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment;
  std::optional<std::chrono::milliseconds> default_timeout;
  BackendChoice backend{BackendChoice::subprocess};
};

struct NewSessionRequest { std::string name; };
struct SessionRow { std::string id; std::string name; };
using SessionRows = std::vector<SessionRow>;

struct DiagnosticStatus {
  bool sink_enabled;
  std::optional<std::string> failure;
};

struct MutationApplied {
  CommandResult command;
  std::optional<std::string> stable_identity;
  std::string hydration_error;
  DeliveryState delivery{DeliveryState::applied};
};

struct RequestValidationError { std::string message; };
struct ProcessFailureEvidence {
  DeliveryState delivery;
  std::vector<std::byte> stdout_bytes;
  std::vector<std::byte> stderr_bytes;
  bool output_truncated;
};
struct SpawnError { std::string message; ProcessFailureEvidence evidence; };
struct PreExecError { std::string message; ProcessFailureEvidence evidence; };
struct PipeError { std::string message; ProcessFailureEvidence evidence; };
struct TimeoutError { std::string message; ProcessFailureEvidence evidence; };
struct TmuxCommandError {
  CommandResult command;
  DeliveryState delivery;
};
struct ProtocolError { std::string message; };
struct DecodeError { std::string message; };

using TransportError = std::variant<
    RequestValidationError,
    SpawnError,
    PreExecError,
    PipeError,
    TimeoutError,
    TmuxCommandError,
    ProtocolError,
    DecodeError,
    MutationApplied>;

template<class Candidate>
concept TransportCandidate = std::copy_constructible<Candidate> && requires(
    Candidate candidate,
    const Candidate& connection,
    ConnectionConfig config,
    CommandRequest request,
    std::function<void(std::string_view)> sink) {
  { Candidate::create(std::move(config)) }
      -> std::same_as<std::expected<Candidate, TransportError>>;
  { connection.execute(request) }
      -> std::same_as<std::expected<CommandResult, TransportError>>;
  { connection.new_session(NewSessionRequest{"contract"}) }
      -> std::same_as<std::expected<SessionRow, TransportError>>;
  { connection.sessions() } -> std::same_as<SessionRows>;
  { connection.sessions_checked() }
      -> std::same_as<std::expected<SessionRows, TransportError>>;
  { connection.is_alive() } -> std::same_as<bool>;
  { candidate.set_diagnostic_sink(std::move(sink)) } -> std::same_as<void>;
  { connection.diagnostic_status() } -> std::same_as<DiagnosticStatus>;
};

template<TransportCandidate Candidate>
void register_transport_contract(std::string_view candidate_name);

}  // namespace libtmux::spike
```

- [ ] **Step 1: Register a failing retained-harness self-test**

`harness_self_test.cpp` calls the non-template registration metadata in
`exercise.cpp`. It requires unique case IDs, deterministic candidate prefixes,
the `transport` label on every runtime case, the additional `concurrency` label
on attribution, reentrancy, and destruction cases, and the downstream vertical
slice in the same candidate executable. Add
`transport_contract_harness_test` and CTest name `transport.contract.harness`
before defining those metadata functions.

- [ ] **Step 2: Configure and observe the harness link failure**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target transport_contract_harness_test
```

Expected: FAIL at link time for the missing retained-harness metadata symbols;
an unknown target or empty CTest selection is not the intended red result.

- [ ] **Step 3: Implement the common typed contract before any contender**

The contract uses `ScopedTmuxServer` to create, list, and kill a session. For a
successfully spawned `list-sessions` command that makes tmux exit nonzero, raw
`execute` returns `CommandResult`, the lenient `sessions` accessor returns an
empty `SessionRows`, and `sessions_checked` returns `TmuxCommandError` retaining
the same argv, termination, normalized stdout/stderr evidence, and honest
delivery state. A successfully spawned `new-session` that exits nonzero makes
`new_session` return the same error leaf rather than a process failure or a
successful row. Neither checked path may assume that a tmux error proves the
operation was not dispatched. None of those paths may misclassify the result as
a spawn, pre-exec, pipe, protocol, or decode failure. The contract also tests
forced post-mutation hydration failure with stable identity and
`DeliveryState::applied`, concurrent request attribution, FIFO diagnostics,
reentrant diagnostics, a throwing sink, and destruction under contention.

Register concurrent attribution, reentrancy, and destruction cases with both
the `transport` and `concurrency` CTest labels so the later standalone TSan
preset runs the same contract.

Define `register_transport_contract<Candidate>` inline in `exercise.hpp`; only
non-template case metadata and shared test bodies belong in `exercise.cpp`, so
candidate types introduced by later binding translation units can instantiate
the contract without missing template definitions.

Add the basic downstream consumer source. `vertical_slice.cpp` includes only
the contender's public exercise header, creates a connection, creates a session,
obtains an immutable row list, applies a local name predicate, and checks zero
or one result. It cannot include a backend class or the POSIX kernel. Each
contender maps kernel-private `ProcessError` values into the public transport
leaf errors above. The harness verifies that `BackendChoice::subprocess` reaches
the candidate unchanged; the enum never names an ownership contender.

- [ ] **Step 4: Build and run the retained harness green**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target transport_contract_harness_test
```

```console
$ ctest \
    --preset cxx-dev \
    -R '^transport\.contract\.harness$' \
    --no-tests=error \
    --output-on-failure
```

Expected: the concrete harness target builds, the self-test passes, and no
contender target exists yet.

- [ ] **Step 5: Commit the retained exercise**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/tests/CMakeLists.txt \
    cxx/tests/contracts/transport \
    cxx/tests/data/transport \
    cxx/spikes/transport/common
```

```console
$ git commit -F - <<'EOF'
CXX(test[transport]): Freeze backend contract

why: Compare transport ownership through one public exercise and
     workload.

what:
- Define process, diagnostic, concurrency, and mutation expectations
- Add a backend-blind real-tmux downstream vertical slice
EOF
```

### Task 3: Implement the abstract-backend contender

**Files:**

- Create: `cxx/spikes/transport/abstract_backend/include/libtmux_spike/server.hpp`
- Create: `cxx/spikes/transport/abstract_backend/src/server.cpp`
- Create: `cxx/spikes/transport/abstract_backend/src/backend.hpp`
- Create: `cxx/spikes/transport/abstract_backend/tests/binding.cpp`
- Create: `cxx/spikes/transport/abstract_backend/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`

**Private mechanism:** `ServerState` owns `std::unique_ptr<Backend>`, where
`Backend` is a private abstract base with a virtual destructor and one private
`execute` operation. No public header declares or names it.

- [ ] **Step 1: Bind the contender to the common exercise**

The binding asserts `TransportCandidate<AbstractCandidate>` and registers the
shared contract under the prefix `transport.abstract`. Register
`transport_contract_abstract` in the root CMake file before implementing its
backend methods.

- [ ] **Step 2: Build the registered contender and observe missing definitions**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target transport_contract_abstract
```

Expected: FAIL at compile or link time for the registered candidate's missing
backend definitions, not for an unknown target.

- [ ] **Step 3: Implement only the abstract contender**

Use the shared process kernel. Protect mutable connection state with a mutex,
release all locks before diagnostic delivery, and use a per-connection FIFO
with at most one active drain. Catch sink exceptions, disable that sink, and
retain a nonthrowing failure status.

- [ ] **Step 4: Run common, sanitizer, and downstream tests**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target transport_contract_abstract
```

```console
$ ctest \
    --preset cxx-dev \
    -R '^transport\.abstract\.' \
    --no-tests=error \
    --output-on-failure
```

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target transport_contract_abstract
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^transport\.abstract\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: all abstract-backend tests pass.

- [ ] **Step 5: Commit the contender**

```console
$ git add cxx/spikes/transport/abstract_backend cxx/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(spike[transport]): Try abstract backend

why: Measure conventional private polymorphism against the shared
     contract.

what:
- Implement private virtual backend ownership behind opaque state
- Pass the real-tmux, concurrency, diagnostic, and consumer exercise
EOF
```

### Task 4: Implement the manual function-table contender

**Files:**

- Create: `cxx/spikes/transport/function_table/include/libtmux_spike/server.hpp`
- Create: `cxx/spikes/transport/function_table/src/server.cpp`
- Create: `cxx/spikes/transport/function_table/src/backend_box.hpp`
- Create: `cxx/spikes/transport/function_table/tests/binding.cpp`
- Create: `cxx/spikes/transport/function_table/tests/ownership_test.cpp`
- Create: `cxx/spikes/transport/function_table/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`

**Private mechanism:** An owned `void*` and a private table of `destroy`,
`move`, and `execute` function pointers. The box is move-only; destruction and
replacement are idempotent from the owner's perspective.

- [ ] **Step 1: Add failing lifetime controls**

Count construction and destruction of a recording backend. Test move
construction, move assignment over an occupied box, self-move protection,
replacement, and destruction after a failed execute.

- [ ] **Step 2: Run the registered target compile/link red**

Register `transport_contract_function_table` before implementation, then use
the compile/link-red sequence:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target transport_contract_function_table
```

Expected: FAIL for missing function-table ownership and execution definitions,
not for an unknown target.

- [ ] **Step 3: Implement the contender**

The candidate uses the same kernel, state synchronization, diagnostic queue,
and binding source shape as the abstract contender. Only backend storage and
dispatch differ.

- [ ] **Step 4: Build and run the identical contract green**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target transport_contract_function_table
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^transport\.function_table\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: all function-table ownership, transport, concurrency, and downstream
contract cases pass.

- [ ] **Step 5: Commit the contender**

```console
$ git add cxx/spikes/transport/function_table cxx/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(spike[transport]): Try function table

why: Test manual type erasure without changing public transport
     semantics.

what:
- Add private owned function-table dispatch and lifetime tests
- Pass the same real-tmux and downstream contract as other contenders
EOF
```

### Task 5: Implement the closed-variant contender

**Files:**

- Create: `cxx/spikes/transport/closed_variant/include/libtmux_spike/server.hpp`
- Create: `cxx/spikes/transport/closed_variant/src/server.cpp`
- Create: `cxx/spikes/transport/closed_variant/src/backend_variant.hpp`
- Create: `cxx/spikes/transport/closed_variant/tests/binding.cpp`
- Create: `cxx/spikes/transport/closed_variant/tests/extensibility_test.cpp`
- Create: `cxx/spikes/transport/closed_variant/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`

**Private mechanism:** `std::variant<SubprocessBackend, RecordingBackend>` is a
private member of opaque state. Visitors are exhaustive and never enter a
public header.

- [ ] **Step 1: Add a recording-backend compile and behavior test**

The test adds a `RecordingBackend`, executes two requests, and observes owned
copies. A public-header scan rejects `std::variant`, `SubprocessBackend`, and
`RecordingBackend` spellings.

- [ ] **Step 2: Run the registered target compile/link red**

Register `transport_contract_closed_variant` before implementation, then use
the compile/link-red sequence:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target transport_contract_closed_variant
```

Expected: FAIL for missing visitor alternatives and candidate definitions, not
for an unknown target.

- [ ] **Step 3: Implement the contender**

Use exhaustive private visitors, the shared process kernel, and the same
connection-state, diagnostic, and synchronization contracts as the other
contenders.

- [ ] **Step 4: Build and pass the shared suite**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target transport_contract_closed_variant
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^transport\.closed_variant\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: all variant tests pass and the public-header scan is empty.

- [ ] **Step 5: Commit the contender**

```console
$ git add cxx/spikes/transport/closed_variant cxx/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(spike[transport]): Try closed variant

why: Measure closed private alternatives with compiler-checked dispatch.

what:
- Implement variant-backed opaque connection state
- Test recording-backend extension and public-header isolation
EOF
```

### Task 6: Probe control-mode framing and failure attribution

**Files:**

- Create: `cxx/spikes/grafts/control_mode/include/control_mode/parser.hpp`
- Create: `cxx/spikes/grafts/control_mode/src/parser.cpp`
- Create: `cxx/spikes/grafts/control_mode/src/connection.cpp`
- Create: `cxx/spikes/grafts/control_mode/tests/parser_test.cpp`
- Create: `cxx/spikes/grafts/control_mode/tests/integration_test.cpp`
- Create: `cxx/spikes/grafts/control_mode/CMakeLists.txt`
- Create: `cxx/docs/bakeoffs/grafts/control-mode.json`
- Create: `cxx/docs/bakeoffs/grafts/control-mode.md`
- Modify: `cxx/CMakeLists.txt`

**Interfaces:**

```cpp
namespace libtmux::spike::control {

enum class ControlTerminal { end, error };
struct ControlBlock {
  std::uint64_t sequence;
  std::uint64_t command_number;
  ControlTerminal terminal;
  std::vector<std::byte> begin_metadata;
  std::vector<std::byte> terminal_metadata;
  std::vector<std::byte> body;
};
struct Notification { std::vector<std::byte> body; };
using Event = std::variant<ControlBlock, Notification>;

class Parser final {
 public:
  std::expected<std::vector<Event>, ProtocolError> feed(
      std::span<const std::byte> bytes);
  std::expected<void, ProtocolError> finish();
};

}  // namespace libtmux::spike::control
```

- [ ] **Step 1: Register split-frame and octal-escape parser tests**

Feed every golden stream at every byte boundary. Test notifications between
`%begin` and `%end`, `%error` termination, command-number and begin-frame
metadata, exact `%end`/`%error` terminal metadata, malformed boundaries,
incomplete EOF, and raw bytes. An error block must never be normalized into a
successful end block, and terminal metadata must not be folded into the body or
discarded.

Create and register aggregate target `graft_control_mode_contracts` plus the
`graft.control.*` CTest cases before defining `Parser` or the persistent
connection. The target includes both parser and real-tmux integration tests.

- [ ] **Step 2: Reproduce the fail-fast semicolon group**

Send a three-command group whose first command fails. The test has a two-second
deadline and requires the remaining group operations to become `skipped` when
deletion is known or `unknown` when attribution is ambiguous. It must never
wait for three result blocks.

- [ ] **Step 3: Test independent requests separately**

Interleave two independent requests and notifications through a persistent
`tmux -C` process. Assert exact reply ownership and deterministic shutdown.

- [ ] **Step 4: Run the registered graft compile/link red**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target graft_control_mode_contracts
```

Expected: FAIL for missing `Parser::feed`, `Parser::finish`, or persistent
connection definitions, not for an unknown target, missing source, or unrelated
infrastructure error.

- [ ] **Step 5: Implement the parser and persistent connection**

Implement only the byte-preserving framed parser and connection behavior needed
by the registered tests. Serialize frame writes, demultiplex command replies from
notifications, preserve begin and terminal metadata, terminate fail-fast groups
without waiting for deleted replies, and bound shutdown.

- [ ] **Step 6: Rebuild the target and record the strict gate**

`control-mode.json` records the explicit tmux binary SHA-256, raw `tmux -V`,
socket mode, golden-stream IDs, fail-fast group results, and the immutable
registration/JUnit gate digest. It contains no executable or socket path.

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target graft_control_mode_contracts
```

```console
$ uv run python -m cxx.tools.evidence.ctest_gate \
    --source-dir cxx \
    --preset cxx-sanitize \
    --match '^graft\.control\.' \
    --gate-id graft-control-sanitize \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/graft-control-sanitize.json
```

Expected: all parser and real-tmux tests pass without a timeout.

- [ ] **Step 7: Commit the probe and report**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/spikes/grafts/control_mode \
    cxx/docs/bakeoffs/grafts/control-mode.json \
    cxx/docs/bakeoffs/grafts/control-mode.md
```

```console
$ git commit -F - <<'EOF'
CXX(spike[control]): Probe backend graft

why: Prove persistent control mode can report fail-fast groups honestly.

what:
- Parse byte-split control frames and interleaved notifications
- Distinguish exact, skipped, and unknown command attribution
EOF
```

### Task 7: Probe the engine-ops adapter boundary

**Files:**

- Create: `cxx/spikes/grafts/engine_ops/include/engine_adapter/model.hpp`
- Create: `cxx/spikes/grafts/engine_ops/src/lower.cpp`
- Create: `cxx/spikes/grafts/engine_ops/src/execute.cpp`
- Create: `cxx/spikes/grafts/engine_ops/tests/adapter_test.cpp`
- Create: `cxx/spikes/grafts/engine_ops/CMakeLists.txt`
- Create: `cxx/tools/bakeoff/materialize_engine_ops.py`
- Create: `tests/cxx/test_materialize_engine_ops.py`
- Create: `cxx/docs/bakeoffs/grafts/engine-ops-source.json`
- Create: `cxx/docs/bakeoffs/grafts/engine-ops.json`
- Create: `cxx/docs/bakeoffs/grafts/engine-ops.md`
- Modify: `cxx/CMakeLists.txt`

**Interfaces:**

```cpp
namespace libtmux::spike::engine_adapter {

using Operation = std::variant<NewSession, SplitPane, SendKeys, KillSession>;

struct CaptureRef {
  std::uint64_t producer;
  std::string field;
};

enum class Status { complete, failed, skipped, unknown };

std::expected<CommandRequest, LoweringError> lower(
    const Operation& operation,
    const Bindings& bindings,
    const Capabilities& capabilities);

PlanReport execute(const Plan& plan, ExecutionAdapter& adapter);

}  // namespace libtmux::spike::engine_adapter
```

- [ ] **Step 1: Write the pinned-source materializer red**

`engine-ops-source.json` fixes repository URI
`https://github.com/tmux-python/libtmux.git`, commit
`5b2c88e57e6e15422a8e845ef5d55fe7a606c315`, tree
`6cf797dc43d0d5b0f20e8dda3ba0383557cf124c`, and exactly these inspected
paths:

```text
src/libtmux/experimental/engines/base.py
src/libtmux/experimental/ops/_chain.py
src/libtmux/experimental/ops/_types.py
src/libtmux/experimental/ops/execute.py
src/libtmux/experimental/ops/operation.py
src/libtmux/experimental/ops/plan.py
src/libtmux/experimental/ops/planner.py
src/libtmux/experimental/ops/results.py
```

Tests construct a disposable Git remote and require rejection of a changed URI,
commit, tree, path set, blob identity, symlink, submodule, dirty destination, or
path escape.

```console
$ uv run pytest tests/cxx/test_materialize_engine_ops.py -v
```

Expected: FAIL importing `materialize_engine_ops`.

- [ ] **Step 2: Implement, materialize, and verify the recorded source**

The materializer fetches only into a task-owned cache, resolves the commit from
the configured URI, verifies the complete tree and every listed blob, and
writes only the eight inspected files plus a normalized materialization
manifest. It never accepts an ambient checkout or searches local worktrees.

```console
$ uv run python cxx/tools/bakeoff/materialize_engine_ops.py \
    --spec cxx/docs/bakeoffs/grafts/engine-ops-source.json \
    --output cxx/build/engine-ops-source
```

Expected: the output is a clean read-only materialization of the pinned Git
objects. `engine-ops.json` records the source-lock and materialization-manifest
digests, repository URI, commit, tree, exact inspected paths, and tool command.

Rerun the focused materializer tests after implementation:

```console
$ uv run pytest tests/cxx/test_materialize_engine_ops.py -v
```

Expected: all materializer tests pass against their disposable Git remotes.

- [ ] **Step 3: Write the dependent-capture plan**

Create a session, capture its pane ID, consume that ID in a split, capture the
new pane ID, and consume it in `send-keys`. Test `kill-session(group=true)` on
both sides of exact tmux `3.7` capability behavior.

Create the declaration-only adapter model and register aggregate target
`graft_engine_ops_contracts` before defining `lower` or `execute`. The target
owns the adapter runtime tests and entity-header compile-isolation probe and
registers the `graft.engine_ops.*` CTest cases.

- [ ] **Step 4: Write honest terminal-status tests**

When a creator fails, its dependent is `skipped`, an independent operation
still executes, and ambiguous grouped attribution is `unknown`. Every
operation receives exactly one terminal status and retains its semantic ID.

- [ ] **Step 5: Prove entity headers do not depend on engine types**

A compile probe includes all contender entity headers and fails the task if
they include or store `Operation`, `Plan`, `ExecutionAdapter`, or executor
types.

- [ ] **Step 6: Run the registered adapter compile/link red**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target graft_engine_ops_contracts
```

Expected: FAIL for missing `lower` and `execute` definitions, not for an unknown
target, missing materialized source, or unrelated infrastructure error.

- [ ] **Step 7: Implement the adapter boundary**

Implement capability-aware lowering and execution without adding engine types
to entity storage. Resolve a `CaptureRef` only after its producer completes,
execute independent operations after another branch fails, preserve semantic
IDs, and assign exactly one `complete`, `failed`, `skipped`, or `unknown` status
to every operation.

- [ ] **Step 8: Rebuild the target and record the strict gate**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target graft_engine_ops_contracts
```

```console
$ uv run python -m cxx.tools.evidence.ctest_gate \
    --source-dir cxx \
    --preset cxx-dev \
    --match '^graft\.engine_ops\.' \
    --gate-id graft-engine-ops \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/graft-engine-ops.json
```

Expected: all adapter and compile-isolation tests pass, and the report retains
the immutable gate digest.

- [ ] **Step 9: Commit the adapter probe**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/spikes/grafts/engine_ops \
    cxx/tools/bakeoff/materialize_engine_ops.py \
    tests/cxx/test_materialize_engine_ops.py \
    cxx/docs/bakeoffs/grafts/engine-ops-source.json \
    cxx/docs/bakeoffs/grafts/engine-ops.json \
    cxx/docs/bakeoffs/grafts/engine-ops.md
```

```console
$ git commit -F - <<'EOF'
CXX(spike[engine]): Probe operation graft

why: Preserve semantic plans without coupling entities to an executor.

what:
- Lower capability-aware operations after dependency capture
- Retain exact, failed, skipped, and unknown terminal attribution
EOF
```

### Task 8: Measure, select, and review the transport design

**Files:**

- Create: `cxx/tools/bakeoff/measure_transport.py`
- Create: `cxx/tools/bakeoff/verify_decision.py`
- Create: `tests/cxx/test_measure_transport.py`
- Create: `tests/cxx/test_verify_decision.py`
- Create: `cxx/docs/bakeoffs/environment.json`
- Create: `cxx/docs/bakeoffs/transport/measurements.json`
- Create: `cxx/docs/bakeoffs/transport/diagnostics/`
- Create: `cxx/docs/bakeoffs/transport/decision.json`
- Create: `cxx/docs/bakeoffs/transport/scorecard.md`
- Create: `cxx/docs/bakeoffs/transport/review.md`
- Create conditionally: `cxx/docs/plans/followups/`
- Modify: `cxx/CMakePresets.json`

**Interfaces:**

`measure_transport.py` accepts `--candidate`, `--build-dir`, `--repetitions 7`,
`--sanitize-gate`, `--tsan-gate`, and `--output`. It rejects a gate whose
registration, JUnit, cache, executable, compiler, or selection digest does not
validate, or whose source/candidate/compiler identity is incompatible with the
measurement build. It records hard-gate results, clean and controlled
incremental compile time, public-header parse time, binary sections, wrapper
dispatch, allocations, source/template footprint, consumer behavior, and
sanitized invalid-code diagnostics.

- [ ] **Step 1: Test deterministic and path-safe measurement output**

The test feeds fake measurements in two different temporary directories and
requires identical normalized JSON. It rejects absolute paths, a hostname,
fewer than seven samples, missing compiler identity, an absent ASan/UBSan or
TSan gate, and any tampered registration, JUnit, cache, or executable digest.
Decision-verifier tests reject a material unknown, a material item without a
linked follow-up result, and a non-material item without a contract-impact
rationale.

- [ ] **Step 2: Run both focused test modules before implementation**

```console
$ uv run pytest \
    tests/cxx/test_measure_transport.py \
    tests/cxx/test_verify_decision.py \
    -v
```

Expected: FAIL importing `measure_transport` and `verify_decision`.

- [ ] **Step 3: Implement and verify the evidence tools**

Implement deterministic normalization, gate/source identity validation,
material-unknown policy, and path-safe diagnostics. Rerun the same focused
tests:

```console
$ uv run pytest \
    tests/cxx/test_measure_transport.py \
    tests/cxx/test_verify_decision.py \
    -v
```

Expected: all measurement and decision-verifier unit tests pass.

- [ ] **Step 4: Run each contender through every hard gate**

Reconfigure and rebuild the isolated ASan/UBSan tree:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize
```

```console
$ uv run python -m cxx.tools.evidence.ctest_gate \
    --source-dir cxx \
    --preset cxx-sanitize \
    --label transport \
    --gate-id transport-sanitize \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/transport-sanitize.json
```

Reconfigure and rebuild the mutually exclusive TSan tree:

```console
$ cmake --preset cxx-tsan
```

```console
$ cmake --build --preset cxx-tsan
```

```console
$ uv run python -m cxx.tools.evidence.ctest_gate \
    --source-dir cxx \
    --preset cxx-tsan \
    --label concurrency \
    --gate-id transport-tsan \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/transport-tsan.json
```

Expected: all three candidates pass the ASan/UBSan transport gate and the same
concurrency contract passes under standalone TSan. Both immutable gate digests
enter `decision.json`. A hard-gate failure disqualifies that implementation
from measurement, and this phase remains incomplete until the contender is
corrected or replaced by another distinct working implementation.

- [ ] **Step 5: Capture seven controlled measurement runs**

Add the isolated `cxx-transport-measure` configure and build presets with
release optimization, the same compiler and flags as the gate, tests enabled,
and `cxx/build/cxx-transport-measure` as the build directory.

```console
$ cmake --preset cxx-transport-measure
```

```console
$ cmake --build --preset cxx-transport-measure
```

```console
$ uv run python cxx/tools/bakeoff/measure_transport.py \
    --candidate all \
    --build-dir cxx/build/cxx-transport-measure \
    --repetitions 7 \
    --sanitize-gate cxx/build/evidence/transport-sanitize.json \
    --tsan-gate cxx/build/evidence/transport-tsan.json \
    --output cxx/docs/bakeoffs/transport/measurements.json
```

Expected: normalized JSON contains all required measures and source/tool
identities.

- [ ] **Step 6: Write the evidence-led decision**

`decision.json` names one winner, accepted grafts, rejected trade-offs, hard
gate evidence IDs, measurement IDs, and classified unknowns. Each unknown has
`materiality`, an evidence ID, and a follow-up disposition. A material unknown
requires a focused follow-up spike with its own failing test, measurement, and
review disposition before selection; it cannot remain open. Before that work,
stop and add a tracked subordinate plan under `cxx/docs/plans/followups/`,
named by the unknown's stable ID and listing exact paths, gates, and commits.
Non-material unknowns state why they cannot change the public contract or
winner. The decision does not use a preselected weighted formula.
`scorecard.md` explains the same result in plain technical prose.

- [ ] **Step 7: Obtain an independent adversarial review**

The reviewer challenges lifetime ownership, hidden serialization, diagnostic
reentrancy, exception containment, timeout certainty, transport leakage,
control-group attribution, engine coupling, and measurement fairness. Fix or
disposition every finding with evidence; correctness or public-contract
findings cannot be waived.

Commit every review-driven implementation, contract, test, or CMake-registration
fix as an atomic issue-family change in its owning contender or graft before
closing the report. After the last fix, repeat Steps 4 through 6 in full: rerun
both hard-gate trees, replace the controlled measurements, and regenerate the
decision and scorecard from those current records. The final source identities
must match the reviewed commits. Before Step 8, status may contain only the
Task 8 evidence, review, and decision paths listed in Step 9; an unstaged source
or registration fix blocks closeout.

- [ ] **Step 8: Run the report verifier**

```console
$ uv run python cxx/tools/bakeoff/verify_decision.py \
    --axis transport \
    --require-review-closed
```

Expected: exit zero only when every contender, graft, measurement, source
identity, and review disposition is present and no material unknown remains.

- [ ] **Step 9: Commit transport evidence**

If a material follow-up ran, its subordinate plan and exact implementation
paths were already committed atomically; do not pass an absent follow-up
directory to `git add`.

```console
$ git add \
    cxx/CMakePresets.json \
    cxx/tools/bakeoff/measure_transport.py \
    cxx/tools/bakeoff/verify_decision.py \
    tests/cxx/test_measure_transport.py \
    tests/cxx/test_verify_decision.py \
    cxx/docs/bakeoffs/environment.json \
    cxx/docs/bakeoffs/transport \
    cxx/docs/bakeoffs/grafts
```

```console
$ git commit -F - <<'EOF'
CXX(docs[transport]): Select backend design

why: Freeze the private transport only after common gates and
     measurements.

what:
- Record normalized contender, control-mode, and engine-adapter evidence
- Close an independent review and name the accepted transport graft
EOF
```
