# Contract and harness evidence review

Verdict: Ready
Unresolved findings: 0
Reviewed source: sha256:123c058b8560c803b3c35d621066b3d692c8e3191421d781ad4f9a4c9e5d4249
Evidence core: sha256:26396124c9aec5b14a74bd8101de181400220a1d3763ad3906d1682fff186b2c

## Scope

The pending evidence record was checked against the Task 6 plan, the current
evidence producers, and their focused tests. Its canonical source inventory,
Git commit and tree, command matrix, tool identities, claims, and digest
projections are internally consistent. The proved claims remain limited to the
current Linux harness and test-only contract phase; the record explicitly
excludes production parity, API, ABI, relocation, matrix, and final
reproducibility claims.

## Evidence

The independently reconstructed 247-entry source inventory matches the pending
record byte for byte. The reviewable-source digest is the exact pending source
digest, and independent evidence-core and final projections reproduce their
published digests. No entry exists under `include`, `src`, or
`cxx/spikes`.

All 33 command records appear once, in the required order, with zero exit
status and normalized public tool versions. The parity manifest, inputs, and
six sidecars are bound to the live Python contract. Git, Python, C, C++, CMake,
CTest, libc++, tmux, and configured Ninja identities are retained across their
uses. The public JSON contains no absolute path, personal identifier, email
address, non-finite value, or raw command path.

The development CTest gate contains 42 matching registered and executed test
IDs. The sanitizer and thread-sanitizer real-tmux gates each contain 40
matching IDs and bind both name-selected and exact-path fixtures to the same
tmux version and executable digest. Every named gate record equals its
content-addressed leaf; registry, JUnit, execution, build snapshot, executable,
and gate digests agree. All three JUnit suites report no failure, error, skip,
or disabled case.

## Trusted CTest boundary

`ctest_gate.py` is the trusted producer for selected-test discovery and result
interpretation. It performs one JSON registry discovery and one execution with
the identical selector, validates the JUnit result and unchanged build inputs,
then publishes the registry, JUnit, and normalized gate record as an immutable
content-addressed leaf. The aggregate gate revalidates the named record, leaf
bytes, digests, selected IDs, execution projection, and retained files. Raw
registry and JUnit paths stay in ignored artifacts and are not public claims.

## Findings and dispositions

- Closed: review and source identities require the canonical review entry,
  exact review bytes, a derived reviewable snapshot, and the complete source
  snapshot. Re-digesting an unrelated review, omitting its entry, or adding an
  unexpected source symlink is rejected.
- Closed: filesystem checks independently reject files, directories, and
  symlinks at every reserved production path before and after Git inventory
  capture. Git excludes cannot hide production C++ code.
- Closed: public and imported CTest schemas require exact integer versions.
  Live uv target metadata and the fixed CTest Kitware paragraph normalize to
  pinned public versions; arbitrary and multiline suffixes are rejected.
- Closed: module execution, sanitized and retained Git, parity regeneration,
  logical tool attribution, immutable CTest records, retained executable
  lifetimes, finite JSON, and transactional publication, rollback, and cleanup
  have executable positive and adversarial coverage.
- Closed: a transient escaped-holder sanitizer failure was non-reproducible.
  Fifty serial repetitions, 80 concurrent repetitions, the 40/40 sanitizer
  lane, and the strict recorder passed without sanitizer diagnostics or source
  change.
- Accepted Minor: configured Ninja is retained, version-pinned, and identical
  across presets, but it may differ from the separately recorded logical Ninja
  executable. The evidence reports these as separate command identities and
  does not claim pathname identity.
- Accepted Minor: the aggregate runner remains large. Splitting it is deferred
  as maintainability work because its checkpoints and transaction boundaries
  are directly exercised.
- Accepted Minor: the CTest producer rejects two unique test IDs that share an
  identical command tuple. Current selected registries contain no such pair;
  broader generic compatibility is deferred.
- Accepted Minor: the private raw CTest record remains extensible at its top
  level. Its complete bytes are content-addressed, while the committed public
  projection uses a closed schema; further private-schema hardening is
  deferred.
