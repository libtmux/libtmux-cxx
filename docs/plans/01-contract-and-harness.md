# Contract and harness implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish reproducible Python parity observations, a pinned C++ build
harness, isolated real-tmux fixtures, and a canonical differential framework
without writing production library code.

**Architecture:** Python tooling reads release and development sources through
Git object APIs and emits deterministic JSON. CMake builds only test support and
contract executables; `ScopedTmuxServer` owns real tmux processes in both socket
modes, while differential tooling compares canonical records independently of
the future C++ implementation.

**Tech Stack:** Python 3.10+, `ast`, Git plumbing, CMake 3.28.3, Ninja 1.11.1,
C++23, GCC 13 with libstdc++ 13, Clang 18.1.3 with libc++ 18.1, GoogleTest
1.17.0, real tmux, Ruff, mypy, and pytest.

## Global Constraints

- No production header under `cxx/include/libtmux/` or source under `cxx/src/`
  is created in this phase.
- C++23 remains dependency-free; GoogleTest is test-only and fetched only when
  `LIBTMUX_FETCH_DEPS=ON`.
- GoogleTest is pinned to `v1.17.0` with archive SHA-256
  `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c`.
- Build outputs stay under ignored `cxx/build/` paths.
- Python source extraction uses `git show` and `git ls-tree`; it never imports
  or checks out the target revision.
- The parity path boundary contains only recorded Python API inputs. New C++
  files cannot create parity drift.
- `ScopedTmuxServer` never sets `HOME`, removes only `TMUX` and `TMUX_PANE`,
  and cleans only resources it owns.
- Tests use real tmux unless a failure path cannot be reached with a real
  process; every such test explains the exception in its docstring.
- Every task ends in an independently reviewable commit using repository commit
  format.
- Every Python function and method uses the repository's NumPy docstring style
  and a working doctest. Run `uv run pytest --doctest-modules cxx/tools` for
  each Python-tool task; `# doctest: +SKIP` is forbidden.

---

## File Structure

- `.clang-format` owns C++ layout.
- `.clang-tidy` contains the initial curated checks.
- `cxx/CMakeLists.txt` is the non-production phase orchestrator and later
  becomes the package root.
- `cxx/CMakePresets.json` defines reproducible configure, build, and test
  presets.
- `cxx/cmake/ProjectOptions.cmake` applies first-party warnings and sanitizers
  without leaking them to dependencies.
- `cxx/cmake/GoogleTest.cmake` resolves system GoogleTest or the explicit pinned
  fallback.
- `cxx/parity/` contains inputs, schemas, observations, mappings, and shards.
- `cxx/tools/parity/` contains deterministic source and manifest tooling.
- `cxx/tests/support/` contains non-installed process and tmux fixtures.
- `cxx/tools/differential/` contains canonical record comparison.
- `cxx/tools/evidence/` captures immutable CTest and phase-gate records.
- `tests/cxx/` tests Python-side C++ tooling.

### Task 1: Pin the C++ build and test harness

**Files:**

- Modify: `.gitignore`
- Modify: `.tool-versions`
- Modify: `justfile`
- Create: `.clang-format`
- Create: `.clang-tidy`
- Create: `cxx/CMakeLists.txt`
- Create: `cxx/CMakePresets.json`
- Create: `cxx/cmake/ProjectOptions.cmake`
- Create: `cxx/cmake/GoogleTest.cmake`
- Create: `cxx/cmake/toolchains/clang-libcxx.cmake`
- Create: `cxx/tests/CMakeLists.txt`
- Create: `cxx/tests/build_smoke_test.cpp`

**Interfaces:**

- Consumes: CMake 3.28.3, Clang 18.1.3 with libc++ 18.1, and Ninja
  1.11.1.
- Produces: presets `cxx-dev`, `cxx-sanitize`, and `cxx-tsan`; target
  `libtmux_build_options`; test `build_smoke`.

- [ ] **Step 1: Add the smoke test before its build exists**

```cpp
#include <gtest/gtest.h>

#include <expected>

#if defined(__clang__) && (!defined(_LIBCPP_VERSION) || _LIBCPP_VERSION != 180100)
#error "Clang 18.1.3 must use libc++ 18.1"
#endif

TEST(BuildSmoke, UsesCxx23) {
  static_assert(__cpp_lib_expected >= 202202L);
  std::expected<int, int> value{42};
  EXPECT_EQ(*value, 42);
}
```

The Clang toolchain file performs standard-library selection before compiler
detection:

```cmake
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-stdlib=libc++")
```

