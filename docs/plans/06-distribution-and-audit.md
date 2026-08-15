# Distribution and completion implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove self-contained headers, source and installed consumption,
relocation, C++20 ABI separation, the stable tmux matrix, reproducible builds,
CI enforcement, and independent final reviews.

**Architecture:** The production library remains one compiled target. Package
tests consume it as an installed config package, `add_subdirectory`, local
FetchContent source, and a vcpkg overlay. C++23 and C++20 are separately built
package identities with the same target alias but incompatible inline ABI
namespaces and output names.

**Tech Stack:** CMake 3.25+, CMakePresets, CPack-style staged installs without
requiring CPack, vcpkg manifest mode, C++23 `std::expected`, C++20
`tl::expected` 1.1.0, GCC 13 with libstdc++ 13, LLVM 18.1.3 with libc++ and
libc++abi 18.1, Ninja 1.11.1, GitHub Actions, tmux 3.2a through 3.7b, ASan,
UBSan, clang-format, and clang-tidy.
Concurrency contracts additionally run in an isolated TSan build.

## Fixed Package Contracts

CMake options are exactly:

- `LIBTMUX_BUILD_TESTS`
- `LIBTMUX_BUILD_EXAMPLES`
- `LIBTMUX_FETCH_DEPS`
- `LIBTMUX_WARNINGS_AS_ERRORS`
- `LIBTMUX_ENABLE_SANITIZERS`
- `LIBTMUX_ENABLE_THREAD_SANITIZER`
- `LIBTMUX_ENABLE_CLANG_TIDY`
- `LIBTMUX_CXX_STANDARD`

Tests and examples default on only when `PROJECT_IS_TOP_LEVEL`. Warnings,
sanitizers, thread sanitizer, clang-tidy, and GoogleTest never propagate through
`libtmux::libtmux`.

The C++23 package uses inline namespace `abi_v23` and output name
`libtmux-cxx23`. The C++20 package uses `abi_v20_tl`, output name
`libtmux-cxx20-tl`, and only the vendored expected header and license. Each
build exports alias `libtmux::libtmux`; the two identities cannot share one
build tree or install prefix.

`cxx/VERSION` is the sole package-version source. `cxx/cmake/Version.cmake`
validates it and fixes
`LIBTMUX_PACKAGE_VERSION_COMPATIBILITY=SameMinorVersion`; project metadata,
configured headers, package config/version files, installed identity markers,
vcpkg metadata, documentation, and evidence bind those values rather than
copying a version or compatibility policy. Package tests require the installed
version to resolve and reject a newer patch request, another minor, and another
major. A temporary package-version fixture with a nonzero installed patch also
proves that an older requested patch in the same major/minor resolves.

### Task 1: Prove every public header is self-contained

**Files:**

- Bind: `cxx/public-headers.json`
- Bind: `cxx/tools/headers/generate_umbrella.py`
- Create: `cxx/tests/consumers/headers/CMakeLists.txt`
- Create: `cxx/tools/headers/generate_consumers.py`
- Create: `tests/cxx/test_header_consumers.py`
- Create: generated sources under `cxx/tests/consumers/headers/generated/`
- Modify: `cxx/CMakeLists.txt`

- [ ] **Step 1: Register the generator-backed consumer target red**

`generate_consumers.py` reads the sorted `cxx/public-headers.json` registry; it
does not discover headers by walking the include tree. Each source includes
exactly one registered public header first, instantiates or names its documented
public types, and links `libtmux::libtmux`. A second aggregate source includes
headers in reverse registry order to expose hidden include ordering. An
umbrella-only consumer includes `libtmux/libtmux.hpp` and exercises the Server,
entity, snapshot, query, option, hook, and result declarations without any
other include. Tests reject registry/filesystem/install-manifest drift,
duplicate ownership, an ungenerated umbrella, and nondeterministic output.
The initial task checks registry/filesystem parity; package tasks supply an
install manifest and require its variant-specific header set to match exactly.
Registry checks distinguish committed/generated source headers from configured
headers such as `libtmux/config.hpp`; the latter must have both their declared
template and the active preset's generated build-tree output.

Before implementing the generator, add the CMake target and a custom command
that requires its generated source manifest. The target therefore exists, but
its build cannot produce the manifest until `generate_consumers.py` is
implemented. Register the built aggregate consumer as CTest name
`package.headers.current` with label `package-headers` before the red.

- [ ] **Step 2: Run the generator and registered target red**

```console
$ uv run pytest tests/cxx/test_header_consumers.py -v
```

Expected: FAIL importing `generate_consumers`. An unrelated collection failure
is not the intended Python red.

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target libtmux_header_consumers
```

Expected: FAIL because the registered custom command cannot generate the
consumer manifest. An unknown target is not the intended red result.

- [ ] **Step 3: Add deterministic generation and build all consumers**

Implement the generator and rerun the identical focused Python test:

```console
$ uv run pytest tests/cxx/test_header_consumers.py -v
```

Expected: all generator unit tests pass.

```console
$ uv run python cxx/tools/headers/generate_consumers.py \
    --registry cxx/public-headers.json \
    --include-root cxx/include \
    --build-include cxx/build/cxx-dev/generated/include \
    --variant cxx23 \
    --output cxx/tests/consumers/headers/generated \
    --check
```

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target libtmux_header_consumers
```

```console
$ ctest \
    --preset cxx-dev \
    -R '^package\.headers\.current$' \
    --no-tests=error \
    --output-on-failure
```

Expected: all sources compile under warnings-as-errors with no umbrella-header
dependency.

```console
$ uv run python cxx/tools/headers/generate_umbrella.py \
    --registry cxx/public-headers.json \
    --output cxx/include/libtmux/libtmux.hpp \
    --check
```

- [ ] **Step 4: Commit header proof**

```console
$ git add \
    cxx/tests/consumers/headers \
    cxx/tools/headers/generate_consumers.py \
    tests/cxx/test_header_consumers.py \
    cxx/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(test[headers]): Prove self containment

why: Make every installed header safe to include independently.

what:
- Generate one compile consumer per public header
- Test direct and reverse-order inclusion under strict warnings
EOF
```

### Task 2: Export and consume the installed CMake package

**Files:**

- Bind: `cxx/VERSION`
- Bind: `cxx/cmake/Version.cmake`
- Create: `cxx/cmake/libtmuxConfig.cmake.in`
- Create: `cxx/cmake/Install.cmake`
- Create: `cxx/tests/consumers/find_package/CMakeLists.txt`
- Create: `cxx/tests/consumers/find_package/main.cpp`
- Create: `cxx/tools/package/run_installed_consumer.py`
- Create: `tests/cxx/test_installed_consumer.py`
- Modify: `cxx/CMakeLists.txt`
- Modify: `cxx/CMakePresets.json`

- [ ] **Step 1: Run the staged consumer red**

```console
$ uv run pytest tests/cxx/test_installed_consumer.py -v
```

Expected: FAIL because no package config or install manifest exists.

- [ ] **Step 2: Add target export and package configuration**

Use `GNUInstallDirs`, build/install include interfaces,
`configure_package_config_file`, `write_basic_package_version_file`, and
`install(EXPORT ...)`. Install public and generated headers, the compiled
library, configuration identity, schema, CMake config, and license. Do not
install tests, examples, spikes, Python tooling, or test dependencies.
Read the version and `SameMinorVersion` compatibility argument only through
`Version.cmake`. The package-version tests request the installed version, a
newer patch, an adjacent minor, and an adjacent major; only the installed
version resolves. A generated nonzero-patch fixture additionally requires an
older patch in its major/minor to resolve. A duplicated version literal or
compatibility policy outside the authoritative files fails.

Rerun the identical focused consumer test after implementing the export and
package configuration:

