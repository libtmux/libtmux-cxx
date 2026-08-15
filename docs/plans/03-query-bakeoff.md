# Query bakeoff implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove Python lookup compatibility, select the owning `FilterExpr<T>`
storage design, and freeze relation, range, cardinality, edge-parser, and
versioned serialization contracts before production implementation.

**Architecture:** A Python oracle and three C++ lookup evaluators classify
semantic deltas first. After explicit approval of every delta, three separately
linked AST contenders run one retained compile/runtime/lifetime contract. A
relation-storage comparison runs only if that exercise leaves a material
ownership or graph-layout question unresolved.

**Tech Stack:** Python `ast` and `re`, C++23 ranges and variants, GoogleTest,
compile-contract CMake tests, ASan, UBSan, JSON Schema, and the accepted private
transport exercise.

## Global Constraints

- The lookup study precedes AST implementation. No contender may hide a regex,
  Unicode, missing-value, or membership difference behind its evaluator.
- Every semantic delta enters `cxx/parity/approvals.json` as `pending` and
  stops this plan until the user explicitly approves or rejects it.
- `FilterExpr<T>` is an owning value with owned `std::string` literals and no
  expression-template public type.
- Every node is a closed `std::variant` alternative with owned payloads;
  contenders vary only the private indirection and storage layout.
- Generated field and relation handles are `inline constexpr` objects with
  field-type-specific operation sets.
- Generic range utilities accept standard range and predicate types. Only AST
  visitors, serializers, and future translators require `FilterExpr<T>`.
- `&&`, `||`, and `!` always construct owned nodes; short-circuiting applies
  only when the completed expression evaluates an entity.
- Candidate code lives only under `cxx/spikes/query/`. Contracts, schema,
  golden data, measurements, decisions, and reviews are retained.
- Each contender uses the same field IDs, syntax, evaluator cases, relation
  graph, range tests, compile probes, and serializer event sink.
- A hard-gate failure disqualifies that implementation before measurement and
  blocks phase completion until it is corrected or replaced by another
  distinct working contender.
- Query evaluation, standard views, relation predicates, cardinality, and
  serialization perform no tmux I/O.
- Before every `ctest --preset cxx-sanitize` invocation below, configure and
  build that same preset after the latest source or CMake change. A CTest run
  against an absent or stale sanitizer executable is not evidence.

## Retained Surface Under Test

Every candidate must compile this source unchanged:

```cpp
auto expr =
    libtmux::pane::command.starts_with("nv")
    && libtmux::pane::active;

auto boolean_expr = expr || !libtmux::pane::active;

auto related = libtmux::window::panes.any_of(
    libtmux::pane::command.contains("pytest"));

auto parent = libtmux::window::session.is(
    libtmux::session::name == "work");

auto filtered = panes | std::views::filter(expr);
auto equivalent = panes | libtmux::tmuxq::matching(expr);
auto composed = panes
    | libtmux::tmuxq::matching(boolean_expr)
    | std::views::take(1);
```

### Task 1: Freeze the Python lookup oracle

**Files:**

- Create: `cxx/tests/data/query/lookups-v1.json`
- Create: `cxx/tests/data/query/python-results-v1.json`
- Modify: `cxx/tools/differential/materialize.py`
- Create: `cxx/tests/differential/query_oracle.py`
- Create: `tests/cxx/test_query_oracle.py`
- Modify: `tests/cxx/test_differential.py`
- Modify: `cxx/parity/mapping.json`
- Modify: `cxx/parity/evidence.json`
- Modify: `cxx/parity/manifest.json`

**Interfaces:**

```python
@dataclasses.dataclass(frozen=True, slots=True)
class LookupCase:
    case_id: str
    operator: str
    value: object
    operand: object
    expected_kind: str


def evaluate_case(case: LookupCase) -> dict[str, object]: ...


def main(argv: t.Sequence[str] | None = None) -> int: ...
```

- [ ] **Step 1: Write the failing complete-inventory test**

Require cases for `eq`, `exact`, `iexact`, `contains`, `icontains`,
`startswith`, `istartswith`, `endswith`, `iendswith`, `in`, `nin`, `regex`, and
`iregex`. Each string operator covers empty operands, missing values, ASCII,
non-ASCII UTF-8, and invalid UTF-8 after Python compatibility decoding.
Membership covers string, sequence, mapping, empty, and wrong-shape operands.
Regex covers anchors, groups, character classes, escapes, flags, and invalid
patterns.

- [ ] **Step 2: Run the oracle test before the runner exists**

```console
$ uv run pytest tests/cxx/test_query_oracle.py -v
```

Expected: FAIL importing `query_oracle` or reporting missing operator cases.

- [ ] **Step 3: Implement the source-grounded Python evaluator**

Extend the retained `cxx.tools.differential.materialize` module with a CLI that
accepts `--repository`, `--observation`,
`--input-manifest`, and `--output`. It reads the commit and tree embedded in the
development observation, materializes only the recorded Python input objects
with `git cat-file`, and writes `<output>/source-receipt.json` containing their
repository-relative paths and object digests. It never reads those paths from
the current worktree. Its tests advance repository `HEAD` with an unrelated
commit and require the materialized bytes and receipt to remain those of the
embedded revision. Reuse the same functions already used by
`python_reference.py`; do not create a second source materializer.

The evaluator requires that materialized source root, receipt, parity-input
manifest, and expected development observation. It rejects a resolved import
outside the materialization or any receipt, input-object, commit, or tree
mismatch; it does not compare the surrounding repository's current `HEAD`.
Embed the verified identities in the result. Emit one result per case with
`case_id`, `outcome` (`match`, `no_match`, or `error`), and an error category.
Do not record exception messages containing local paths.

- [ ] **Step 4: Generate and reproduce the golden results**

