# Transport adversarial review

Status: Ready
Unresolved findings: 0
Reviewed source commit: 90ffde066931501c5a4c5236cbd14d2f9466d748
Reviewed source tree: 579a9ae9043fbd102e6aacaec5e8cf80b59bf3ae
Reviewed source manifest: sha256:e892775406ea9ccc23593d125765b846a2daef6897040e8598759a3f2423f45f
Decision core: sha256:b54a7478bb94014a0a66b139d985f9bd74749fd775cb39b84e0eb0f42c6fa181
Findings: 4

## Review axes

- lifetime_ownership: no_finding [transport-sanitize, transport-tsan]
- hidden_serialization: no_finding [transport.measurements.v1]
- diagnostic_reentrancy: no_finding [transport-tsan]
- exception_containment: no_finding [transport-sanitize]
- timeout_certainty: finding_recorded [transport-sanitize, graft.control_mode]
- transport_leakage: no_finding [transport.measurements.v1]
- control_group_attribution: no_finding [graft.control_mode, graft.engine_ops]
- engine_coupling: no_finding [graft.engine_ops]
- measurement_fairness: finding_fixed [transport.measurements.v1, follow_up.dispatch_overhead_scale.result]

## Disposition

Three axes produced findings and each was fixed or recorded before selection.

The gate validator required the retained `ctest --show-only=json-v1`
registration to be canonical JSON. No real capture satisfies that, and the
harness fixture happened to synthesise canonical bytes, so the constraint
looked satisfied until it met a live gate. A retained capture is evidence
precisely because it is verbatim, so the registration is now bound by its
digest and its parsed semantics and the fixture emits CTest's own encoding.

Two of the three build trees had been configured before the presets pinned a C
compiler. Reconfiguring such a tree makes CMake delete the cache and re-detect,
which silently selected the system GNU toolchain. The project options guard
turned that into a hard error rather than letting a GCC-built tree supply
evidence for a Clang and libc++ comparison. Every tree was removed and
reconfigured from scratch, and all three now carry one compiler metadata
digest.

The decisive wrapper runtime axis was unanchored. It separates the contenders
by single-digit nanoseconds per call, and nothing measured the process launch
each dispatched command performs, so an ordering within noise could have chosen
a winner. The `dispatch_overhead_scale` follow-up closes this against a
measured launch cost and its result is bound into this decision.

One control-mode graft deadline test passes under the sanitizer preset that
recorded its gate and fails under release optimization. The graft is separate
spike evidence, no contender links it, and both transport hard gates run
instrumented builds, so this narrows that graft's deadline claim rather than
the transport selection. It is retained as a structured limitation.

## Contract questions

Ownership stays private in every contender: the public header is byte-identical
across all three, no entity value carries an executor, and no public virtual
base or templated entity policy appears in any candidate. The sanitizer gate
covers exception containment and diagnostic behaviour, and the same concurrency
contract passes under standalone thread sanitizer for all three.