```console
$ uv run pytest tests/cxx/test_installed_consumer.py -v
```

Expected: all installed-consumer unit tests pass.

- [ ] **Step 3: Configure, compile, link, and run the staged consumer**

```console
$ uv run python cxx/tools/package/run_installed_consumer.py \
    --source . \
    --build-root cxx/build/installed-consumer
```

Expected: the orchestrator runs separate producer and consumer CMake trees.
The isolated consumer resolves only `find_package(libtmux CONFIG
REQUIRED)`, links `libtmux::libtmux`, runs against a real scoped tmux server,
and finds no source/build path in installed CMake files.

- [ ] **Step 4: Commit package config**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/CMakePresets.json \
    cxx/cmake/libtmuxConfig.cmake.in \
    cxx/cmake/Install.cmake \
    cxx/tests/consumers/find_package \
    cxx/tools/package/run_installed_consumer.py \
    tests/cxx/test_installed_consumer.py
```

```console
$ git commit -F - <<'EOF'
CXX(chore[cmake]): Export package config

why: Support isolated installed consumers through one namespaced target.

what:
- Install headers, library, schema, configuration, and exported targets
- Build and run a package-config consumer from a staged prefix
EOF
```

### Task 3: Prove install relocation

**Files:**

- Create: `cxx/tools/package/relocate_install.py`
- Create: `tests/cxx/test_relocated_consumer.py`
- Create: `cxx/tests/consumers/relocated/CMakeLists.txt`
- Create: `cxx/tests/consumers/relocated/main.cpp`
- Modify: `cxx/CMakePresets.json`

- [ ] **Step 1: Make a source-prefix leak fail**

The test stages an install, moves the complete prefix to a different temporary
directory, deletes the original stage, scans text and binary metadata for old
paths, then configures and runs the relocated consumer.

- [ ] **Step 2: Run the relocation test red**

```console
$ uv run pytest tests/cxx/test_relocated_consumer.py -v
```

Expected: FAIL for missing relocation support or retained build/source paths.

- [ ] **Step 3: Fix only relocatability defects and rerun**

Rerun the identical focused relocation test after implementing the relocation
tool and fixing only the defects it exposes:

```console
$ uv run pytest tests/cxx/test_relocated_consumer.py -v
```

Expected: all relocation unit tests pass.

```console
$ uv run python cxx/tools/package/relocate_install.py \
    --source . \
    --build-root cxx/build/relocation
```

Expected: configure, link, execution, path scan, and RPATH inspection all pass.

- [ ] **Step 4: Commit relocation proof**

```console
$ git add \
    cxx/CMakePresets.json \
    cxx/tools/package/relocate_install.py \
    tests/cxx/test_relocated_consumer.py \
    cxx/tests/consumers/relocated
```

```console
$ git commit -F - <<'EOF'
CXX(test[package]): Prove relocation

why: Ensure installed configuration does not depend on its staging path.

what:
- Move and rescan a complete install prefix
- Configure, link, and run a consumer after deleting the original stage
EOF
```

### Task 4: Add source-tree consumers

**Files:**

- Create: `cxx/tests/consumers/add_subdirectory/CMakeLists.txt`
- Create: `cxx/tests/consumers/add_subdirectory/main.cpp`
- Create: `cxx/tests/consumers/fetchcontent/CMakeLists.txt`
- Create: `cxx/tests/consumers/fetchcontent/main.cpp`
- Create: `cxx/tools/package/run_source_consumers.py`
- Create: `tests/cxx/test_source_consumers.py`
- Modify: `cxx/CMakePresets.json`

- [ ] **Step 1: Configure both consumers before support exists**

```console
$ uv run pytest tests/cxx/test_source_consumers.py -v
```

Expected: FAIL for missing consumers or leaked top-level test options.

- [ ] **Step 2: Implement isolated source consumption**

The first uses `add_subdirectory` on `cxx/`. The second uses local
`FetchContent_Declare` with `SOURCE_DIR` and `SOURCE_SUBDIR cxx`; it performs no
network request. Both require tests/examples off by default, build and link the
single target, execute against real tmux, and inspect directory properties for
warning/sanitizer/tidy leakage.

Rerun the identical focused source-consumer test after implementation:

```console
$ uv run pytest tests/cxx/test_source_consumers.py -v
```

Expected: all source-consumer unit tests pass.

- [ ] **Step 3: Run and commit**

```console
$ uv run python cxx/tools/package/run_source_consumers.py \
    --source . \
    --build-root cxx/build/source-consumers
```

Expected: both consumers configure, compile, link, and run.

```console
$ git add \
    cxx/CMakePresets.json \
    cxx/tests/consumers/add_subdirectory \
    cxx/tests/consumers/fetchcontent \
    cxx/tools/package/run_source_consumers.py \
    tests/cxx/test_source_consumers.py
```

```console
$ git commit -F - <<'EOF'
CXX(test[package]): Add source consumers

why: Support nested CMake and local FetchContent without option leakage.

what:
- Build and run add-subdirectory and source-subdir consumers
- Prove top-level tests and first-party flags stay private
EOF
```

### Task 5: Prove static and shared library modes

**Files:**

- Create: `cxx/tools/package/audit_binary.py`
- Create: `cxx/tools/package/run_library_modes.py`
- Create: `tests/cxx/test_library_modes.py`
- Create: `cxx/tests/consumers/library_modes/CMakeLists.txt`
- Create: `cxx/tests/consumers/library_modes/main.cpp`
- Modify: `cxx/CMakePresets.json`

- [ ] **Step 1: Run the missing mode orchestrator**

```console
$ uv run pytest tests/cxx/test_library_modes.py -v
```

Expected: FAIL importing the library-mode orchestrator. An unrelated fixture or
collection failure is not the intended red result.

- [ ] **Step 2: Build, install, inspect, and run both modes**

The orchestrator invokes each single-tree CMake workflow separately. Verify the
expected archive/shared object exists, exported target metadata is correct, no
unexpected RPATH or third-party dependency appears, no warning or sanitizer
flag leaks, and the same consumer source runs in each mode.

Implement the orchestrator and binary audit, then rerun the identical focused
test:

```console
$ uv run pytest tests/cxx/test_library_modes.py -v
```

Expected: all library-mode unit tests pass.

```console
$ uv run python cxx/tools/package/run_library_modes.py
```

Expected: the isolated `cxx-static` and `cxx-shared` workflows both pass their
consumer and binary audits.

- [ ] **Step 3: Commit the mode matrix**

```console
$ git add \
    cxx/tools/package/audit_binary.py \
    cxx/tools/package/run_library_modes.py \
    tests/cxx/test_library_modes.py \
    cxx/tests/consumers/library_modes \
    cxx/CMakePresets.json
```

```console
$ git commit -F - <<'EOF'
CXX(test[package]): Prove library modes

why: Keep static and shared packages equivalent and dependency-clean.