```console
$ uv run python -m cxx.tools.differential.materialize \
    --repository . \
    --observation cxx/parity/development.json \
    --input-manifest cxx/parity/inputs.json \
    --output cxx/build/python-source/development
```

```console
$ uv run python cxx/tests/differential/query_oracle.py \
    --source-root cxx/build/python-source/development \
    --input-manifest cxx/parity/inputs.json \
    --expected-observation cxx/parity/development.json \
    --cases cxx/tests/data/query/lookups-v1.json \
    --output cxx/tests/data/query/python-results-v1.json
```

```console
$ uv run python cxx/tests/differential/query_oracle.py \
    --source-root cxx/build/python-source/development \
    --input-manifest cxx/parity/inputs.json \
    --expected-observation cxx/parity/development.json \
    --cases cxx/tests/data/query/lookups-v1.json \
    --check cxx/tests/data/query/python-results-v1.json
```

Expected: the second command exits zero and the golden has no timestamps or
machine paths.

Rerun the exact focused test from Step 2:

```console
$ uv run pytest tests/cxx/test_query_oracle.py -v
```

Expected: all focused query-oracle tests pass.

Synchronize the oracle evidence into the reviewed manifest before staging.

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

- [ ] **Step 5: Commit the oracle**

```console
$ git add \
    cxx/tests/data/query \
    cxx/tools/differential/materialize.py \
    cxx/tests/differential/query_oracle.py \
    tests/cxx/test_query_oracle.py \
    tests/cxx/test_differential.py \
    cxx/parity/mapping.json \
    cxx/parity/evidence.json \
    cxx/parity/manifest.json
```

```console
$ git commit -F - <<'EOF'
CXX(test[query]): Freeze lookup oracle

why: Make Python lookup behavior finite before selecting C++ AST
     storage.

what:
- Record every operator, operand shape, missing value, Unicode, and
  regex case
- Generate deterministic outcomes from the pinned Python implementation
EOF
```

### Task 2: Classify C++ lookup compatibility

**Files:**

- Create: `cxx/spikes/query/compat/include/query_compat/evaluator.hpp`
- Create: `cxx/spikes/query/compat/src/ascii_ecmascript.cpp`
- Create: `cxx/spikes/query/compat/src/extended_regex.cpp`
- Create: `cxx/spikes/query/compat/src/python_subset.cpp`
- Create: `cxx/spikes/query/compat/tests/evaluator_test.cpp`
- Create: `cxx/spikes/query/compat/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`
- Create: `cxx/tools/bakeoff/compare_query_oracle.py`
- Create: `tests/cxx/test_compare_query_oracle.py`
- Create: `cxx/docs/bakeoffs/query/compatibility.json`
- Create: `cxx/docs/bakeoffs/query/compatibility.md`

**Interfaces:**

```cpp
namespace libtmux::spike::query_compat {

enum class LookupOp {
  eq,
  exact,
  iexact,
  contains,
  icontains,
  startswith,
  istartswith,
  endswith,
  iendswith,
  in,
  nin,
  regex,
  iregex
};

using LookupScalar =
    std::variant<std::monostate, bool, std::int64_t, std::string>;
using LookupSequence = std::vector<LookupScalar>;
using LookupMapping = std::map<std::string, LookupScalar>;
using LookupValue = std::variant<LookupScalar, LookupSequence, LookupMapping>;
using LookupOperand = LookupValue;

struct LookupError { std::string category; };
struct RegexFlags { bool ignore_case; };
class RegexPattern;

std::expected<bool, LookupError> evaluate_lookup(
    LookupOp op,
    const LookupValue& value,
    const LookupOperand& operand);

std::expected<RegexPattern, LookupError> compile_regex(
    std::string source,
    RegexFlags flags);

}  // namespace libtmux::spike::query_compat
```

- [ ] **Step 1: Add a C++ golden-runner test**

The Python harness validates `lookups-v1.json`, lowers each case through the
retained version-one differential wire protocol, and reconstructs result JSON
from the runner's typed frames. C++ never parses JSON. Run each evaluator over
the wire cases and emit the same result fields as the Python oracle. Invalid
regex must fail at `compile_regex`; evaluation does not throw.

Run the comparator test before its implementation exists:

```console
$ uv run pytest tests/cxx/test_compare_query_oracle.py -v
```

Expected: FAIL importing `compare_query_oracle`. An unrelated fixture or
collection failure is not the intended red result.

- [ ] **Step 2: Run the evaluator target compile/link red**

Register `query_compat_contracts` and all three evaluator bindings before
implementation, then use the compile/link-red sequence:

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target query_compat_contracts
```

Expected: FAIL for missing evaluator definitions, not for an unknown target.

- [ ] **Step 3: Compare three concrete evaluator strategies**

Implement and test:

- ECMAScript `std::regex` with deterministic ASCII-only lowering.
- Extended `std::regex` with deterministic ASCII-only lowering.
- A rejecting Python-subset translator that lowers only proven constructs to
  ECMAScript and returns `unsupported_pattern` otherwise.

Do not use the process locale. Do not silently accept a C++ regex meaning that
differs from Python.

- [ ] **Step 4: Build and run every evaluator case**

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target query_compat_contracts
```

```console
$ ctest --preset cxx-dev -R '^query\.compat\.' --no-tests=error --output-on-failure
```

Expected: every case executes and every mismatch is emitted as a typed delta;
the runtime harness itself has no missing, crashed, or silently skipped case.

- [ ] **Step 5: Generate the compatibility classification**

Implement the deterministic comparator and rerun the identical focused test:

```console
$ uv run pytest tests/cxx/test_compare_query_oracle.py -v
```

Expected: all comparator tests pass.