The pinned [libc++ 18.1.3 configuration](https://github.com/llvm/llvm-project/blob/llvmorg-18.1.3/libcxx/include/__config#L62-L65)
encodes the 18.1 release series as `_LIBCPP_VERSION=180100`; the evidence layer
records resolved header and library digests separately.

- [ ] **Step 2: Verify configure fails for the missing project**

```console
$ cmake --preset cxx-dev
```

Expected: FAIL because `cxx/CMakePresets.json` or the CMake project does not
exist yet.

- [ ] **Step 3: Add the project and target-local options**

`cxx/CMakeLists.txt` starts with this contract:

```cmake
cmake_minimum_required(VERSION 3.25)
project(libtmux_cxx LANGUAGES CXX)

option(LIBTMUX_BUILD_TESTS "Build libtmux C++ tests" ${PROJECT_IS_TOP_LEVEL})
option(LIBTMUX_BUILD_EXAMPLES "Build libtmux C++ examples" ${PROJECT_IS_TOP_LEVEL})
option(LIBTMUX_FETCH_DEPS "Download missing test-only dependencies" OFF)
option(LIBTMUX_ENABLE_SANITIZERS "Enable ASan and UBSan" OFF)
option(LIBTMUX_ENABLE_THREAD_SANITIZER "Enable TSan" OFF)
option(LIBTMUX_ENABLE_CLANG_TIDY "Run clang-tidy" OFF)
option(LIBTMUX_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" OFF)
set(LIBTMUX_CXX_STANDARD "23" CACHE STRING "C++ package standard: 20 or 23")
set_property(CACHE LIBTMUX_CXX_STANDARD PROPERTY STRINGS 20 23)

if(LIBTMUX_ENABLE_SANITIZERS AND LIBTMUX_ENABLE_THREAD_SANITIZER)
  message(FATAL_ERROR "ASan/UBSan and TSan require separate build trees")
endif()

include(cmake/ProjectOptions.cmake)

if(LIBTMUX_BUILD_TESTS)
  enable_testing()
  include(cmake/GoogleTest.cmake)
  libtmux_resolve_googletest()
  add_subdirectory(tests)
endif()
```

`cxx/cmake/GoogleTest.cmake` uses the pinned fallback only when allowed:

```cmake
include(FetchContent)

function(libtmux_resolve_googletest)
  find_package(GTest 1.17 CONFIG QUIET)
  if(TARGET GTest::gtest_main)
    return()
  endif()
  if(NOT LIBTMUX_FETCH_DEPS)
    message(FATAL_ERROR
      "GoogleTest 1.17 is required when LIBTMUX_BUILD_TESTS=ON; "
      "set LIBTMUX_FETCH_DEPS=ON to use the pinned fallback")
  endif()
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endfunction()
```

`cxx/tests/CMakeLists.txt` keeps requirements private:

```cmake
add_executable(build_smoke build_smoke_test.cpp)
target_compile_features(build_smoke PRIVATE cxx_std_23)
target_link_libraries(build_smoke PRIVATE GTest::gtest_main libtmux_build_options)
add_test(NAME build_smoke COMMAND build_smoke)
set_tests_properties(build_smoke PROPERTIES LABELS "contract")
```

`cxx/CMakePresets.json` is complete enough to run all initial gates:

```json
{
  "version": 6,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 25,
    "patch": 0
  },
  "configurePresets": [
    {
      "name": "cxx-base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "toolchainFile": "${sourceDir}/cmake/toolchains/clang-libcxx.cmake",
      "environment": {
        "LIBTMUX_EXPECT_COMPILER_ID": "Clang",
        "LIBTMUX_EXPECT_COMPILER_VERSION": "18.1.3"
      },
      "cacheVariables": {
        "CMAKE_CXX_COMPILER": "clang++",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "LIBTMUX_BUILD_TESTS": "ON",
        "LIBTMUX_FETCH_DEPS": "ON",
        "LIBTMUX_WARNINGS_AS_ERRORS": "ON"
      }
    },
    {
      "name": "cxx-dev",
      "inherits": "cxx-base",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug"
      }
    },
    {
      "name": "cxx-sanitize",
      "inherits": "cxx-dev",
      "cacheVariables": {
        "LIBTMUX_ENABLE_SANITIZERS": "ON",
        "LIBTMUX_ENABLE_THREAD_SANITIZER": "OFF"
      }
    },
    {
      "name": "cxx-tsan",
      "inherits": "cxx-dev",
      "cacheVariables": {
        "LIBTMUX_ENABLE_SANITIZERS": "OFF",
        "LIBTMUX_ENABLE_THREAD_SANITIZER": "ON"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "cxx-dev",
      "configurePreset": "cxx-dev"
    },
    {
      "name": "cxx-sanitize",
      "configurePreset": "cxx-sanitize"
    },
    {
      "name": "cxx-tsan",
      "configurePreset": "cxx-tsan"
    }
  ],
  "testPresets": [
    {
      "name": "cxx-dev",
      "configurePreset": "cxx-dev",
      "output": {
        "outputOnFailure": true
      }
    },
    {
      "name": "cxx-sanitize",
      "configurePreset": "cxx-sanitize",
      "output": {
        "outputOnFailure": true
      }
    },
    {
      "name": "cxx-tsan",
      "configurePreset": "cxx-tsan",
      "output": {
        "outputOnFailure": true
      }
    }
  ]
}
```

`ProjectOptions.cmake` creates one interface target used only by first-party
tests and spikes. For GCC and Clang it adds `-Wall`, `-Wextra`, `-Wpedantic`,
`-Wconversion`, and `-Wsign-conversion`; it adds `-Werror` only when requested.
The sanitizer option adds `-fsanitize=address,undefined` to both compile and
link options on that target. The thread-sanitizer option instead adds
`-fsanitize=thread`; the configure-time exclusion above prevents either flag
set from entering the other's build tree. After `project()`, the module also
checks the configured `CMAKE_CXX_COMPILER_ID` and
`CMAKE_CXX_COMPILER_VERSION` against the preset environment. Configuration
fails unless the cache-selected `clang++` is Clang 18.1.3 and the toolchain
probe reports libc++ 18.1 with `__cpp_lib_expected >= 202202L`. The
`clang-libcxx.cmake` toolchain selects `-stdlib=libc++` for compilation and
linking before `project()`; standard-library selection is toolchain-wide, while
warnings and sanitizers remain target-local. Later compiler-matrix presets pair
Clang 18.1.3 with libc++ 18.1 and GCC 13 with libstdc++ 13, overriding the
toolchain, compiler, and both expected values together.

The post-`project()` compiler guard is exact:

```cmake
if(NOT IS_ABSOLUTE "${CMAKE_CXX_COMPILER}"
   OR NOT EXISTS "${CMAKE_CXX_COMPILER}")
  message(FATAL_ERROR "CMAKE_CXX_COMPILER is not a resolved executable")
endif()
if(DEFINED ENV{LIBTMUX_EXPECT_COMPILER_ID}
   AND NOT CMAKE_CXX_COMPILER_ID STREQUAL
       "$ENV{LIBTMUX_EXPECT_COMPILER_ID}")
  message(FATAL_ERROR "unexpected C++ compiler ID")
endif()
if(DEFINED ENV{LIBTMUX_EXPECT_COMPILER_VERSION}
   AND NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL
       "$ENV{LIBTMUX_EXPECT_COMPILER_VERSION}")
  message(FATAL_ERROR "unexpected C++ compiler version")
endif()
```

Start `.clang-format` from LLVM style with an 88-column limit, left-aligned
pointers, case-sensitive include sorting, and `Standard: Latest`. Start
`.clang-tidy` with the closed list `clang-analyzer-*`,
`bugprone-use-after-move`, `bugprone-unchecked-optional-access`,
`performance-*`, and `portability-*`; later quality review may change that list
only with evidence.

- [ ] **Step 4: Pin tools and ignore only C++ build outputs**

Add `clang 18.1.3`, `cmake 3.28.3`, and `ninja 1.11.1` to
`.tool-versions`. Record libc++ and libc++abi 18.1 as required system toolchain
components; the smoke configuration rejects another implementation or
major/minor, and evidence records the resolved header and library digests. Add
`/cxx/build/` to `.gitignore`. Add
`cxx-configure`, `cxx-build`, and `cxx-test` recipes that each invoke one preset
command.

- [ ] **Step 5: Reject a mixed sanitizer tree**

```console
$ cmake \
    -S . \
    -B build/invalid-sanitizers \
    -G Ninja \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DLIBTMUX_ENABLE_SANITIZERS=ON \
    -DLIBTMUX_ENABLE_THREAD_SANITIZER=ON
```

Expected: configure fails with `ASan/UBSan and TSan require separate build
trees` before creating a test target.

- [ ] **Step 6: Configure, build, and run the smoke test**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev
```

```console
$ ctest --preset cxx-dev -R '^build_smoke$' --no-tests=error --output-on-failure
```

Expected: one passing C++23 GoogleTest.

The configure log and cache must identify the resolved compiler as Clang
18.1.3. A cache created with ambient `c++`, another Clang release, or GCC fails
the preset expectation rather than becoming bakeoff evidence.

Configure and build the isolated TSan tree:

```console
$ cmake --preset cxx-tsan
```

```console
$ cmake --build --preset cxx-tsan --target build_smoke
```

```console
$ ctest --preset cxx-tsan -R '^build_smoke$' --no-tests=error --output-on-failure
```

Expected: the smoke test passes with TSan and without ASan/UBSan flags.

- [ ] **Step 7: Prove production-only configure does not use the network**

```console
$ cmake \
    -S . \
    -B build/no-tests \
    -DLIBTMUX_BUILD_TESTS=OFF \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
```

Expected: configure exits zero without resolving GoogleTest.

- [ ] **Step 8: Commit the build harness**

```console
$ git add \
    .clang-format \
    .clang-tidy \
    .gitignore \
    .tool-versions \
    justfile \
    cxx/CMakeLists.txt \
    cxx/CMakePresets.json \
    cxx/cmake \
    cxx/tests/CMakeLists.txt \
    cxx/tests/build_smoke_test.cpp
```

```console
$ git commit -F - <<'EOF'
CXX(chore[build]): Add test scaffold

why: Give every C++ contract and spike a pinned, isolated build gate.

what:
- Add compiler-verified CMake presets and isolated sanitizer options
- Resolve test-only GoogleTest through an explicit pinned fallback
EOF
```

### Task 2: Extract reproducible Python API observations

**Files:**

- Modify: `pyproject.toml`
- Create: `cxx/parity/inputs.json`
- Create: `cxx/tools/parity/__init__.py`
- Create: `cxx/tools/parity/model.py`
- Create: `cxx/tools/parity/git_objects.py`
- Create: `cxx/tools/parity/drift.py`
- Create: `cxx/tools/parity/extract.py`
- Create: `tests/cxx/__init__.py`
- Create: `tests/cxx/test_parity_extract.py`

**Interfaces:**

- Consumes: repository path, revision, and exact parity-input paths.
- Produces: `SourceIdentity`, `ApiObservation`, and
  `extract_revision(revision, paths)` without importing `libtmux`.

- [ ] **Step 1: Write a failing disposable-repository extraction test**

```python
from __future__ import annotations

import pathlib
import subprocess

from cxx.tools.parity.extract import extract_revision


def test_extract_revision_reads_committed_source(tmp_path: pathlib.Path) -> None:
    """Extraction reads the requested Git object, not the working tree."""
    repo = tmp_path / "repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    subprocess.run(
        ["git", "config", "user.email", "test@example.invalid"],
        cwd=repo,
        check=True,
    )
    subprocess.run(
        ["git", "config", "user.name", "Parity Test"],
        cwd=repo,
        check=True,
    )
    source = repo / "api.py"
    source.write_text("def public(value: int = 1) -> str:\n    return str(value)\n")
    subprocess.run(["git", "add", "api.py"], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-qm", "fixture"], cwd=repo, check=True)
    source.write_text("def changed() -> None:\n    return None\n")

    observation = extract_revision(repo, "HEAD", ("api.py",))

    assert [entry.qualname for entry in observation.entries] == ["public"]
    assert tuple(item.path for item in observation.inputs) == ("api.py",)
    assert observation.clean is False
```

- [ ] **Step 2: Run the focused test and verify the import fails**

```console
$ uv run pytest tests/cxx/test_parity_extract.py -v
```

Expected: FAIL because `cxx.tools.parity.extract` does not exist.

- [ ] **Step 3: Define immutable observation models**

```python
from __future__ import annotations

import dataclasses


@dataclasses.dataclass(frozen=True, slots=True)
class SourceIdentity:
    revision: str
    commit: str
    tree: str
    generator_version: int
    clean_policy: str
    clean: bool


@dataclasses.dataclass(frozen=True, slots=True)
class InputObject:
    path: str
    kind: str
    object_id: str


@dataclasses.dataclass(frozen=True, slots=True)
class ApiEntry:
    entry_id: str
    kind: str
    module: str
    qualname: str
    source_path: str
    signature: str | None
    value_shape: object | None = None
    decorators: tuple[str, ...] = ()
    bases: tuple[str, ...] = ()
    observable_protocols: tuple[str, ...] = ()


@dataclasses.dataclass(frozen=True, slots=True)
class ApiObservation:
    source: SourceIdentity
    inputs: tuple[InputObject, ...]
    entries: tuple[ApiEntry, ...]
```

Implement `git_objects.py` with `subprocess.run(..., check=True,
capture_output=True)` wrappers for `git rev-parse`, `git show`, `git diff`, and
`git ls-tree`. Record every configured blob or subtree object ID. `extract.py`
parses the returned source bytes with `ast.parse` and sorts entries by
`entry_id`.

`drift.py` compares only those recorded inputs with the working tree. It
reports added, modified, deleted, and blob/tree type changes under recorded
subtrees while ignoring `cxx/`, build output, and unrelated documentation.
Tests modify both an input and a non-input to prove the boundary. The recorded
`clean_policy` is `recorded_inputs`; `clean` means those inputs match their Git
objects, not that unrelated C++ paths are absent from the worktree.

- [ ] **Step 4: Record the exact Python parity boundary**

`cxx/parity/inputs.json` contains the `src/libtmux/` subtree, the public API
documentation paths, `conftest.py`, and the package metadata fields that expose
pytest plugins or supported Python versions. It does not contain `cxx/`, cache
paths, build paths, unrelated documentation, or the whole repository tree.

Golden extraction tests cover `__all__`, free functions, constants, exposed
type aliases, overloads, properties, parameters and defaults, dataclass and
named-tuple fields, enum members, exception inheritance, protocols,
deprecations, equality/repr/iteration/index/hash/context methods, format
versions and scopes, options, hooks, sparse arrays, documented internal return
types, and pytest fixtures. The extractor evaluates only literal AST nodes and
never imports a target module.

- [ ] **Step 5: Add tooling to the Python quality boundary**

Retain recursive `tests` and add `cxx/tools` to `[tool.mypy].files` in
`pyproject.toml`. The recursive `tests` root covers `tests/cxx`; do not list
both roots because mypy 2.3.0 discovers duplicate `tests.cxx` modules.

- [ ] **Step 6: Run extraction tests and Python static gates**

```console
$ uv run pytest tests/cxx/test_parity_extract.py -v
```

```console
$ uv run ruff check cxx/tools tests/cxx
```

```console
$ uv run mypy cxx/tools tests/cxx
```

```console
$ uv run pytest --doctest-modules cxx/tools/parity
```

Expected: all commands exit zero.

- [ ] **Step 7: Commit the extractor**

```console
$ git add pyproject.toml cxx/parity/inputs.json cxx/tools/parity tests/cxx
```

```console
$ git commit -F - <<'EOF'
CXX(feat[parity]): Extract Python surface

why: Make release and development parity inputs reproducible Git
     objects.

what:
- Record the exact Python API source boundary
- Extract deterministic symbols and observable protocols without imports
EOF
```

### Task 3: Generate and validate parity manifests

**Files:**

- Create: `cxx/parity/manifest.schema.json`
- Create: `cxx/parity/approvals.schema.json`
- Create: `cxx/parity/evidence.schema.json`
- Create: `cxx/parity/release-v0.62.0.json`
- Create: `cxx/parity/development.json`
- Create: `cxx/parity/mapping.json`
- Create: `cxx/parity/approvals.json`
- Create: `cxx/parity/evidence.json`
- Create: `cxx/parity/manifest.json`
- Create: `cxx/parity/shards.json`
- Create: `cxx/tools/parity/__main__.py`
- Create: `cxx/tools/parity/generate.py`
- Create: `cxx/tools/parity/check_manifest.py`
- Create: `cxx/tools/parity/sync.py`
- Create: `cxx/tools/parity/shard.py`
- Create: `tests/cxx/test_parity_manifest.py`

**Interfaces:**

- Consumes: `extract_revision`, `v0.62.0`, `HEAD`, and `inputs.json`.
- Produces: deterministic observation JSON; `validate_mapping(observations,
mapping, approvals, evidence, complete)`; review-preserving synchronization;
  `manifest.semantic_contract_sha256`; and dependency-ordered `ParityShard`
  records.

- [ ] **Step 1: Write failing manifest validation tests**

```python
from __future__ import annotations

from cxx.tools.parity.check_manifest import validate_mapping


def test_adaptation_requires_delta_oracle_and_approval() -> None:
    """An adapted entry is incomplete without evidence and approval."""
    errors = validate_mapping(
        observations=(),
        mapping={
            "entries": [
                {
                    "entry_id": "libtmux.server.Server.__eq__",
                    "observed_in": ["release-v0.62.0", "development"],
                    "observation_hashes": {
                        "release-v0.62.0": "release-hash",
                        "development": "development-hash",
                    },
                    "status": "adapted",
                    "cpp_symbol": "libtmux::Server::operator==",
                    "cpp_api_id": "libtmux::Server::operator==(Server const&) const",
                    "cpp_alias_of": None,
                    "compile_probe": "compile.server-equality",
                    "behavior_tests": ["behavior.server-equality"],
                    "doc_id": "docs.server-equality",
                    "example_id": "example.server-equality",
                    "error_behavior": "typed result",
                    "tmux_versions": ["all-supported"],
                    "boundary_tests": [],
                    "semantic_delta": "",
                    "oracle_id": "",
                    "approval_id": None,
                    "reconciliation": None,
                    "inapplicability_proof": None,
                }
            ]
        },
        approvals={"approvals": []},
        evidence={
            "evidence": [
                {
                    "evidence_id": "compile.server-equality",
                    "kind": "compile",
                    "path": "cxx/tests/compile/server-equality.cpp",
                    "case_id": "server-equality",
                },
                {
                    "evidence_id": "behavior.server-equality",
                    "kind": "behavior",
                    "path": "cxx/tests/integration/server-equality.cpp",
                    "case_id": "server-equality",
                    "cmake_target": "server_equality_test",
                    "ctest_name": "parity.server-equality",
                    "ctest_label": "real-tmux",
                    "execution_mode": "real_tmux",
                    "real_tmux": True,
                    "result_sha256": "result-hash",
                    "tmux_binary_sha256": "binary-hash",
                    "tmux_version": "3.7b",
                },
                {
                    "evidence_id": "docs.server-equality",
                    "kind": "documentation",
                    "path": "cxx/docs/api/server.md",
                    "case_id": "server-equality",
                },
                {
                    "evidence_id": "example.server-equality",
                    "kind": "example",
                    "path": "cxx/examples/parity/server.cpp",
                    "case_id": "server-equality",
                },
            ]
        },
        complete=False,
    )
    adaptation_errors = [error for error in errors if "adapted entry" in error]
    assert adaptation_errors == [
        "libtmux.server.Server.__eq__: adapted entry lacks semantic_delta",
        "libtmux.server.Server.__eq__: adapted entry lacks oracle_id",
        "libtmux.server.Server.__eq__: adapted entry lacks approval_id",
    ]
```

Also add `test_evidence_only_sync_preserves_semantic_contract_sha256`, which
changes only evidence records and their digests, requires the full manifest
binding to refresh, and requires `semantic_contract_sha256` to remain equal. A
parameterized `test_semantic_change_updates_semantic_contract_sha256` changes an
observation identity, one reviewed mapping semantic field, one approval, and one
entry's shard owner in turn; every change must produce a different semantic
digest.

- [ ] **Step 2: Run the test and verify the validator is missing**

```console
$ uv run pytest tests/cxx/test_parity_manifest.py -v
```

Expected: FAIL importing `check_manifest`.

- [ ] **Step 3: Implement deterministic generation and validation**

Each mapping entry has these concrete keys:

```json
{
  "entry_id": "libtmux.server.Server.__eq__",
  "observed_in": ["release-v0.62.0", "development"],
  "observation_hashes": null,
  "status": "pending",
  "cpp_symbol": null,
  "cpp_api_id": null,
  "cpp_alias_of": null,
  "compile_probe": null,
  "behavior_tests": [],
  "doc_id": null,
  "example_id": null,
  "error_behavior": null,
  "tmux_versions": [],
  "boundary_tests": [],
  "semantic_delta": null,
  "oracle_id": null,
  "approval_id": null,
  "reconciliation": null,
  "inapplicability_proof": null
}
```

`cpp_api_id` is a normalized declaration identity, not a free-form display
name. Implemented and adapted rows require it. Exactly one canonical row owns
each ID; another Python observation may reuse that C++ declaration only by
setting `cpp_alias_of` to the canonical entry. Alias rows inherit the canonical
compile, documentation, and example ownership and cannot declare conflicting
evidence. Excluded and pending rows do not claim a C++ declaration.
A pending row may bind `semantic_delta`, `oracle_id`, and `approval_id` to
record a preapproved query-semantic decision. It still leaves every C++ symbol
and implementation-evidence field null until its production parity shard
promotes it to `adapted`; a matching design remains pending without adaptation
fields until that shard promotes it to `implemented`.

This manifest remains a Python-derived translation ledger, not an inventory of
the C++ headers. Plan 05 independently inventories exported declarations from
the registered public headers with Clang and requires a bijective join between
that inventory and canonical `cpp_api_id` owners. The coverage gate therefore
detects a C++-only declaration even when no Python observation introduced it.

`approvals.json` stores records with `approval_id`, decision kind, scope entry
IDs, accepted decision hash, and evidence path. `evidence.json` stores unique
`evidence_id`, kind (`compile`, `behavior`, `differential`, `documentation`,
`example`, or `version`), repository-relative path, and case ID. Executed
records additionally store the CMake target, CTest name and label, execution
mode, `real_tmux` flag, normalized result digest, selected tmux binary digest,
raw tmux version, and differential scenario record digest where applicable.
Validation rejects unknown, duplicate, wrong-kind, multiply owned, stale, or
failed evidence references. A required runtime row is incomplete unless its
behavior evidence names a registered live-tmux CTest and a matching current
result; a unit test or fixture flag alone cannot satisfy it.

When release and development contain the same entry ID with different
signatures, defaults, decorators, bases, or value shapes, synchronization emits
a conflict rather than unioning them silently. The mapping must describe a C++
surface compatible with both observations or name a versioned/adapted
reconciliation with differential and approval evidence.

Synchronization writes these differences to a deterministic
`unresolved_conflicts` array. Each record contains the `entry_id`, the sorted
set of differing observation fields, and the row's already-derived per-source
`observation_hashes`; records are sorted by `entry_id`. Structural verification
with `--allow-pending` accepts an exact unresolved conflict only while its
mapping row remains `pending`, claims no C++ declaration or implementation
evidence, and has no fabricated reconciliation or approval. It rejects a
missing, extra, stale, or inaccurately derived conflict record. Any promotion
from `pending` and complete verification reject unresolved conflicts.

Resolution requires either a C++ surface compatible with both observations or
an explicit versioned/adapted reconciliation with the required approval and
differential evidence. `unresolved_conflicts` is derived and excluded from the
semantic hash because source identities, observation hashes, and reviewed
reconciliation fields already carry its semantics. The initial generated
manifest records the four observed release/development conflicts and still
passes structural verification with `--allow-pending`; it does not normalize,
auto-approve, or invent evidence for them.

`generate.py` serializes with `sort_keys=True`, two-space indentation, UTF-8,
and one trailing newline. It builds the mapping scaffold by preserving reviewed
entries and adding only newly observed IDs as `pending`. Synchronization fills
the pending scaffold's `observation_hashes` from generated observations.

`sync.py` never rewrites a reviewed field. It requires the approval and evidence
sidecars, validates every referenced record, and embeds their current digests
and normalized records into the manifest. It writes
`manifest.semantic_contract_sha256` as the SHA-256 of canonical sorted JSON
containing only:

- the release and development source identities plus each entry's `entry_id`,
  `observed_in`, and `observation_hashes`;
- the reviewed mapping fields `status`, `cpp_symbol`, `cpp_api_id`,
  `cpp_alias_of`, `error_behavior`, `tmux_versions`, `semantic_delta`,
  `oracle_id`, `approval_id`, `reconciliation`, and `inapplicability_proof`;
- normalized approval records; and
- the fixed shard dependency order and each entry's shard owner.

The semantic projection excludes evidence records, embedded evidence digests,
CTest and JUnit data, differential results, selected-binary digests, and every
other execution result. Full manifest validation still requires and validates
the current approval and evidence sidecars and their embedded bindings.
Differential artifacts bind `semantic_contract_sha256`, never the full manifest
digest, so refreshing execution evidence cannot invalidate the results used to
perform that refresh.

New observations become `pending`; removed observations produce
stale-classification violations. `shard.py` assigns every entry exactly once to
the fixed dependency order used by `05-python-parity.md` and writes
`shards.json`; synchronization computes its semantic projection with the same
assignment function. `__main__.py` exposes `generate`, `drift`, `sync`,
`record-evidence`, and `verify` subcommands so every later plan uses one CLI.
`record-evidence` consumes immutable `ctest_gate.py` records and differential
result records, verifies their registration digest, JUnit digest, selected
binary, and fixture mode, and updates only the named shard's deterministic
evidence records. It never reads CTest's mutable `Testing/` directory.

- [ ] **Step 4: Generate release and development observations**

```console
$ uv run python -m cxx.tools.parity generate --release v0.62.0 --development HEAD
```

Expected: the two observation files embed full commit/tree identities and the
mapping contains every unioned entry exactly once.

- [ ] **Step 5: Prove byte-for-byte regeneration**

```console
$ uv run python -m cxx.tools.parity generate --check cxx/parity
```

Expected: the command regenerates from the revisions embedded in the committed
observations and exits zero with no diff, even after unrelated C++ commits.

- [ ] **Step 6: Validate the pending scaffold and shard graph**

```console
$ uv run python -m cxx.tools.parity \
    sync \
    --release cxx/parity/release-v0.62.0.json \
    --development cxx/parity/development.json \
    --mapping cxx/parity/mapping.json \
    --approvals cxx/parity/approvals.json \
    --evidence cxx/parity/evidence.json \
    --output cxx/parity/manifest.json
```

```console
$ uv run python -m cxx.tools.parity \
    drift \
    --manifest cxx/parity/manifest.json \
    --worktree .
```

Expected: exit zero because only the recorded Python input objects participate
in drift detection.

```console
$ uv run python -m cxx.tools.parity \
    verify \
    --manifest cxx/parity/manifest.json \
    --mode structural \
    --allow-pending
```

Expected: exit zero; required entries may be pending but none are absent,
duplicated, or malformed.

Rerun the identical focused test after implementation:

```console
$ uv run pytest tests/cxx/test_parity_manifest.py -v
```

Expected: all manifest synchronization and validation tests pass.

- [ ] **Step 7: Commit the generated contract**

```console
$ git add cxx/parity cxx/tools/parity tests/cxx/test_parity_manifest.py
```

```console
$ git commit -F - <<'EOF'
CXX(feat[parity]): Generate API ledger

why: Turn full Python parity into a finite, reviewable completion
     contract.

what:
- Generate pinned release and development observations
- Validate mappings, approvals, evidence links, and dependency shards
EOF
```

### Task 4: Add the real-tmux RAII fixture

**Files:**

- Create: `cxx/tests/support/process.hpp`
- Create: `cxx/tests/support/process.cpp`
- Create: `cxx/tests/support/scoped_tmux_server.hpp`
- Create: `cxx/tests/support/scoped_tmux_server.cpp`
- Create: `cxx/tests/scoped_tmux_server_test.cpp`
- Create: `cxx/tests/scoped_tmux_server_failure_test.cpp`
- Create: `cxx/tests/support/process_test.cpp`
- Create: `cxx/tests/support/fake_tmux.cpp`
- Modify: `cxx/tests/CMakeLists.txt`

**Interfaces:**

- Consumes: explicit tmux binary and `SocketMode`.
- Produces: move-only `ScopedTmuxServer`, `socket_name()`, `socket_path()`,
  `command_prefix()`, and bounded `noexcept` teardown.

- [ ] **Step 1: Write failing `-L` and `-S` fixture tests**

Add the test source and register `scoped_tmux_server_test` before the red
build. The target initially binds declarations only, so the failure must name
missing fixture definitions rather than an unknown target.

```cpp
#include "support/scoped_tmux_server.hpp"

#include <gtest/gtest.h>

TEST(ScopedTmuxServer, StartsByNameAndExposesResolvedPath) {
  auto server = libtmux::test::ScopedTmuxServer::start(
      {.mode = libtmux::test::SocketMode::Name});
  ASSERT_TRUE(server.has_value());
  EXPECT_EQ(server->socket_mode(), libtmux::test::SocketMode::Name);
  EXPECT_TRUE(server->socket_name().has_value());
  EXPECT_FALSE(server->socket_path().empty());
  const auto prefix = server->command_prefix();
  ASSERT_EQ(prefix.size(), 3U);
  EXPECT_EQ(prefix[1], "-S");
  EXPECT_EQ(prefix[2], server->socket_path().string());
  EXPECT_TRUE(server->is_alive());
}

TEST(ScopedTmuxServer, StartsByExactPath) {
  auto server = libtmux::test::ScopedTmuxServer::start(
      {.mode = libtmux::test::SocketMode::Path});
  ASSERT_TRUE(server.has_value());
  EXPECT_EQ(server->socket_mode(), libtmux::test::SocketMode::Path);
  EXPECT_FALSE(server->socket_name().has_value());
  const auto prefix = server->command_prefix();
  ASSERT_EQ(prefix.size(), 3U);
  EXPECT_EQ(prefix[1], "-S");
  EXPECT_EQ(prefix[2], server->socket_path().string());
  EXPECT_TRUE(server->is_alive());
}
```

- [ ] **Step 2: Run the test and verify missing fixture symbols**

Configure the target before attempting the red build:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target scoped_tmux_server_test
```

Expected: FAIL at compile or link time for missing fixture definitions. An
unknown target is not the intended red result.

- [ ] **Step 3: Define the move-only fixture contract**

```cpp
#include <chrono>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libtmux::test {

enum class SocketMode { Name, Path };

struct TeardownReport {
  std::vector<std::string> messages;
};

struct ScopedTmuxServerOptions {
  std::filesystem::path tmux_binary{"tmux"};
  SocketMode mode{SocketMode::Path};
  std::chrono::milliseconds startup_timeout{5000};
  std::chrono::milliseconds teardown_timeout{2000};
  std::string session_name{"libtmux_test"};
  std::shared_ptr<TeardownReport> teardown_report;
};

class ScopedTmuxServer final {
 public:
  static std::expected<ScopedTmuxServer, std::string> start(
      ScopedTmuxServerOptions options = {});
  ~ScopedTmuxServer() noexcept;
  ScopedTmuxServer(ScopedTmuxServer&&) noexcept;
  ScopedTmuxServer& operator=(ScopedTmuxServer&&) noexcept;
  ScopedTmuxServer(const ScopedTmuxServer&) = delete;
  ScopedTmuxServer& operator=(const ScopedTmuxServer&) = delete;

  [[nodiscard]] SocketMode socket_mode() const noexcept;
  [[nodiscard]] std::optional<std::string_view> socket_name() const noexcept;
  [[nodiscard]] const std::filesystem::path& socket_path() const noexcept;
  [[nodiscard]] const std::filesystem::path& tmux_tmpdir() const noexcept;
  [[nodiscard]] std::string_view session_name() const noexcept;
  [[nodiscard]] int server_pid() const noexcept;
  [[nodiscard]] std::vector<std::string> command_prefix() const;
  [[nodiscard]] bool is_alive() const;

 private:
  struct State;
  explicit ScopedTmuxServer(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

}  // namespace libtmux::test
```

Implement `start()` with `mkdtemp`, a short fixture-owned directory,
`posix_spawnp`, `-D -u -f /dev/null`, and either `-L` plus child-only
`TMUX_TMPDIR` or exact `-S`. Readiness uses `steady_clock` and bounded polling.
After readiness, query `#{socket_path}` and create one deterministic detached
session. Regardless of the startup mode, `command_prefix()` returns exactly the
tmux executable, `-S`, and the resolved socket path. No caller needs the
fixture-owned `TMUX_TMPDIR`; `socket_mode()` and `socket_name()` preserve how the
server was started for assertions and evidence.

The destructor sends `kill-server`, waits to its deadline, sends `SIGTERM` then
`SIGKILL` only to its owned PID, reaps it, and removes only its private tree.
It appends teardown text to the optional shared report but never throws or
invokes user callbacks.

Register one executable with
`gtest_discover_tests(scoped_tmux_server_test TEST_PREFIX
"scoped_tmux_server_" PROPERTIES LABELS "contract;real-tmux")`. The support
process implementation is independent test code and must not be reused by the
production runner.

- [ ] **Step 4: Run both socket-mode tests**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target scoped_tmux_server_test
```

```console
$ ctest --preset cxx-dev -R '^scoped_tmux_server_' --no-tests=error --output-on-failure
```

Expected: both modes pass and no server remains on either selector.

- [ ] **Step 5: Add parallel, ambient-environment, and failure coverage**

Add tests that start eight fixtures concurrently; cover move construction and
assignment; preserve the parent `HOME`; remove child `TMUX`/`TMUX_PANE`; reject
an invalid binary; clean constructor and assertion-failure paths; survive a
server self-exit and stale socket; reject an overlong exact socket path without
deleting its parent; terminate a hung, TERM-resistant fake server; reap an
escaped pipe holder; and retain teardown diagnostics without masking the
primary failure.

- [ ] **Step 6: Run the fixture tests under sanitizers**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target scoped_tmux_server_test
```

```console
$ ctest --preset cxx-sanitize -L real-tmux --no-tests=error --output-on-failure
```

Expected: no leaks, zombies, use-after-free, or fixture collisions.

- [ ] **Step 7: Commit the fixture**

```console
$ git add \
    cxx/tests/CMakeLists.txt \
    cxx/tests/support \
    cxx/tests/scoped_tmux_server_test.cpp \
    cxx/tests/scoped_tmux_server_failure_test.cpp
```

```console
$ git commit -F - <<'EOF'
CXX(test[tmux]): Add scoped server fixture

why: Give every contract and bakeoff a real isolated tmux process.

what:
- Own named and exact-path socket modes through move-only RAII
- Bound readiness, teardown, reaping, and fixture-owned cleanup
EOF
```

### Task 5: Add canonical differential records

**Files:**

- Create: `cxx/tests/differential/scenario.schema.json`
- Create: `cxx/tests/differential/scenario_registry.json`
- Create: `cxx/tests/differential/scenarios/server-lifecycle.json`
- Create: `cxx/tests/differential/CMakeLists.txt`
- Modify: `cxx/tests/CMakeLists.txt`
- Create: `cxx/tools/differential/__init__.py`
- Create: `cxx/tools/differential/model.py`
- Create: `cxx/tools/differential/wire.py`
- Create: `cxx/tools/differential/canonicalize.py`
- Create: `cxx/tools/differential/compare.py`
- Create: `cxx/tools/differential/materialize.py`
- Create: `cxx/tools/differential/runner.py`
- Create: `cxx/tools/differential/python_reference.py`
- Create: `cxx/tests/support/differential_wire.hpp`
- Create: `cxx/tests/support/differential_wire.cpp`
- Create: `cxx/tests/differential/wire_protocol_test.cpp`
- Create: `tests/cxx/test_differential.py`

**Interfaces:**

- Consumes: scenario JSON, a recorded Python observation, its Git repository,
  the parity manifest, and an explicit fixture socket.
- Produces: `ScenarioRecord`, deterministic canonical JSON, and a structural
  diff with only approved unstable-field removal.

- [ ] **Step 1: Write a failing canonicalization test**

```python
from __future__ import annotations

from cxx.tools.differential.canonicalize import CanonicalizationRules, canonicalize


def test_canonicalize_removes_only_declared_unstable_fields() -> None:
    """Canonicalization retains semantic fields and declared ordering."""
    record = {
        "pid": 123,
        "socket_path": "/tmp/private/socket",
        "sessions": [{"name": "work", "id": "$9"}],
    }
    rules = CanonicalizationRules(
        entity_id_pointers=("/sessions/*/id",),
        remove_pointers=("/pid", "/socket_path"),
        unordered_pointers=(),
    )
    assert canonicalize(record, rules) == {
        "sessions": [{"name": "work", "id": "$SESSION_1"}],
    }
```

Add `wire_protocol_test.cpp` and register declaration-only target
`differential_wire_contracts` plus CTest name `differential.wire.protocol`
before implementing the wire encoder or decoder.

- [ ] **Step 2: Run the focused test and verify the module is missing**

```console
$ uv run pytest tests/cxx/test_differential.py -v
```

Expected: FAIL importing `canonicalize`.

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target differential_wire_contracts
```

Expected: FAIL at compile or link time for the missing wire implementation, not
for an unknown target or unrelated configure error.

- [ ] **Step 3: Implement explicit canonical record types**

```python
@dataclasses.dataclass(frozen=True, slots=True)
class ScenarioRecord:
    scenario_id: str
    tmux_version: str
    tmux_binary_sha256: str
    python_source_commit: str
    python_input_manifest_sha256: str
    semantic_contract_sha256: str
    operations: tuple[dict[str, object], ...]
    observations: tuple[dict[str, object], ...]


@dataclasses.dataclass(frozen=True, slots=True)
class AdapterSpec:
    name: str
    argv: tuple[str, ...]


@dataclasses.dataclass(frozen=True, slots=True)
class CanonicalizationRules:
    entity_id_pointers: tuple[str, ...]
    remove_pointers: tuple[str, ...]
    unordered_pointers: tuple[str, ...]
```

Canonicalization may replace tmux IDs, PIDs, timestamps, socket paths, and only
ordering marked unordered by exact JSON pointers in the scenario schema. A
missing pointer is an error. It must not remove stderr, return shapes, warnings,
or error categories.

Entity IDs use deterministic, per-kind bijections in first-occurrence order:
session IDs become `$SESSION_1`, `$SESSION_2`, window IDs become `@WINDOW_1`,
and pane IDs become `%PANE_1`. Tests require repeated references to retain the
same token, distinct IDs to remain distinct, cross-record foreign keys to
resolve, and dangling or wrong-kind references to fail. Collapsing every ID to
one token is forbidden.

- [ ] **Step 4: Add the Python reference runner**

`python_reference.py` accepts `--tmux-bin`, exactly one of `--socket-name` or
`--socket-path`, `--scenario`, `--output`, `--repository`,
`--observation`, `--input-manifest`, and `--semantic-contract-sha256`. It does
not accept `--source-root` or infer `HEAD`. `materialize.py` resolves the commit
recorded in the observation, verifies every recorded blob and subtree object
against the input manifest, and writes only those inputs to a task-owned
temporary import tree. The reference runner imports public `libtmux` from that
tree and writes one canonical record without configuring global logging or
spawning an unscoped server. It rejects a missing object, commit/tree mismatch,
missing or malformed semantic contract digest, path escape, symlink, submodule,
or import outside the materialized root.

`runner.py` supervises Python and future C++ adapter argv without a shell. Its
tests cover malformed JSON, the wrong schema or scenario ID, a missing or
unequal semantic digest, nonzero exit, stderr retention, timeout, and distinct
fixture sockets. It loads the explicit parity manifest, recomputes its semantic
projection, rejects a mismatched `semantic_contract_sha256`, and passes that
exact digest plus one resolved tmux executable to both adapters. It verifies the
executable's SHA-256 and raw `tmux -V`. The Python adapter also receives the
repository, recorded development observation, and input manifest; its
materialized source remains independent of the current checkout and `HEAD`. It
emits the recorded commit, input-manifest hash, and semantic contract digest.
The supervisor requires both adapters and every differential result artifact to
bind that same digest; it never substitutes the full manifest or
evidence-sidecar digest. Tests dirty and advance the working tree after
observation generation and still require the pinned revision's behavior. A
version string alone cannot make different binaries or stale Python objects
compare equal. The C++ adapter is added only after the production Server
vertical slice exists.

`scenario_registry.json` is the closed operation registry. Each operation tag
names its JSON request and response schema, Python handler, C++ handler, and
owning scenario. Both adapters reject unregistered tags. Later parity slices
extend this registry and both dispatchers in the same atomic commit. Both
adapters expose a test-only `--list-operations` mode: Python emits canonical
JSON, while C++ emits the same tags and registry digest through the retained
binary wire protocol. Coverage tooling invokes those modes rather than parsing
dispatcher source text.

Only Python parses scenario JSON. `model.py` performs the closed schema checks,
then `wire.py` lowers operations to version-one binary frames: a 32-bit
big-endian payload length, one-byte record tag, fixed field count, and
length-prefixed byte fields. The retained C++ test support accepts and emits
only those bounded frames; Python reconstructs canonical JSON observations.
Tests cover truncated lengths, oversized frames, unknown tags, embedded NUL,
arbitrary bytes, and trailing data. No C++ target links a JSON parser, and the
wire protocol is test-only rather than a public serialization API.

- [ ] **Step 5: Run comparator and reference tests**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target differential_wire_contracts
```

```console
$ ctest \
    --preset cxx-dev \
    -R '^differential\.wire\.protocol$' \
    --no-tests=error \
    --output-on-failure
```

```console
$ uv run pytest tests/cxx/test_differential.py -v
```

```console
$ uv run ruff check cxx/tools/differential tests/cxx/test_differential.py
```

```console
$ uv run mypy cxx/tools/differential tests/cxx/test_differential.py
```

Expected: all commands exit zero.

- [ ] **Step 6: Commit the differential framework**

```console
$ git add \
    cxx/tests/CMakeLists.txt \
    cxx/tests/differential \
    cxx/tests/support/differential_wire.hpp \
    cxx/tests/support/differential_wire.cpp \
    cxx/tools/differential \
    tests/cxx/test_differential.py
```

```console
$ git commit -F - <<'EOF'
CXX(test[parity]): Add differential records

why: Compare Python and C++ behavior without normalizing semantic
     differences.

what:
- Define canonical scenario and observation records
- Add an explicit-socket Python reference runner and structural
  comparator
EOF
```

### Task 6: Record and review the phase evidence

**Files:**

- Create: `cxx/tools/evidence/ctest_gate.py`
- Create: `cxx/tools/evidence/contract_gate.py`
- Create: `cxx/docs/evidence/contract-and-harness.json`
- Create: `cxx/docs/evidence/contract-and-harness-review.md`
- Create: `tests/cxx/test_ctest_gate.py`
- Create: `tests/cxx/test_contract_gate.py`

**Interfaces:**

- Consumes: current Git tree, tool versions, manifest checks, immutable CTest
  gate records, and Python gate results.
- Produces: path-scrubbed machine evidence and an independent review with no
  unresolved finding.

- [ ] **Step 1: Write the evidence schema test**

The test rejects absolute paths, missing source identities, nonzero gate exit
codes, and an empty fixture-mode list. `test_ctest_gate.py` uses a fake `ctest`
to require one `--show-only=json-v1` discovery call and one test call with
`--output-junit`. It rejects zero selected tests, duplicate or unexpected JUnit
cases, missing registered cases, a non-passing result, a changed executable or
cache digest, malformed XML/JSON, and reuse of mutable `Testing/` state.

- [ ] **Step 2: Run the schema test and verify the writer is missing**

```console
$ uv run pytest tests/cxx/test_ctest_gate.py tests/cxx/test_contract_gate.py -v
```

Expected: FAIL importing `ctest_gate` and `contract_gate`.

- [ ] **Step 3: Implement the gate writer and run the full phase gate**

`ctest_gate.py` accepts `--source-dir`, `--preset`, exactly one selector from
`--label` or `--match`, `--gate-id`, `--output-root`, and `--record`. It first
captures the selected registry with `ctest --show-only=json-v1`, then executes
the identical selection with `--no-tests=error --output-junit`. It hashes the
CMake cache, CTest registration files, selected executables, raw registry JSON,
and per-gate JUnit. After both commands finish, it atomically installs
`registered-tests.json`, `results.junit.xml`, and path-scrubbed `gate.json`
under a content-addressed gate directory, then writes the same normalized gate
record at the exact `--record` path. Content-addressed leaves are immutable.
After a successful later run, the named record is atomically replaced with a
pointer to the new leaf; the old leaf remains intact. A failed or incomplete
run cannot replace it. The tool never reads `Testing/`.

Capture the real-tmux sanitizer gate:

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
    --label real-tmux \
    --gate-id contract-real-tmux \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/contract-real-tmux.json
```

`contract_gate.py` executes commands as argv, records command names rather than
local binary paths, captures exit status and version text, and writes sorted
JSON. It invokes `ctest_gate.py` for every CTest selection and records only the
returned gate digest, registered/executed test IDs, and semantic outcomes in
the committed report; raw registry and JUnit files remain ignored build
artifacts.

```console
$ uv run python -m cxx.tools.evidence.contract_gate \
    --output cxx/docs/evidence/contract-and-harness.json
```

Expected: exit zero only after CMake, immutable CTest gates, parity
regeneration, Ruff, mypy, pytest, and docs succeed.

Rerun the identical focused test after implementation:

```console
$ uv run pytest tests/cxx/test_ctest_gate.py tests/cxx/test_contract_gate.py -v
```

Expected: all evidence-gate tests pass.

- [ ] **Step 4: Obtain an independent evidence review**

The reviewer checks provenance, ignored-path boundaries, real-tmux ownership,
failure coverage, and whether any production implementation entered the tree.
Record every finding and its disposition in
`contract-and-harness-review.md`; unresolved findings fail the phase.
Commit every review-driven fixture, support, retained-contract, test, or
CMake-registration fix as an atomic issue-family change in its owning task
before closing the report. After the last fix, repeat Step 3 in full so the
focused tests and generated evidence describe the reviewed current source.
Only that regenerated evidence may be committed. Before Step 5, status may
contain only the Task 6 evidence-tool, focused-test, generated-evidence, and
review paths listed above; an unstaged source or registration fix blocks
closeout.

- [ ] **Step 5: Commit phase evidence**

```console
$ git add \
    cxx/tools/evidence \
    cxx/docs/evidence \
    tests/cxx/test_ctest_gate.py \
    tests/cxx/test_contract_gate.py
```

```console
$ git commit -F - <<'EOF'
CXX(test[evidence]): Close contract gate

why: Require reproducible parity and real-tmux evidence before bakeoffs.

what:
- Record immutable CTest, source, tool, build, fixture, and Python gates
- Close independent review findings without local-path leakage
EOF
```