what:
- Build, install, inspect, and run both production library modes
- Reject unexpected RPATHs, dependencies, or propagated build flags
EOF
```

### Task 6: Add the separate C++20 package

**Files:**

- Bind: `cxx/VERSION`
- Bind: `cxx/cmake/Version.cmake`
- Bind: `cxx/include/libtmux/config.hpp.in`
- Bind: every existing public and generated header registered in
  `cxx/public-headers.json`
- Bind: `cxx/tools/headers/generate_umbrella.py`
- Modify: `cxx/public-headers.json`
- Modify generated: `cxx/include/libtmux/libtmux.hpp`
- Create: `cxx/third_party/tl-expected-1.1.0/include/tl/expected.hpp`
- Create: `cxx/third_party/tl-expected-1.1.0/LICENSE`
- Create: `cxx/third_party/tl-expected-1.1.0/source.json`
- Create: `cxx/include/libtmux/compat/expected.hpp`
- Create: `cxx/tests/consumers/cxx20/CMakeLists.txt`
- Create: `cxx/tests/consumers/cxx20/main.cpp`
- Create: `cxx/tests/consumers/abi_mismatch/`
- Create: `tests/cxx/test_cxx20_package.py`
- Create: `cxx/tools/package/run_variant_matrix.py`
- Create: `tests/cxx/test_package_variant_matrix.py`
- Create: `cxx/docs/guides/consuming.md`
- Create: `cxx/docs/guides/consumer-cases.json`
- Create: `cxx/tools/docs/check_consumer_guides.py`
- Create: `tests/cxx/test_consumer_guides.py`
- Modify: `cxx/tools/package/run_installed_consumer.py`
- Modify: `cxx/tools/package/run_source_consumers.py`
- Modify: `cxx/CMakeLists.txt`
- Modify: `cxx/CMakePresets.json`
- Modify: `cxx/cmake/Install.cmake`

**Pinned source:** tag `v1.1.0`, commit
`292eff8bd8ee230a7df1d6a1c00c4ea0eb2f0362`, archive SHA-256
`1db357f46dd2b24447156aaf970c4c40a793ef12a8a9c2ad9e096d9801368df6`.

- [ ] **Step 1: Register and build the C++20 contract red**

Add the `cxx20` configure/build presets and register target
`libtmux_cxx20_contract` from the public-header consumer before creating the
compatibility header or vendored fallback. Register CTest name
`package.cxx20.contract` with label `package-cxx20` in the same configure step.

```console
$ uv run pytest tests/cxx/test_cxx20_package.py -v
```

Expected: FAIL because the compatibility header and pinned fallback are absent.
An unrelated fixture or collection failure is not the intended Python red.

```console
$ cmake --preset cxx20
```

```console
$ cmake --build --preset cxx20 --target libtmux_cxx20_contract
```

Expected: FAIL compiling the named target because the declared C++20 branch
cannot resolve `libtmux/compat/expected.hpp` and the pinned fallback, not
because the target is unknown or existing public headers lack ABI wrapping.

- [ ] **Step 2: Vendor only the required source and license**

Verify the archive hash before extraction. Retain the upstream header and
license unmodified plus `source.json`; do not vendor upstream tests, CMake,
examples, Git metadata, or binaries.

- [ ] **Step 3: Select expected and ABI identity at configure time**

`LIBTMUX_CXX_STANDARD=23` selects `abi_v23` and `libtmux-cxx23`.
`LIBTMUX_CXX_STANDARD=20` selects `abi_v20_tl` and `libtmux-cxx20-tl`. Generated
configuration controls every public type namespace and result alias. The
package core already declares every public and generated header inside
`LIBTMUX_ABI_NAMESPACE` and defines `result` through a config-selected expected
facade: the active C++23 branch uses `std::expected`, while the dormant C++20
branch names `<libtmux/compat/expected.hpp>`. This task binds that preexisting
configuration and every existing header, creates the fallback header/vendor,
and enables the declared C++20 branch without rewriting any existing public or
generated header. A full-header source-diff scan and both variant consumer
builds enforce that invariant. A failure requiring an existing header edit
returns to the package-core task instead of being patched here.

Preserve the package-core target mapping: `20` selects `cxx_std_20`, `23`
selects `cxx_std_23`, both require the selected standard, and extensions remain
off. The C++20 contract contains `static_assert(__cplusplus == 202002L);`; its
recorded compile command must contain the toolchain's C++20 flag and must not
contain a C++23 or draft-C++23 flag. The C++23 contract asserts a language mode
newer than C++20. These checks bind ABI selection to the actual compiler mode
instead of trusting the option name.

Register the new compatibility header as C++20-only in
`cxx/public-headers.json` and regenerate the conditional umbrella in the same
commit. The registry and generator remain authoritative; no handwritten
umbrella include is allowed.

- [ ] **Step 4: Prove mismatch fails to link**

Build one object against each configuration, intentionally combine each with
the other library, and require unresolved ABI-qualified symbols. Also require
install refusal when the other identity marker already exists in the prefix.

- [ ] **Step 5: Run the C++20 proof**

Rerun the identical focused package test after binding the expected provider,
ABI identity, compile feature, and mismatch fixtures:

```console
$ uv run pytest tests/cxx/test_cxx20_package.py -v
```

Expected: all C++20 package unit tests pass.

```console
$ cmake --preset cxx20
```

```console
$ cmake --build --preset cxx20 --target libtmux_cxx20_contract
```

```console
$ ctest \
    --preset cxx20 \
    -R '^package\.cxx20\.contract$' \
    --no-tests=error \
    --output-on-failure
```

Expected: C++20 headers, tests, package consumer, and negative ABI tests pass;
C++23 dependency scan still reports no third-party production dependency.

- [ ] **Step 6: Re-run package contracts for both ABI variants**

```console
$ uv run pytest tests/cxx/test_package_variant_matrix.py -v
```

Expected: FAIL importing `run_variant_matrix`. An unrelated fixture or
collection failure is not the intended red result.

`run_variant_matrix.py` creates isolated C++23/C++20 static/shared build and
install trees. For each, it compares the sorted public-header registry to the
install manifest, compiles one source per installed public header, and compiles
an umbrella-only source. It requires the correct generated `config.hpp`, ABI
namespace, output name, result provider, compatibility-header presence or
absence, and `SameMinorVersion` package behavior. It then runs relocation,
system `find_package`, local `FetchContent`, and binary audits with the same
consumer source. Tests reject a missing or extra header/package file, a stale
umbrella, cross-variant header leakage, an incorrect package-version decision,
or a shared build tree or install prefix.

It also hashes every preexisting public/generated source header before and
after configuration and all four builds. Any change fails, even if consumers
pass. The only new public source header permitted by this task is the registered
C++20 compatibility header; the umbrella may change only as deterministic
generator output from the registry.

Implement the matrix orchestrator and rerun the identical focused test:

```console
$ uv run pytest tests/cxx/test_package_variant_matrix.py -v
```

Expected: all variant-matrix unit tests pass.

```console
$ uv run python cxx/tools/package/run_variant_matrix.py \
    --standards 23 20 \
    --modes static shared \
    --output cxx/build/package-variants.json
```

Expected: all four variants pass header self-containment, install relocation,
system and FetchContent consumer execution, package-version selection, and
dependency/RPATH audits without sharing a build tree or install prefix. The
runner atomically writes the normalized current execution record consumed by
the guide checker.

- [ ] **Step 7: Bind consumer documentation to executed cases**

```console
$ uv run pytest tests/cxx/test_consumer_guides.py -v
```

Expected: FAIL importing the guide checker. An unrelated fixture or collection
failure is not the intended red result.

`consuming.md` documents system installation plus `find_package`, local
`FetchContent` with `SOURCE_SUBDIR cxx`, and C++20 selection for each. Every
command and CMake fragment has a stable case ID in `consumer-cases.json`.
`check_consumer_guides.py` requires each ID to name a successful current
`run_variant_matrix.py` record; no prose-only snippet passes. Task 7 adds the
vcpkg section only after its overlay and execution record exist.

Implement the guide checker and rerun the identical focused test:

```console
$ uv run pytest tests/cxx/test_consumer_guides.py -v
```

Expected: all consumer-guide unit tests pass.

```console
$ uv run python cxx/tools/docs/check_consumer_guides.py \
    --guide cxx/docs/guides/consuming.md \
    --cases cxx/docs/guides/consumer-cases.json \
    --variant-record cxx/build/package-variants.json