```console
$ uv run python cxx/tools/bakeoff/compare_query_oracle.py \
    --python cxx/tests/data/query/python-results-v1.json \
    --cxx-dir cxx/build/cxx-dev/query-compat \
    --output cxx/docs/bakeoffs/query/compatibility.json
```

Expected: every case is `matching`, `adaptation_required`, or
`candidate_rejected`; no case is `unclassified`.

- [ ] **Step 6: Stop for explicit semantic approval**

Present a finite review list generated from every `adaptation_required` item in
`compatibility.json`. Each item names its mapping `entry_id`, exact semantic
delta, `oracle_id`, proposed C++ behavior, and evidence digest. Keep its mapping
row at `status: pending` with `approval_id: null` until the user decides. Do not
start Task 3 until every item is explicitly approved or the implementation is
changed to match Python. After any rejected adaptation or evaluator change,
repeat Steps 4 and 5 in full, including configure, the exact target rebuild,
strict CTest, comparator tests, and regenerated `compatibility.json`, before
presenting the remaining list again.

- [ ] **Step 7: Record approval evidence and rerun the gate**

For each approved item, add the decision record to `approvals.json`, set the
still-pending mapping row's `semantic_delta`, `oracle_id`, and `approval_id`,
and leave every C++ symbol and implementation-evidence field null. A
now-matching item remains pending without adaptation fields. Its Plan 05
production shard alone promotes the row to `adapted` or `implemented` after
the required C++ API and behavior evidence exist. Then synchronize the reviewed
records:

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
    verify \
    --manifest cxx/parity/manifest.json \
    --mode structural \
    --allow-pending \
    --require-query-approvals
```

Expected: exit zero with no pending query-semantic approval.

- [ ] **Step 8: Commit compatibility evidence**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/spikes/query/compat \
    cxx/tools/bakeoff/compare_query_oracle.py \
    tests/cxx/test_compare_query_oracle.py \
    cxx/docs/bakeoffs/query \
    cxx/parity
```

```console
$ git commit -F - <<'EOF'
CXX(spike[query]): Classify lookup semantics

why: Prevent C++ regex and case rules from silently changing Python
     behavior.

what:
- Compare three evaluators against the complete Python lookup oracle
- Record every approved adaptation with differential evidence
EOF
```

### Task 3: Freeze the shared AST, range, and schema contracts

**Files:**

- Create: `cxx/tests/contracts/query/exercise.hpp`
- Create: `cxx/tests/contracts/query/exercise.cpp`
- Create: `cxx/tests/contracts/query/lifetime_test.cpp`
- Create: `cxx/tests/contracts/query/ranges_test.cpp`
- Create: `cxx/tests/contracts/query/relations_test.cpp`
- Create: `cxx/tests/contracts/query/serialization_test.cpp`
- Create: `cxx/tests/contracts/query/relation_probe.cpp`
- Create: `cxx/tests/contracts/compile/query/valid/`
- Create: `cxx/tests/contracts/compile/query/invalid/`
- Create: `cxx/tests/contracts/compile/query/manifest.json`
- Create: `cxx/tests/data/query/filter-expression-events-v1.json`
- Create: `cxx/schema/filter-expression-v1.schema.json`
- Create: `cxx/cmake/CompileContract.cmake`
- Create: `cxx/tests/contracts/compile/query/run_compile_contract.cmake`
- Create: `cxx/tools/bakeoff/generate_query_contract.py`
- Create: `tests/cxx/test_generate_query_contract.py`
- Modify: `cxx/parity/mapping.json`
- Modify: `cxx/parity/approvals.json`
- Modify: `cxx/parity/manifest.json`
- Modify: `cxx/CMakeLists.txt`
- Modify: `cxx/tests/CMakeLists.txt`

**Interfaces:**

```cpp
namespace libtmux {

template<class Entity>
class FilterExpr;

enum class CardinalityError { no_match, multiple };

template<class T>
using cardinality_result = std::expected<T, CardinalityError>;

template<class Range>
concept StableConstReferenceRange =
    std::ranges::forward_range<Range>
    && std::same_as<
        std::ranges::range_reference_t<Range>,
        const std::ranges::range_value_t<Range>&>
    && (std::is_lvalue_reference_v<Range>
        || std::ranges::borrowed_range<std::remove_cvref_t<Range>>);

template<class Range>
concept CopyMaterializableInputRange =
    std::ranges::input_range<Range>
    && std::constructible_from<
        std::ranges::range_value_t<Range>,
        std::ranges::range_reference_t<Range>>;

template<class Range>
concept MoveMaterializableInputRange =
    std::ranges::input_range<Range>
    && std::constructible_from<
        std::ranges::range_value_t<Range>,
        std::ranges::range_rvalue_reference_t<Range>>;

namespace tmuxq {

template<class Entity>
auto matching(FilterExpr<Entity> expression);

template<std::ranges::forward_range Range>
  requires StableConstReferenceRange<Range>
auto first(Range&& range)
    -> std::optional<std::reference_wrapper<
        const std::ranges::range_value_t<Range>>>;

template<std::ranges::forward_range Range>
  requires StableConstReferenceRange<Range>
auto exactly_one(Range&& range)
    -> cardinality_result<std::reference_wrapper<
        const std::ranges::range_value_t<Range>>>;

template<CopyMaterializableInputRange Range>
auto first_value(Range& range)
    -> std::optional<std::ranges::range_value_t<Range>>;

template<CopyMaterializableInputRange Range>
auto exactly_one_value(Range& range)
    -> cardinality_result<std::ranges::range_value_t<Range>>;

template<class Range>
  requires (!std::is_lvalue_reference_v<Range>)
        && MoveMaterializableInputRange<Range>
auto first_value(Range&& range)
    -> std::optional<std::ranges::range_value_t<Range>>;

template<class Range>
  requires (!std::is_lvalue_reference_v<Range>)
        && MoveMaterializableInputRange<Range>
auto exactly_one_value(Range&& range)
    -> cardinality_result<std::ranges::range_value_t<Range>>;

}  // namespace tmuxq
}  // namespace libtmux
```