```

Expected: system and FetchContent cases are executed for C++23 and C++20 with
no missing, pending, or prose-only case.

- [ ] **Step 8: Commit the C++20 package and guide**

```console
$ git add \
    cxx/third_party \
    cxx/include/libtmux/compat \
    cxx/public-headers.json \
    cxx/include/libtmux/libtmux.hpp \
    cxx/tests/consumers/cxx20 \
    cxx/tests/consumers/abi_mismatch \
    cxx/tools/package/run_variant_matrix.py \
    cxx/tools/package/run_installed_consumer.py \
    cxx/tools/package/run_source_consumers.py \
    cxx/docs/guides/consuming.md \
    cxx/docs/guides/consumer-cases.json \
    cxx/tools/docs/check_consumer_guides.py \
    tests/cxx/test_cxx20_package.py \
    tests/cxx/test_package_variant_matrix.py \
    tests/cxx/test_consumer_guides.py \
    cxx/CMakeLists.txt \
    cxx/CMakePresets.json \
    cxx/cmake/Install.cmake
```

```console
$ git commit -F - <<'EOF'
CXX(feat[compat]): Add C++20 package

why: Support C++20 without making expected representation ABI-ambiguous.

what:
- Vendor only pinned tl expected source and license
- Separate namespaces, binaries, installs, and mismatch link behavior
EOF
```

### Task 7: Add the vcpkg overlay consumer

**Files:**

- Bind: `cxx/VERSION`
- Modify: `cxx/docs/guides/consuming.md`
- Modify: `cxx/docs/guides/consumer-cases.json`
- Create: `cxx/vcpkg/ports/libtmux/portfile.cmake`
- Create: `cxx/vcpkg/ports/libtmux/vcpkg.json`
- Create: `cxx/tests/consumers/vcpkg/vcpkg.json`
- Create: `cxx/tests/consumers/vcpkg/vcpkg-configuration.json`
- Create: `cxx/tests/consumers/vcpkg/CMakeLists.txt`
- Create: `cxx/tests/consumers/vcpkg/main.cpp`
- Create: `cxx/tools/package/generate_vcpkg_metadata.py`
- Create: `cxx/tools/package/run_vcpkg_consumer.py`
- Create: `tests/cxx/test_generate_vcpkg_metadata.py`
- Create: `tests/cxx/test_vcpkg_consumer.py`
- Modify: `cxx/CMakePresets.json`

**Pinned registry:** tag `2026.07.29`, commit
`9e593bb18ea69cc5095e012465dcd675a822ed0d`, archive SHA-256
`6b1a5b0170fda8e585b258fa416e4251197bba4633414d2ac00f02625c78194e`.

- [ ] **Step 1: Run overlay installation red**

```console
$ uv run pytest \
    tests/cxx/test_generate_vcpkg_metadata.py \
    tests/cxx/test_vcpkg_consumer.py \
    -v
```

Expected: FAIL importing the metadata generator or because the overlay port is
unresolved. An unrelated fixture or collection failure is not the intended red.

- [ ] **Step 2: Add manifest-mode overlay packaging**

The port installs from the supplied local source, forwards static/shared and
C++ standard features, fixes CMake config paths, removes debug header/config
duplicates, and verifies licenses. The consumer uses only manifest mode and the
pinned registry baseline. Port and consumer metadata use the package version
generated from `cxx/VERSION`; tests reject a hand-edited or divergent version.
`generate_vcpkg_metadata.py` owns the tracked port manifest, emits sorted JSON
with one trailing newline, and supports byte-for-byte `--check` regeneration.
Only after the overlay exists, add its exact manifest-mode command and CMake
fragment to `consuming.md` with a stable case ID.

Rerun the identical focused command after implementing the metadata generator,
overlay, and consumer orchestrator:

```console
$ uv run pytest \
    tests/cxx/test_generate_vcpkg_metadata.py \
    tests/cxx/test_vcpkg_consumer.py \
    -v
```

Expected: all focused vcpkg tests pass.

```console
$ uv run python cxx/tools/package/generate_vcpkg_metadata.py \
    --version-file cxx/VERSION \
    --output cxx/vcpkg/ports/libtmux/vcpkg.json \
    --check
```

- [ ] **Step 3: Build and run the consumer matrix**

```console
$ uv run python cxx/tools/package/run_vcpkg_consumer.py \
    --standards 23 20 \
    --modes static shared \
    --output cxx/build/vcpkg-consumer.json
```

Expected: four isolated overlay installs, consumer configures, links,
executions, and package audits pass against the pinned registry. The runner
atomically writes the normalized current execution record consumed by the guide
checker.

```console
$ uv run python cxx/tools/docs/check_consumer_guides.py \
    --guide cxx/docs/guides/consuming.md \
    --cases cxx/docs/guides/consumer-cases.json \
    --variant-record cxx/build/package-variants.json \
    --vcpkg-record cxx/build/vcpkg-consumer.json
```

Expected: the documented system, FetchContent, and vcpkg C++23/C++20 cases all
name successful current execution records; no pending case remains.

- [ ] **Step 4: Commit vcpkg support**

```console
$ git add \
    cxx/CMakePresets.json \
    cxx/docs/guides/consuming.md \
    cxx/docs/guides/consumer-cases.json \
    cxx/vcpkg \
    cxx/tests/consumers/vcpkg \
    cxx/tools/package/generate_vcpkg_metadata.py \
    cxx/tools/package/run_vcpkg_consumer.py \
    tests/cxx/test_generate_vcpkg_metadata.py \
    tests/cxx/test_vcpkg_consumer.py
```

```console
$ git commit -F - <<'EOF'
CXX(chore[vcpkg]): Add overlay port

why: Prove manifest-mode consumption through a pinned package registry.

what:
- Add local-source overlay packaging for both library modes
- Build, run, and audit a pinned vcpkg consumer
EOF
```

### Task 8: Pin quality tools and complete CMake workflows

**Files:**

- Modify: `.tool-versions`
- Modify: `.clang-format`
- Modify: `.clang-tidy`
- Create: `cxx/tools/toolchain.lock.json`
- Create: `cxx/cmake/Sanitizers.cmake`
- Create: `cxx/cmake/StaticAnalysis.cmake`
- Create: `cxx/tools/quality/verify_tools.py`
- Create: `cxx/tools/quality/run_quality.py`
- Create: `tests/cxx/test_verify_tools.py`
- Create: `tests/cxx/test_run_quality.py`
- Modify: `cxx/CMakePresets.json`

**Pinned tools:** CMake 3.28.3, Ninja 1.11.1, GCC 13 with libstdc++ 13, Clang
18.1.3 with libc++ and libc++abi 18.1, clang-format 18.1.3, and clang-tidy
18.1.3.

- [ ] **Step 1: Reject a wrong binary version**

Tests place fake version-reporting binaries first on `PATH` and require stable
violations. A floating LLVM major does not satisfy the lock. The Clang lane
also rejects libstdc++, a libc++ major/minor other than 18.1, or a resolved
libc++ header/library digest that differs from the recorded environment lock.

```console
$ uv run pytest \
    tests/cxx/test_verify_tools.py \
    tests/cxx/test_run_quality.py \
    -v
```

Expected: FAIL importing the tool verifier or quality orchestrator. An unrelated
fixture or collection failure is not the intended red result.

- [ ] **Step 2: Implement and verify the quality tools**

Implement exact tool and standard-library verification plus the isolated
quality-workflow orchestrator, then rerun the identical focused command:

```console
$ uv run pytest \
    tests/cxx/test_verify_tools.py \
    tests/cxx/test_run_quality.py \
    -v
```

Expected: all focused quality-tool tests pass.

- [ ] **Step 3: Add configure/build/test/workflow presets**

Single-tree presets cover `cxx-dev`, `cxx-release`, `cxx20`, `cxx-sanitize`,
`cxx-tsan`, `cxx-tidy`, `cxx-static`, `cxx-shared`, `cxx-install`,
`cxx-relocation`, `cxx-source-consumers`, `cxx-vcpkg`, and
`cxx-reproducible`. Each earlier package task adds the preset it first uses;
this task consolidates their inheritance without changing their commands. Keep
build directories isolated. Multi-tree matrices are Python orchestrators, not
invalid CMake workflow presets: a workflow may contain only one configure step
and every later step must use that configure tree.

Compiler-matrix presets pair GCC 13 only with libstdc++ 13 and Clang 18.1.3
only with the `clang-libcxx.cmake` toolchain and libc++/libc++abi 18.1. Each
result records compiler, standard-library macro, resolved header digest, and
linked standard-library identity; a compiler-only version match is
insufficient.

- [ ] **Step 4: Run formatting, tidy, warnings, and sanitizer workflows**

`run_quality.py` invokes the format/tidy, ASan/UBSan, and TSan workflows in
separate build trees. TSan runs the `concurrency` label and is never combined
with ASan. It records each exact command and result.

```console
$ uv run python cxx/tools/quality/run_quality.py
```

Expected: exact tools run, formatting is clean, curated clang-tidy checks pass,
first-party warnings are errors, ASan/UBSan tests pass, and the independent TSan
concurrency suite reports no data race.

- [ ] **Step 5: Commit quality tooling**

```console
$ git add \
    .tool-versions \
    .clang-format \
    .clang-tidy \
    cxx/tools/toolchain.lock.json \
    cxx/cmake \
    cxx/tools/quality \
    tests/cxx/test_verify_tools.py \
    tests/cxx/test_run_quality.py \
    cxx/CMakePresets.json
```

```console
$ git commit -F - <<'EOF'
CXX(chore[quality]): Pin C++ tools

why: Make local and CI formatting, diagnostics, and sanitizers
     identical.

what:
- Verify exact CMake, compiler, formatter, tidy, and Ninja identities
- Add inherited presets for every blocking build and quality workflow
EOF
```

### Task 9: Pin and build the stable tmux matrix

**Files:**

- Create: `cxx/ci/tmux-sources.json`
- Create: `cxx/tools/tmux/build_tmux.py`
- Create: `tests/cxx/test_build_tmux.py`
- Create: `cxx/docs/evidence/tmux-sources.md`

**Stable locks:**

| Version | Commit                                     | Archive SHA-256                                                    |
| ------- | ------------------------------------------ | ------------------------------------------------------------------ |
| 3.2a    | `3b929f332aafa7f1080eacc31feb11ffbb1d1841` | `497bc4ee16f10b53b161bf0253b6f9e20cd0f86c5d0104f149212cb0778ae13a` |
| 3.3a    | `0b355ae8114511e1ff6359272b164f1cdf718e80` | `f9687493203f86d346791a9327cde9148b9b4be959381b1effc575a9364a043f` |
| 3.4     | `9ae69c3795ab5ef6b4d760f6398cd9281151f632` | `ec7ddf021a0a1d3778862feb845fd0c02759dcdb37ba5380ba4e0038164f7187` |
| 3.5     | `ac44566c9c7e3e94d23be6def4c7ae83472543f5` | `74460f85bd81d73661356f777cdada121033ba8b0bc9119991d9fb0b5381c35e` |
| 3.6     | `0dac7fe434d029a4f0b819cba8eb7963df291990` | `bb8e96da3809845c72b53589cd7f4c00d5e3cbbb88d3284e15e5a8c2220d471d` |
| 3.7     | `81f88f8517c9fc5371b56cf117530c6b477c96ac` | `123b9ed37c7ee957327a483024ea6ae0e6032348c02680be2ac91b9350e62d36` |
| 3.7a    | `0e418b62d259ce8da8970f75732cc6632ee4c3a0` | `6eb3d0fe4058586fc403ff40ce384188fc5da17610a69b3fbf4db2d9f95a20fd` |
| 3.7b    | `e802909de06012a4df6209d55e86487c56223163` | `156dc43dcbc7f06e35e1fae3118c44d77a370c46676b34b82bbafc4e608d8130` |

- [ ] **Step 1: Test source and binary identity enforcement**

Alter every tag, commit, and hash field in fixtures and require rejection. A
built binary is accepted only when its explicit path exists and `tmux -V`
matches the requested raw version.

```console
$ uv run pytest tests/cxx/test_build_tmux.py -v
```

Expected: FAIL importing `build_tmux` or resolving the absent stable-build
implementation. An unrelated fixture or collection failure is not the intended
red result.

- [ ] **Step 2: Implement hermetic stable builds**

Download exactly the locked archive, verify SHA-256 before extraction, verify
the archive tree identity against the recorded commit, build in a task-owned
cache, and return the exact binary path. Do not search `PATH` after selection.

- [ ] **Step 3: Resolve informational master once per run**

The master command resolves one upstream commit, downloads the archive for
that commit, records its hash, and uses only that identity for the run. Master
results are informational and never change stable locks.

- [ ] **Step 4: Run and commit source checks**

```console
$ uv run pytest tests/cxx/test_build_tmux.py -v
```

```console
$ git add \
    cxx/ci/tmux-sources.json \
    cxx/tools/tmux \
    tests/cxx/test_build_tmux.py \
    cxx/docs/evidence/tmux-sources.md
```

```console
$ git commit -F - <<'EOF'
CXX(chore[tmux]): Pin test versions

why: Test capability boundaries against immutable verified tmux sources.

what:
- Lock every stable tag to a commit and archive hash
- Verify selected binaries and resolve informational master once
EOF
```

### Task 10: Run differential parity across every stable tmux

**Files:**

- Bind: `cxx/parity/manifest.json`
- Bind: `cxx/public-headers.json`
- Bind: `cxx/tools/headers/inventory_public_api.py`
- Bind: `cxx/parity/shards.json`
- Bind: `cxx/tests/differential/scenario_registry.json`
- Bind: `cxx/tools/differential/check_coverage.py`
- Create: `cxx/tools/differential/run_matrix.py`
- Create: `tests/cxx/test_differential_matrix.py`
- Create: `cxx/docs/evidence/differential-matrix.schema.json`
- Create at execution: normalized matrix records under
  `cxx/docs/evidence/differential-matrix/`
- Create at execution:
  `cxx/docs/evidence/differential-matrix/matrix-coverage.json`

- [ ] **Step 1: Test matrix completeness**

The runner and coverage checker reject a missing stable version, implicit tmux
binary, reused socket selector, different binary digests with the same version
text, stale or wrong Python source identity, skipped scenario,
canonicalization outside declared pointers, or missing below/at-boundary
version case. For every non-metadata shard and every blocking stable tmux
version, they require each registered scenario, its exact Python driver, and
both registered adapter handlers to run successfully. Duplicate or orphaned
scenarios, tags, handlers, or drivers fail. Informational master is recorded
separately and cannot satisfy any stable cell.

- [ ] **Step 2: Run the focused matrix test red**

```console
$ uv run pytest tests/cxx/test_differential_matrix.py -v
```

Expected: FAIL importing `cxx.tools.differential.run_matrix`. An unrelated
fixture or collection error is not the intended red result.

- [ ] **Step 3: Implement and verify the matrix runner**

Implement the closed input validation, isolated-socket scheduling, adapter
provenance checks, deterministic records, and schema validation described in
Step 1. Then rerun the identical focused command:

```console
$ uv run pytest tests/cxx/test_differential_matrix.py -v
```

Expected: all focused runner tests pass.

- [ ] **Step 4: Run every scenario against separate sockets**

Reconfigure the development tree after all preceding distribution and header
changes, then rebuild the exact adapter consumed by the matrix:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target libtmux_differential_adapter
```