- [ ] **Step 1: Write positive and negative compile contracts**

Positive probes cover `&&`, `||`, and `!` construction, direct
`std::predicate`, standard `views::filter`, `tmuxq::matching` composed before
and after standard views, mixed ordinary predicates, relation predicates using
`any_of`, `all_of`, `none_of`, and to-one `is`, lvalue forward-range references,
borrowed rvalues, a copyable single-pass lvalue range, a proxy-reference input
range whose value is constructible from the proxy, and an rvalue single-pass
range of move-only values whose `iter_move` result constructs the value.
Negative probes cover invalid field/operator pairs, mixed entities, wrong row
types, invalid relation kinds, temporary snapshots, temporary containers,
prvalue-producing reference ranges, non-borrowed rvalue reference views, a
move-only lvalue input range, and a proxy range whose value is constructible
from neither its reference nor its rvalue reference.

The compile contract also requires constant initialization of every generated
field and relation handle; a runtime-initialized registry is not compatible.

Register `query_contract_harness_test` with the positive and intentional
negative control sources before implementing `CompileContract.cmake` or the
golden-case generator. Run the compile/link red against that named target:

```console
$ uv run pytest tests/cxx/test_generate_query_contract.py -v
```

Expected: FAIL importing `generate_query_contract`. An unrelated fixture or
collection failure is not the intended Python red.

```console
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target query_contract_harness_test
```

Expected: FAIL for the missing compile-contract helper or generated case
definitions, not for an unknown target or unrelated configure failure.

Runtime counters prove that lvalue owning overloads construct from `*iterator`
without moving from the source, while rvalue overloads construct through
`std::ranges::iter_move`. Because a single-pass range cannot retain its first
reference while probing a second element, `exactly_one_value` materializes the
first value before advancing. On `multiple`, an lvalue source has only been
copied; an rvalue source may have had its first element moved from. Tests pin
that consumption contract.

Each candidate must satisfy this exact public predicate check:

```cpp
template<class Expression, class Entity>
concept ExactFilterPredicate = requires(
    const Expression& expression,
    const Entity& entity) {
  { expression(entity) } -> std::same_as<bool>;
};

static_assert(ExactFilterPredicate<
              libtmux::FilterExpr<libtmux::Pane>,
              libtmux::Pane>);
static_assert(std::predicate<
              libtmux::FilterExpr<libtmux::Pane>,
              const libtmux::Pane&>);
```

- [ ] **Step 2: Add owned-expression lifetime tests**

Construct expressions from temporary strings, copy and move them, store them in
containers, return them from functions, destroy their source builders, and
evaluate under ASan/UBSan. Boolean evaluation must short-circuit.

- [ ] **Step 3: Add snapshot and relation laws**

The exercise uses immutable heap-stable records and a normalized graph. Test
linked windows, a loaded empty relation, a missing to-one edge,
`any_of(empty) == false`, `all_of(empty) == true`,
`none_of(empty) == true`, present and missing to-one `is` evaluation, and zero
transport calls.
Test snapshot copy/move construction, source-bound views, destination
assignment invalidation, and source stability exactly as the approved design
specifies.

- [ ] **Step 4: Commit schema and serializer event goldens**

Schema v1 uses `$id`
`https://libtmux.git-pull.com/schema/filter-expression/v1`, integer major
version `1`, closed node tags, entity kinds, field IDs, relation IDs, scalar
encodings, and regex flags. The C++
exercise emits serializer events only; the event golden covers every node kind,
unknown schema version in the cross-language oracle, and serializer failure
propagation. No JSON library is linked.

Python validates the JSON schema and goldens and generates a build-directory
C++ case header. The C++ golden runner consumes that typed header and emits the
retained version-one differential wire frames; Python converts the frames back
to JSON for schema comparison. Neither contender nor production code parses
JSON, and generated build files are not committed.

`CompileContract.cmake` exposes
`libtmux_add_compile_contract(ID, VALID_SOURCE, INVALID_SOURCE,
EXPECT_DIAGNOSTIC)` as required single-value keyword arguments. The helper
compiles the positive source first. The negative source succeeds only when
compilation fails, names that source, and matches the expected diagnostic; an
infrastructure or positive-control failure always fails CTest. Complete the
already registered `query_contract_harness_test` target and register its CTest
name as `contract.query.harness`.

Implement the generator and rerun the identical focused test:

```console
$ uv run pytest tests/cxx/test_generate_query_contract.py -v
```

Expected: all generator tests pass and the typed case header reproduces
byte-for-byte.

- [ ] **Step 5: Run the harness self-test**

Record the approved design hash as the approval evidence for stable-forward
reference helpers and owning input-range overloads. Synchronize those mapping
rows without changing any lookup approval from Task 2.

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
$ cmake --preset cxx-dev
```

```console
$ cmake --build --preset cxx-dev --target query_contract_harness_test
```

```console
$ ctest \
    --preset cxx-dev \
    -R '^contract\.query\.harness$' \
    --no-tests=error \
    --output-on-failure
```

Expected: the positive control compiles, the intentional invalid control fails
with its source named, and infrastructure failure cannot count as success.

- [ ] **Step 6: Commit retained query contracts**

```console
$ git add \
    cxx/CMakeLists.txt \
    cxx/cmake/CompileContract.cmake \
    cxx/tests/CMakeLists.txt \
    cxx/tests/contracts/query \
    cxx/tests/contracts/compile/query \
    cxx/tests/data/query \
    cxx/schema/filter-expression-v1.schema.json \
    cxx/tools/bakeoff/generate_query_contract.py \
    tests/cxx/test_generate_query_contract.py \
    cxx/parity/mapping.json \
    cxx/parity/approvals.json \
    cxx/parity/manifest.json
```

```console
$ git commit -F - <<'EOF'
CXX(test[query]): Freeze AST contracts

why: Compare storage designs through identical syntax, lifetime, and
     schema.

what:
- Add runtime, compile-fail, relation, range, and cardinality contracts
- Commit the version-one schema and serializer event goldens
EOF
```

### Task 4: Implement the flat postorder arena

**Files:**

- Create: `cxx/spikes/query/flat_arena/include/libtmux/query/expr.hpp`
- Create: `cxx/spikes/query/flat_arena/include/libtmux/query/fields.hpp`
- Create: `cxx/spikes/query/flat_arena/include/libtmux/query/algorithms.hpp`
- Create: `cxx/spikes/query/flat_arena/include/libtmux/query/parser.hpp`
- Create: `cxx/spikes/query/flat_arena/include/libtmux/query/visitor.hpp`
- Create: `cxx/spikes/query/flat_arena/include/libtmux/serialization/query_json.hpp`
- Create: `cxx/spikes/query/flat_arena/src/evaluate.cpp`
- Create: `cxx/spikes/query/flat_arena/tests/binding.cpp`
- Create: `cxx/spikes/query/flat_arena/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`

**Representation:** A postorder `std::vector<std::variant<...>>`, child
indices, and one root index. Combining expressions copies nodes and rebases all
child indices. Literals are owned values.

- [ ] **Step 1: Bind the common contract and run it red**

Register `query_flat_arena` and its CTest cases before the red build, with the
binding referring to the required candidate declarations. Use the
compile/link-red sequence:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_flat_arena
```

Expected: FAIL for missing candidate declarations or definitions, not for an
unknown target.

- [ ] **Step 2: Implement nodes, generated handles, evaluation, and visitor**

Implement scalar operations, approved lookup semantics, Boolean nodes,
relation nodes, range helpers, edge parsing, and serializer events. Add a
focused test that combines two multi-node expressions and verifies every
rebased index.

- [ ] **Step 3: Pass runtime, compile, lifetime, and schema gates**

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_flat_arena
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^query\.flat_arena\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: all flat-arena gates pass.

- [ ] **Step 4: Commit the contender**

```console
$ git add cxx/spikes/query/flat_arena cxx/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(spike[query]): Try flat AST arena

why: Test compact value storage with stable node IDs and direct
     traversal.

what:
- Add postorder variant nodes with index rebasing and owned literals
- Pass shared query, relation, range, lifetime, and schema contracts
EOF
```

### Task 5: Implement the recursive owning tree

**Files:**

- Create: `cxx/spikes/query/recursive_tree/include/libtmux/query/expr.hpp`
- Create: `cxx/spikes/query/recursive_tree/include/libtmux/query/fields.hpp`
- Create: `cxx/spikes/query/recursive_tree/include/libtmux/query/algorithms.hpp`
- Create: `cxx/spikes/query/recursive_tree/include/libtmux/query/parser.hpp`
- Create: `cxx/spikes/query/recursive_tree/include/libtmux/query/visitor.hpp`
- Create: `cxx/spikes/query/recursive_tree/include/libtmux/serialization/query_json.hpp`
- Create: `cxx/spikes/query/recursive_tree/src/evaluate.cpp`
- Create: `cxx/spikes/query/recursive_tree/tests/binding.cpp`
- Create: `cxx/spikes/query/recursive_tree/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`

**Representation:** Recursively boxed variant nodes with explicit deep-copy
clone and owned literals.

- [ ] **Step 1: Add clone and deep-destruction failures first**

Test copy independence, move validity, 10,000 nested negations, destruction,
and serializer order under sanitizers.

- [ ] **Step 2: Run the registered target compile/link red**

Register `query_recursive_tree` and its CTest cases before implementation:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_recursive_tree
```

Expected: FAIL for missing recursive-tree definitions, not for an unknown
target.

- [ ] **Step 3: Implement and pass the common contract**

Implement the recursively boxed nodes, deep copy, evaluation, generated
handles, range/parser surface, visitor, and serializer events.

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_recursive_tree
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^query\.recursive_tree\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: the same contract passes. Retain failing evidence while correcting
the contender; do not advance with two candidates.

- [ ] **Step 4: Commit the contender**

```console
$ git add cxx/spikes/query/recursive_tree cxx/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(spike[query]): Try recursive AST

why: Establish the readability and ownership baseline for AST storage.

what:
- Add recursively boxed variant nodes with explicit deep copy
- Exercise depth, destruction, visitor, and common query contracts
EOF
```

### Task 6: Implement the immutable shared DAG

**Files:**

- Create: `cxx/spikes/query/shared_dag/include/libtmux/query/expr.hpp`
- Create: `cxx/spikes/query/shared_dag/include/libtmux/query/fields.hpp`
- Create: `cxx/spikes/query/shared_dag/include/libtmux/query/algorithms.hpp`
- Create: `cxx/spikes/query/shared_dag/include/libtmux/query/parser.hpp`
- Create: `cxx/spikes/query/shared_dag/include/libtmux/query/visitor.hpp`
- Create: `cxx/spikes/query/shared_dag/include/libtmux/serialization/query_json.hpp`
- Create: `cxx/spikes/query/shared_dag/src/evaluate.cpp`
- Create: `cxx/spikes/query/shared_dag/tests/binding.cpp`
- Create: `cxx/spikes/query/shared_dag/CMakeLists.txt`
- Modify: `cxx/CMakeLists.txt`

**Representation:** A closed `std::variant` `Node` with
`std::shared_ptr<const Node>` children. Composition shares immutable subtrees
without global interning; no mutable node handle exists.

- [ ] **Step 1: Add immutability and cycle controls**

Compile probes reject a mutable node visitor. Runtime tests verify repeated
subexpression sharing, absence of reference cycles, stable serializer order,
and deterministic destruction.

- [ ] **Step 2: Run the registered target compile/link red**

Register `query_shared_dag` and its CTest cases before implementation:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_shared_dag
```