The matrix runner rejects the adapter unless its executable, registration,
source, compiler, and semantic-contract identities match this build.

```console
$ uv run python cxx/tools/differential/run_matrix.py \
    --sources cxx/ci/tmux-sources.json \
    --manifest cxx/parity/manifest.json \
    --shards cxx/parity/shards.json \
    --registry cxx/tests/differential/scenario_registry.json \
    --python-adapter cxx/tools/differential/python_reference.py \
    --cpp-adapter cxx/build/cxx-dev/tests/differential/cpp_adapter \
    --drivers tests/cxx/differential \
    --output cxx/docs/evidence/differential-matrix
```

Expected: Python and C++ records match or name only explicitly approved
adaptations for all eight blocking versions. Master is reported separately.

```console
$ uv run python cxx/tools/differential/check_coverage.py \
    --mode matrix \
    --manifest cxx/parity/manifest.json \
    --shards cxx/parity/shards.json \
    --registry cxx/tests/differential/scenario_registry.json \
    --python-adapter cxx/tools/differential/python_reference.py \
    --cpp-adapter cxx/build/cxx-dev/tests/differential/cpp_adapter \
    --results cxx/docs/evidence/differential-matrix \
    --sources cxx/ci/tmux-sources.json \
    --output cxx/docs/evidence/differential-matrix/matrix-coverage.json
```

Expected: the machine report proves the complete
stable-version-by-behavior-shard product and current scenario/handler/driver
digests.

- [ ] **Step 5: Commit stable matrix evidence**

```console
$ git add \
    cxx/tools/differential/run_matrix.py \
    tests/cxx/test_differential_matrix.py \
    cxx/docs/evidence/differential-matrix.schema.json \
    cxx/docs/evidence/differential-matrix
```

```console
$ git commit -F - <<'EOF'
CXX(test[parity]): Prove tmux matrix

why: Validate Python and C++ behavior at every supported capability
     boundary.

what:
- Run all canonical scenarios on eight immutable stable tmux binaries
- Separate blocking stable results from informational upstream master
EOF
```

### Task 11: Verify executable examples and API documentation

**Files:**

- Bind: `cxx/parity/manifest.json`
- Bind: `cxx/docs/guides/consuming.md`
- Bind: `cxx/docs/guides/consumer-cases.json`
- Bind: `cxx/tools/docs/check_consumer_guides.py`
- Modify: `cxx/examples/CMakeLists.txt`
- Create: `cxx/tools/docs/check_public_api.py`
- Create: `cxx/docs/conf.py`
- Create: `tests/cxx/test_public_api_docs.py`
- Create: `cxx/docs/api/index.md`

- [ ] **Step 1: Write independent API-coverage fixtures**

Require failures for missing/duplicate symbol ownership, missing doc anchors,
uncompiled examples, compiled but unexecuted examples, and cases not registered
with CTest. A separate fixture adds an exported C++ declaration to a temporary
public header without adding a manifest row. Corrupt only temporary headers and
manifest fixtures; this task treats the committed headers, manifest, and
reviewed mapping as read-only inputs.

- [ ] **Step 2: Run the documentation checker test red**

```console
$ uv run pytest tests/cxx/test_public_api_docs.py -v
```

Expected: FAIL importing `check_public_api`.

- [ ] **Step 3: Implement and verify the documentation checker**

Join the independent declaration inventory, canonical manifest owners,
documentation anchors, compiled example targets, and executed CTest case IDs.
Reject an extra declaration on either side, alias ownership, duplicate anchors,
and stale execution evidence. Rerun the focused test:

```console
$ uv run pytest tests/cxx/test_public_api_docs.py -v
```

Expected: all API-documentation coverage tests pass.

- [ ] **Step 4: Build and execute every public example**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target libtmux_examples
```

```console
$ ctest --preset cxx-dev -L example --no-tests=error --output-on-failure
```

Expected: every mapped case compiles and executes; real-tmux cases use only the
test-support fixture and bounded interactive client.

- [ ] **Step 5: Run the declaration-to-doc coverage gate**

Regenerate the public declaration inventory without passing the parity
manifest:

```console
$ uv run python cxx/tools/headers/inventory_public_api.py \
    --registry cxx/public-headers.json \
    --include-root cxx/include \
    --build-dir cxx/build/cxx-dev \
    --output cxx/build/public-api-inventory.json
```

```console
$ uv run python cxx/tools/docs/check_public_api.py \
    --manifest cxx/parity/manifest.json \
    --public-api cxx/build/public-api-inventory.json \
    --docs cxx/docs/api \
    --examples cxx/examples
```

Expected: every independently inventoried public declaration maps to exactly
one canonical manifest owner, existing documentation anchor, and executed case
ID, with no orphaned manifest declaration.

If the committed manifest is stale or lacks a mapping, stop and return the
finding to the owning parity shard. This task may add prose, anchors, example
registration, and execution evidence, but it must not edit, synchronize, or
reclassify `cxx/parity/mapping.json` or `manifest.json`. Task 14 performs the
single post-distribution evidence refresh after every source and registration
change is complete; it may refresh execution evidence and resynchronize the
unchanged reviewed classifications.

Re-run `check_consumer_guides.py` without editing the already executed consumer
guide or its case registry.

```console
$ uv run python cxx/tools/docs/check_consumer_guides.py \
    --guide cxx/docs/guides/consuming.md \
    --cases cxx/docs/guides/consumer-cases.json \
    --variant-record cxx/build/package-variants.json \
    --vcpkg-record cxx/build/vcpkg-consumer.json
```

- [ ] **Step 6: Build the C++ documentation as warnings-fatal HTML**

The isolated Sphinx/MyST tree includes every C++ API page, resolves internal
anchors and links, and maps every compilable C++ fence to an executed example
case. It writes only under `cxx/build/docs/`.

```console
$ uv run sphinx-build -W --keep-going -b html cxx/docs cxx/build/docs/html
```

Expected: all pages render with no missing toctree entry, anchor, link, or code
example mapping. This gate is separate from the Python `just build-docs` tree.

- [ ] **Step 7: Commit documentation proof**

```console
$ git add \
    cxx/examples \
    cxx/tools/docs \
    tests/cxx/test_public_api_docs.py \
    cxx/docs/conf.py \
    cxx/docs/api
```

```console
$ git commit -F - <<'EOF'
CXX(docs[examples]): Cover public API

why: Give every public C++ symbol executable documentation evidence.

what:
- Register all examples as CTest cases
- Reject missing or duplicate API, documentation, and example mappings
EOF
```

### Task 12: Add blocking C++ continuous integration

**Files:**

- Create: `.github/workflows/cxx.yml`
- Create: `cxx/tools/ci/__init__.py`
- Create: `cxx/tools/ci/verify_workflow.py`
- Create: `tests/cxx/test_cxx_workflow.py`

- [ ] **Step 1: Write workflow-policy tests**

Require GCC 13 with libstdc++ 13 and Clang 18.1.3 with libc++/libc++abi 18.1,
C++23 and C++20, static and shared, format, tidy, warnings-as-errors,
ASan/UBSan, a separate TSan concurrency job, real-tmux tests, differential
stable matrix, install/relocation/source/vcpkg consumers, examples, Python
gates, and docs. Require master to be informational and Linux to be the only
support claim. TSan cannot share a job or build tree with ASan.
Keep policy parsing and diagnostics in importable functions with executable
doctests under `cxx/tools/ci`; do not place Python tooling under `cxx/ci` where
the repository doctest gate will miss it.

```console
$ uv run pytest tests/cxx/test_cxx_workflow.py -v
```

Expected: FAIL because the workflow and policy verifier do not exist. An
unrelated fixture or collection failure is not the intended red result.

- [ ] **Step 2: Add one independent workflow**

Do not change tag, release, or publish workflows. Pin every GitHub Action to a
full commit. Use a change filter for C++ and shared Python parity inputs without
letting it skip a required status on pull requests.

- [ ] **Step 3: Validate workflow structure locally**

```console
$ uv run pytest tests/cxx/test_cxx_workflow.py -v
```

```console
$ uv run pytest --doctest-modules cxx/tools/ci -v
```

```console
$ uv run python -m cxx.tools.ci.verify_workflow .github/workflows/cxx.yml
```

Expected: every required job, matrix cell, immutable action pin, artifact, and
blocking/informational policy is present.

- [ ] **Step 4: Commit CI**

```console
$ git add \
    .github/workflows/cxx.yml \
    cxx/tools/ci/__init__.py \
    cxx/tools/ci/verify_workflow.py \
    tests/cxx/test_cxx_workflow.py