Expected: FAIL for missing shared-DAG definitions, not for an unknown target.

- [ ] **Step 3: Implement and pass the common contract**

Implement the immutable shared nodes, evaluation, generated handles,
range/parser surface, visitor, and serializer events.

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_shared_dag
```

```console
$ ctest \
    --preset cxx-sanitize \
    -R '^query\.shared_dag\.' \
    --no-tests=error \
    --output-on-failure
```

Expected: all common and focused tests pass. Retain failing evidence while
correcting the contender; do not advance with two.

- [ ] **Step 4: Commit the contender**

```console
$ git add cxx/spikes/query/shared_dag cxx/CMakeLists.txt
```

```console
$ git commit -F - <<'EOF'
CXX(spike[query]): Try shared AST DAG

why: Measure immutable subtree sharing against owning value
     alternatives.

what:
- Add const shared nodes without mutable aliases or global interning
- Test sharing, cycles, serialization, and common query behavior
EOF
```

### Task 7: Resolve relation storage only when required

**Files:**

- Create: `cxx/tools/bakeoff/relation_trigger.py`
- Create: `cxx/tools/bakeoff/summarize_query_gates.py`
- Create: `cxx/tools/bakeoff/measure_relation.py`
- Bind: `cxx/tools/evidence/ctest_gate.py`
- Create: `tests/cxx/test_relation_trigger.py`
- Create: `tests/cxx/test_measure_relation.py`
- Create conditionally: `cxx/spikes/query/relations/hash_graph/`
- Create conditionally: `cxx/spikes/query/relations/sorted_tables/`
- Create conditionally: `cxx/spikes/query/relations/row_bundles/`
- Create conditionally: `cxx/docs/bakeoffs/query/relations/hash-graph.json`
- Create conditionally: `cxx/docs/bakeoffs/query/relations/sorted-tables.json`
- Create conditionally: `cxx/docs/bakeoffs/query/relations/row-bundles.json`
- Modify conditionally: `cxx/CMakeLists.txt`
- Create: `cxx/docs/bakeoffs/query/candidate-results.json`
- Create: `cxx/docs/bakeoffs/query/relation-decision.json`

- [ ] **Step 1: Test the material-uncertainty trigger**

The trigger requires a relation bakeoff only if no passing AST contender proves
all of: exact linked-window adjacency, loaded-empty semantics, missing to-one
behavior, copy/move lifetime, zero-I/O evaluation, and a complete baseline
graph-overhead measurement.
The retained `relation_probe.cpp` emits capture time, lookup time, allocation,
and duplication records for the shared baseline graph. The trigger emits
explicit evidence IDs for each unresolved item and does not use an undefined
acceptability threshold.

`summarize_query_gates.py` accepts `--gate-record` and `--output`. It validates
the content-addressed inventory and JUnit leaf written by the retained
`ctest_gate.py`; it does not implement another CTest runner or read CTest's
mutable `Testing/` directory. It refuses a stale source, configure, executable,
selection, or tool identity. The normalized public output records the inventory
and JUnit digests without durations, host paths, or environment data.
`test_relation_trigger.py` covers both this summarizer and the trigger that
consumes its output.

`measure_relation.py` accepts one exact contender gate record, runs that
contender's probe seven times, and binds gate, source, compiler, configuration,
and raw-sample digests into its normalized output. Unit tests reject a missing
test, a reused record for another contender, or measurements captured before
the current gate passed.

- [ ] **Step 2: Run the relation-tool tests red**

```console
$ uv run pytest \
    tests/cxx/test_relation_trigger.py \
    tests/cxx/test_measure_relation.py \
    -v
```

Expected: FAIL importing the relation trigger, gate summarizer, or measurement
tool. An unrelated fixture or collection error is not the intended red result.

- [ ] **Step 3: Implement and verify the relation tools**

Implement the three closed interfaces from Step 1, then rerun the identical
focused command:

```console
$ uv run pytest \
    tests/cxx/test_relation_trigger.py \
    tests/cxx/test_measure_relation.py \
    -v
```

Expected: all focused relation-tool tests pass.

- [ ] **Step 4: Run the trigger**

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
    --label query \
    --gate-id query-relation-trigger \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/query-relation-trigger.json
```

```console
$ uv run python cxx/tools/bakeoff/summarize_query_gates.py \
    --gate-record cxx/build/evidence/query-relation-trigger.json \
    --output cxx/docs/bakeoffs/query/candidate-results.json
```

```console
$ uv run python cxx/tools/bakeoff/relation_trigger.py \
    --query-results cxx/docs/bakeoffs/query/candidate-results.json \
    --output cxx/docs/bakeoffs/query/relation-decision.json
```

Expected: either `not_run` with evidence that every trigger is settled or
`required` with at least one named unresolved material question.

- [ ] **Step 5: Commit the trigger and measurement tooling**

```console
$ git add \
    cxx/tools/bakeoff/relation_trigger.py \
    cxx/tools/bakeoff/summarize_query_gates.py \
    cxx/tools/bakeoff/measure_relation.py \
    tests/cxx/test_relation_trigger.py \
    tests/cxx/test_measure_relation.py \
    cxx/docs/bakeoffs/query/candidate-results.json \
    cxx/docs/bakeoffs/query/relation-decision.json
```

```console
$ git commit -F - <<'EOF'
CXX(test[relations]): Classify graph trigger

why: Preserve current query gates before deciding whether graph storage
     needs a separate comparison.

what:
- Capture immutable CTest inventories and JUnit results
- Record the relation trigger and reproducible measurement harness
EOF
```

If the decision is `not_run`, this task ends here.

- [ ] **Step 6: If required, freeze the contender binding**

Each conditional directory contains `include/relation_store.hpp`,
`src/relation_store.cpp`, `tests/relation_store_test.cpp`, and
`CMakeLists.txt`. All three bind the retained exercise through the same private
interface:

```cpp
struct RelationContext {
  virtual ~RelationContext() = default;
  virtual std::span<const EntityId> related_ids(
      EntityId source, RelationId relation) const = 0;
};
```

The public `expr(entity)` surface and capture input stay fixed. Immediately
before each contender's red, create and register only that contender's target.
Do not add either sibling directory or registration until its own step; every
contender commit must configure, build, test, and measure independently.

- [ ] **Step 7: If required, implement and measure the hash graph**

Create and register `query_relation_hash_graph` with the declaration-only
binding and retained contract before implementing the store:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_relation_hash_graph
```

Expected: FAIL at compile or link time for missing hash-graph store definitions,
not for an unknown target.

Implement ID-keyed scalar records and adjacency hash maps. Reconfigure and
rebuild the exact target before running its strict gate:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_relation_hash_graph
```

```console
$ uv run python -m cxx.tools.evidence.ctest_gate \
    --source-dir cxx \
    --preset cxx-sanitize \
    --match '^query\.relation\.hash_graph$' \
    --gate-id query-relation-hash-graph \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/query-relation-hash-graph.json
```

```console
$ uv run python cxx/tools/bakeoff/measure_relation.py \
    --candidate hash_graph \
    --build-dir cxx/build/cxx-sanitize \
    --gate-record cxx/build/evidence/query-relation-hash-graph.json \
    --repetitions 7 \
    --output cxx/docs/bakeoffs/query/relations/hash-graph.json
```

```console
$ git add \
    cxx/spikes/query/relations/hash_graph \
    cxx/CMakeLists.txt \
    cxx/docs/bakeoffs/query/relations/hash-graph.json
```

Commit these exact paths as `CXX(spike[relations]): Try hash graph`.

- [ ] **Step 8: If required, implement and measure sorted tables**

Create and register `query_relation_sorted_tables` with the declaration-only
binding and retained contract before implementing the store:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_relation_sorted_tables
```

Expected: FAIL at compile or link time for missing sorted-table definitions,
not for an unknown target.

Implement sorted typed record vectors with compact edge ranges. Reconfigure and
rebuild the exact target before running its strict gate:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_relation_sorted_tables
```

```console
$ uv run python -m cxx.tools.evidence.ctest_gate \
    --source-dir cxx \
    --preset cxx-sanitize \
    --match '^query\.relation\.sorted_tables$' \
    --gate-id query-relation-sorted-tables \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/query-relation-sorted-tables.json
```

```console
$ uv run python cxx/tools/bakeoff/measure_relation.py \
    --candidate sorted_tables \
    --build-dir cxx/build/cxx-sanitize \
    --gate-record cxx/build/evidence/query-relation-sorted-tables.json \
    --repetitions 7 \
    --output cxx/docs/bakeoffs/query/relations/sorted-tables.json
```

```console
$ git add \
    cxx/spikes/query/relations/sorted_tables \
    cxx/CMakeLists.txt \
    cxx/docs/bakeoffs/query/relations/sorted-tables.json
```

Commit these exact paths as `CXX(spike[relations]): Try sorted tables`.

- [ ] **Step 9: If required, implement and measure row bundles**

Create and register `query_relation_row_bundles` with the declaration-only
binding and retained contract before implementing the store:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_relation_row_bundles
```

Expected: FAIL at compile or link time for missing row-bundle definitions, not
for an unknown target.

Implement denormalized per-row related scalar bundles. Reconfigure and rebuild
the exact target before running its strict gate:

```console
$ cmake --preset cxx-sanitize
```

```console
$ cmake --build --preset cxx-sanitize --target query_relation_row_bundles
```

```console
$ uv run python -m cxx.tools.evidence.ctest_gate \
    --source-dir cxx \
    --preset cxx-sanitize \
    --match '^query\.relation\.row_bundles$' \
    --gate-id query-relation-row-bundles \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/query-relation-row-bundles.json
```

```console
$ uv run python cxx/tools/bakeoff/measure_relation.py \
    --candidate row_bundles \
    --build-dir cxx/build/cxx-sanitize \
    --gate-record cxx/build/evidence/query-relation-row-bundles.json \
    --repetitions 7 \
    --output cxx/docs/bakeoffs/query/relations/row-bundles.json
```

```console
$ git add \
    cxx/spikes/query/relations/row_bundles \
    cxx/CMakeLists.txt \
    cxx/docs/bakeoffs/query/relations/row-bundles.json
```

Commit these exact paths as `CXX(spike[relations]): Try row bundles`.

- [ ] **Step 10: If required, compare all three contenders**

Each contender's atomic commit now contains its implementation and normalized
gate and measurement record. Compare all three only from those committed
records:

```console
$ uv run python cxx/tools/bakeoff/relation_trigger.py \
    --query-results cxx/docs/bakeoffs/query/candidate-results.json \
    --relation-results cxx/docs/bakeoffs/query/relations \
    --output cxx/docs/bakeoffs/query/relation-decision.json
```

Update `relation-decision.json` only after all three atomic contender commits.
If any concrete path, target, or measure above proves insufficient, stop and
write a tracked subordinate plan before editing a contender.

- [ ] **Step 11: Commit the selected relation result**

```console
$ git add cxx/docs/bakeoffs/query/relation-decision.json
```

```console
$ git commit -F - <<'EOF'
CXX(docs[relations]): Select graph storage

why: Resolve the material graph-layout question from complete comparable
     evidence.