```

```console
$ git commit -F - <<'EOF'
CXX(chore[ci]): Add C++ gates

why: Enforce the supported compiler, language, tmux, and package matrix.

what:
- Add pinned Linux build, quality, sanitizer, parity, and consumer jobs
- Keep stable tmux blocking and upstream master informational
EOF
```

### Task 13: Prove generated and installed reproducibility

**Files:**

- Bind: `cxx/VERSION`
- Bind: `cxx/public-headers.json`
- Create: `cxx/tools/audit/check_generated.py`
- Create: `cxx/tools/audit/check_install.py`
- Create: `cxx/tools/audit/run_reproducible.py`
- Create: `tests/cxx/test_generated_reproducibility.py`
- Create: `tests/cxx/test_install_audit.py`
- Create: `cxx/docs/evidence/reproducibility.json`

- [ ] **Step 1: Inject timestamps and paths into test fixtures**

`check_generated.py` must catch both after generating twice in different
temporary directories. `check_install.py` must catch source/build strings,
absolute RPATH, symlinks, missing transitive headers, unexpected files, and
different relative hashes across two prefixes. For each package variant it
also rejects an install manifest that differs from the applicable
`public-headers.json` set, a stale umbrella, wrong `config.hpp` ABI selection,
wrong compatibility-header presence, a version not sourced from `cxx/VERSION`,
or package-version behavior other than `SameMinorVersion`.

```console
$ uv run pytest \
    tests/cxx/test_generated_reproducibility.py \
    tests/cxx/test_install_audit.py \
    -v
```

Expected: FAIL importing the generated-file or install-audit tools. An unrelated
fixture or collection failure is not the intended red result.

- [ ] **Step 2: Implement and verify the reproducibility tools**

Implement deterministic regeneration, install-tree inspection, and the
two-producer orchestrator, then rerun the identical focused command:

```console
$ uv run pytest \
    tests/cxx/test_generated_reproducibility.py \
    tests/cxx/test_install_audit.py \
    -v
```

Expected: all focused reproducibility tests pass.

- [ ] **Step 3: Run two clean out-of-tree builds and installs**

```console
$ uv run python cxx/tools/audit/run_reproducible.py \
    --source . \
    --build-root cxx/build/reproducible \
    --standards 23 20 \
    --modes static shared \
    --output cxx/docs/evidence/reproducibility.json
```

Expected: for each of the four variants, committed headers, schemas, docs,
relative install manifests, package selection decisions, and installed file
hashes from two independent producer/install trees match. Relative to the
pre-run task baseline, no checkout path changes except the declared normalized
evidence output; existing ignored build artifacts are not deleted or reused.

- [ ] **Step 4: Commit audit tooling and normalized evidence**

```console
$ git add \
    cxx/tools/audit \
    tests/cxx/test_generated_reproducibility.py \
    tests/cxx/test_install_audit.py \
    cxx/docs/evidence/reproducibility.json
```

```console
$ git commit -F - <<'EOF'
CXX(test[package]): Audit artifacts

why: Detect hidden paths, undeclared files, and nondeterministic
     generation.

what:
- Compare two clean generations and relocated install manifests
- Reject RPATH, symlink, dependency, path, and checkout leakage
EOF
```

### Task 14: Close independent reviews and completion audit

**Files:**

- Bind: `cxx/parity/mapping.json`
- Bind: `cxx/parity/approvals.json`
- Bind: `cxx/parity/shards.json`
- Bind: `cxx/parity/release-v0.62.0.json`
- Bind: `cxx/parity/development.json`
- Modify: `cxx/parity/evidence.json`
- Modify: `cxx/parity/manifest.json`
- Bind: `cxx/public-headers.json`
- Bind: `cxx/tests/differential/scenario_registry.json`
- Bind: `cxx/tests/differential/cpp_adapter.cpp`
- Bind: `tests/cxx/differential/`
- Bind: `cxx/tools/headers/inventory_public_api.py`
- Bind: `cxx/tools/evidence/parity_gates.py`
- Bind: `cxx/tools/differential/check_coverage.py`
- Bind: `cxx/tools/differential/python_reference.py`
- Bind: `cxx/tools/parity/check_api_coverage.py`
- Bind: `cxx/tools/parity/refresh_evidence.py`
- Bind: `cxx/tools/parity/write_completion_evidence.py`
- Bind: `cxx/docs/evidence/parity-completion.schema.json`
- Modify: `cxx/docs/evidence/parity-completion.json`
- Create: `cxx/docs/reviews/final-architecture.md`
- Create: `cxx/docs/reviews/final-cpp.md`
- Create: `cxx/tools/audit/completion.py`
- Create: `cxx/tools/audit/run_complete.py`
- Create: `cxx/tools/audit/completion-owned-paths.json`
- Create: `tests/cxx/test_completion_audit.py`
- Create: `tests/cxx/test_run_complete.py`
- Create: `cxx/docs/evidence/completion.json`
- Create at execution: `cxx/build/complete-run.json`

- [ ] **Step 1: Make missing live evidence fail**

```console
$ git status --short
```

Expected before creating any Task 14 path: no output.

The completion audit maps every approved-design requirement to an executable
command and artifact. Its unit test removes one evidence fixture and requires a
stable nonzero violation. Historical green text without a live command does
not satisfy a gate. A disposable-repository test proves that post-commit check
mode accepts only the clean tracked/untracked classification transition with
byte-identical content and rejects a changed file, unrelated status, or stale
run record. Begin this task only from a clean `HEAD`; the test suite requires
rejection when any preexisting tracked or untracked path would be folded into
the task-owned baseline.
`completion-owned-paths.json` includes the three refreshed durable parity paths
alongside the Task 14 reports and tools; it does not include bound inputs or
ignored run output.

Run the audit-tool tests before their implementations exist:

```console
$ uv run pytest \
    tests/cxx/test_completion_audit.py \
    tests/cxx/test_run_complete.py \
    -v
```

Expected: FAIL importing the completion checker or live-gate runner. An
unrelated fixture or collection failure is not the intended red result.

- [ ] **Step 2: Implement and verify the completion tools**

Implement the owned-path, dirty-baseline, current-evidence, and check-mode
contracts from Step 1, then rerun the identical focused command:

```console
$ uv run pytest \
    tests/cxx/test_completion_audit.py \
    tests/cxx/test_run_complete.py \
    -v
```

Expected: all focused completion-tool tests pass.

- [ ] **Step 3: Run a fresh architecture review**

The reviewer traces architecture decisions through production, parity,
packaging, the tmux matrix, examples, and CI. Fix issue families in separate
commits. Commit the report only when no finding remains unresolved.

- [ ] **Step 4: Run a fresh changed-line C++ review**

A different reviewer examines every changed C++ and CMake line for ownership,
UB, exception safety, process/pipe/signal/timeouts, synchronization, ABI,
header hygiene, compile cost, dependency leakage, naming, duplication,
comments, and review-hostile noise. Fix every finding; do not waive correctness,
lifetime, or packaging defects.

- [ ] **Step 5: Refresh post-distribution parity evidence**

After all review fixes are committed, rerun every parity CTest and local
differential scenario against the final distribution source:

```console
$ uv run python -m cxx.tools.evidence.parity_gates \
    --source-dir cxx \
    --shards cxx/parity/shards.json \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/parity-ctest-index.json