what:
- Compare three atomically measured relation contenders
- Select the layout that satisfies the retained graph contract
EOF
```

### Task 8: Measure, select, and review the query design

**Files:**

- Create: `cxx/tools/bakeoff/measure_query.py`
- Create: `tests/cxx/test_measure_query.py`
- Modify: `cxx/CMakePresets.json`
- Modify: `cxx/docs/bakeoffs/query/candidate-results.json`
- Create: `cxx/docs/bakeoffs/query/measurements.json`
- Create: `cxx/docs/bakeoffs/query/diagnostics/`
- Create: `cxx/docs/bakeoffs/query/decision.json`
- Create: `cxx/docs/bakeoffs/query/scorecard.md`
- Create: `cxx/docs/bakeoffs/query/review.md`
- Create or extend conditionally: `cxx/docs/plans/followups/`

`measure_query.py` records hard gates, clean and incremental compile time,
public-header parse time, binary size, construction/copy/evaluation
allocations, composition/evaluation time, maximum passing depth, diagnostics,
source/template footprint, and serializer behavior across seven runs. Its
record binds the configure-preset cache digest, compiler executable and
version, normalized compile-command digest, source identity, and gate summary.

- [ ] **Step 1: Verify measurement normalization**

Tests reject absolute paths, host data, missing raw sample counts, differing
compiler flags, a mutable CTest `Testing/` directory as evidence, and any
measurement attempted before all three current hard gates pass.

- [ ] **Step 2: Run the measurement test before implementation**

```console
$ uv run pytest tests/cxx/test_measure_query.py -v
```

Expected: FAIL importing `measure_query`.

- [ ] **Step 3: Implement and verify normalized measurement**

Implement only the identity checks, deterministic normalization, and metric
collection contract above, then rerun the focused test:

```console
$ uv run pytest tests/cxx/test_measure_query.py -v
```

Expected: all query-measurement unit tests pass.

- [ ] **Step 4: Run the complete hard gate**

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
    --label query \
    --gate-id query-final \
    --output-root cxx/build/evidence/ctest \
    --record cxx/build/evidence/query-final.json
```

```console
$ uv run python cxx/tools/bakeoff/summarize_query_gates.py \
    --gate-record cxx/build/evidence/query-final.json \
    --output cxx/docs/bakeoffs/query/candidate-results.json
```

Expected: every selected candidate result has runtime, compile-fail, lifetime,
relation, range, parser, schema, and zero-I/O evidence bound to an immutable
registered-test inventory and JUnit result digest.

- [ ] **Step 5: Capture controlled measurements**

Add configure and build presets named `cxx-query-measure`. The configure preset
inherits the non-sanitizer compiler and warning configuration from `cxx-dev`,
uses `cxx/build/cxx-query-measure`, sets `CMAKE_BUILD_TYPE=Release`, enables
tests and warnings-as-errors, and disables ASan, UBSan, TSan, and clang-tidy.
It must resolve to the same compiler executable and version as the query hard
gate. The build preset binds only that configure preset. The measurement tool
rejects a different build directory, cache values, compiler identity, source
identity, or normalized non-instrumentation flags.

```console
$ cmake --preset cxx-query-measure
```

```console
$ cmake --build --preset cxx-query-measure
```

```console
$ uv run python cxx/tools/bakeoff/measure_query.py \
    --candidate all \
    --build-dir cxx/build/cxx-query-measure \
    --gate-summary cxx/docs/bakeoffs/query/candidate-results.json \
    --repetitions 7 \
    --output cxx/docs/bakeoffs/query/measurements.json
```

Expected: deterministic normalized JSON with one record per contender.

- [ ] **Step 6: Name the selected AST and graph contract**

`decision.json` records the AST winner, approved semantic adaptations, relation
trigger result, any selected relation layout, accepted grafts, rejected
trade-offs, schema identity, and classified unknowns. Each unknown has
`materiality`, evidence, and a follow-up disposition. A material unknown blocks
selection until a focused follow-up spike, measurement, and review closes it;
before that work, stop and add a tracked subordinate plan under
`cxx/docs/plans/followups/`, named by the unknown's stable ID and listing exact
paths, gates, and commits. A non-material unknown explains why it cannot change
the public contract or winner. No fixed performance weight determines the
result.

- [ ] **Step 7: Obtain an independent adversarial review**

The reviewer challenges dangling references, snapshot assignment, recursion,
shared ownership, invalid operation diagnostics, hidden I/O, empty-relation
laws, cardinality categories, parser ambiguity, regex deltas, schema stability,
and measurement fairness. Fix or evidence every finding.

Commit every review-driven contender, retained-contract, test, schema, or
CMake-registration fix as an atomic issue-family change in its owning task
before closing the report. After the last fix, repeat Steps 4 through 6 in full:
replace the immutable hard-gate summary, controlled measurements, decision, and
scorecard from the reviewed current source. Before Step 8, status may contain
only the Task 8 evidence, review, and decision paths listed below; an unstaged
source or registration fix blocks closeout.

- [ ] **Step 8: Verify and commit the decision**

```console
$ uv run python cxx/tools/bakeoff/verify_decision.py \
    --axis query \
    --require-review-closed
```

Expected: exit zero only when query approvals, every contender, the relation
trigger, measurements, schema, and review dispositions are complete, with no
material unknown remaining.

```console
$ git add \
    cxx/CMakePresets.json \
    cxx/tools/bakeoff/measure_query.py \
    tests/cxx/test_measure_query.py \
    cxx/docs/bakeoffs/query \
    cxx/schema \
    cxx/parity
```

```console
$ git commit -F - <<'EOF'
CXX(docs[query]): Select query design

why: Freeze query ownership only after semantic and lifetime proof.

what:
- Record lookup, AST, relation, range, parser, and schema evidence
- Close an independent review and name the selected query graft
EOF
```