```

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target libtmux_differential_adapter
```

```console
$ uv run pytest tests/cxx/differential -v
```

Regenerate and verify the independent declaration and differential coverage
reports:

```console
$ uv run python cxx/tools/headers/inventory_public_api.py \
    --registry cxx/public-headers.json \
    --include-root cxx/include \
    --build-dir cxx/build/cxx-dev \
    --output cxx/build/public-api-inventory.json
```

```console
$ uv run python cxx/tools/parity/check_api_coverage.py \
    --manifest cxx/parity/manifest.json \
    --public-api cxx/build/public-api-inventory.json \
    --ctest-index cxx/build/evidence/parity-ctest-index.json \
    --output cxx/build/api-coverage.json
```

```console
$ uv run python cxx/tools/differential/check_coverage.py \
    --mode local \
    --manifest cxx/parity/manifest.json \
    --shards cxx/parity/shards.json \
    --registry cxx/tests/differential/scenario_registry.json \
    --python-adapter cxx/tools/differential/python_reference.py \
    --cpp-adapter cxx/build/cxx-dev/tests/differential/cpp_adapter \
    --drivers tests/cxx/differential \
    --results cxx/build/differential-results.json \
    --output cxx/build/differential-coverage.json
```

Refresh only execution records, then resynchronize the unchanged reviewed
mapping. The differential records bind `semantic_contract_sha256`, so this
evidence-only synchronization does not invalidate them:

```console
$ uv run python -m cxx.tools.parity \
    refresh-evidence \
    --manifest cxx/parity/manifest.json \
    --shards cxx/parity/shards.json \
    --ctest-index cxx/build/evidence/parity-ctest-index.json \
    --differential-index cxx/build/differential-results.json \
    --evidence cxx/parity/evidence.json
```

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

Require the semantic contract digest to remain byte-identical across the sync,
then regenerate the full parity completion record from the refreshed manifest
and evidence:

```console
$ uv run python -m cxx.tools.parity \
    verify \
    --manifest cxx/parity/manifest.json \
    --mode complete \
    --report cxx/build/parity-verify.json
```

```console
$ uv run python cxx/tools/parity/write_completion_evidence.py \
    --manifest cxx/parity/manifest.json \
    --evidence cxx/parity/evidence.json \
    --public-api cxx/build/public-api-inventory.json \
    --validator cxx/build/parity-verify.json \
    --ctest-index cxx/build/evidence/parity-ctest-index.json \
    --differential cxx/build/differential-results.json \
    --differential-coverage cxx/build/differential-coverage.json \
    --coverage cxx/build/api-coverage.json \
    --output cxx/docs/evidence/parity-completion.json
```

Expected: every durable parity execution record names the final distribution
source, while reviewed semantic classifications and
`semantic_contract_sha256` remain unchanged.

- [ ] **Step 6: Execute the complete live gate**

`run_complete.py` invokes each isolated CMake workflow and Python gate in the
declared order, records command argv, source/tool identity, build directory,
exit status, and output digest, and rejects a reused or stale result. It is the
multi-tree orchestrator; no `cxx-complete` CMake workflow is declared.
It redirects every mutable gate output into its new run-ID tree and reruns the
Step 5 parity pipeline in byte-for-byte check mode; it never rewrites the
durable parity sidecars or completion record during this gate.

After the two review reports, audit tools, owned-path manifest, and tests are
ready, `run_complete.py` captures the exact porcelain-v1 status and content
digests as its dirty baseline. That baseline must equal the repository-relative
paths in `completion-owned-paths.json` except the not-yet-written completion
evidence; a directory, wildcard, absolute path, or any unrelated
changed/untracked path fails. The runner never cleans,
restores, deletes, or stages a path. It chooses a previously nonexistent
run-ID directory and may write only that declared ignored
`cxx/build/complete/<run-id>/` tree plus `cxx/build/complete-run.json`; other
ignored artifacts are neither baseline inputs nor cleanup targets. It then
requires tracked/untracked status and every baseline file digest to be
unchanged.

```console
$ uv run python cxx/tools/audit/run_complete.py \
    --owned-paths cxx/tools/audit/completion-owned-paths.json \
    --output cxx/build/complete-run.json
```

```console
$ uv run python cxx/tools/audit/completion.py \
    --design cxx/docs/design/bakeoff-and-rewrite.md \
    --manifest cxx/parity/manifest.json \
    --owned-paths cxx/tools/audit/completion-owned-paths.json \
    --run-record cxx/build/complete-run.json \
    --output cxx/docs/evidence/completion.json
```

Expected: all C++23/C++20, compiler, quality, sanitizer, real-tmux, parity,
matrix, example, docs, package, consumer, vcpkg, reproducibility, and review
gates run now and exit zero. Both static/shared variants for both language
standards pass the per-installed-header and umbrella consumers, exact install
manifest, `config.hpp`/compatibility-header selection, isolated package
identity, relocation, and `SameMinorVersion` positive/negative requests.

`completion.py` verifies the run record's baseline digest and post-run equality,
then writes only `cxx/docs/evidence/completion.json`. It requires the status
after excluding that one declared output to equal the captured baseline and
records these semantics without claiming the in-progress task was clean.
Its `--check --require-clean` mode performs no write: after the task commit, it
requires a clean checkout, revalidates the committed evidence against the same
run record and current file digests, and permits only the tracked/untracked
classification change caused by committing byte-identical baseline files.

- [ ] **Step 7: Run the full Python workflow once more**

```console
$ uv run ruff format . --check
```

```console
$ env -u __MISE_ZSH_ACTIVATE_PATH -u __MISE_ORIG_PATH uv run pytest
```

```console
$ uv run ruff check .
```

```console
$ uv run mypy .
```

```console
$ just build-docs
```

```console
$ env -u __MISE_ZSH_ACTIVATE_PATH -u __MISE_ORIG_PATH uv run pytest
```

Expected: every command exits zero. Any failure blocks completion, even when
the C++-specific subset is green.

- [ ] **Step 8: Verify clean checkout and commit closeout evidence**

```console
$ git status --short
```

Expected before staging: exactly the repository-relative paths declared in
`completion-owned-paths.json` are changed, with no ignored-output path staged.
After the commit, output is empty. A nonempty status blocks completion; no
baseline allowance survives the atomic commit.

```console
$ git add \
    cxx/parity/evidence.json \
    cxx/parity/manifest.json \
    cxx/docs/evidence/parity-completion.json \
    cxx/docs/reviews/final-architecture.md \
    cxx/docs/reviews/final-cpp.md \
    cxx/tools/audit/completion.py \
    cxx/tools/audit/run_complete.py \
    cxx/tools/audit/completion-owned-paths.json \
    tests/cxx/test_completion_audit.py \
    tests/cxx/test_run_complete.py \
    cxx/docs/evidence/completion.json
```

```console
$ git commit -F - <<'EOF'
CXX(test[audit]): Prove completion gates

why: Require current evidence for every design, parity, and distribution
     claim.

what:
- Close independent architecture and changed-line C++ reviews
- Execute and record the complete live build, test, matrix, and package
  gate
EOF
```

- [ ] **Step 9: Revalidate committed evidence from a clean checkout**

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

Expected: the committed evidence matches the current byte-identical sources,
the live run record remains valid, and porcelain status is empty.
