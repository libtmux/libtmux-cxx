# Transport scorecard

Winner: abstract
Method: evidence-led selection without numeric weights
Decision core: sha256:b54a7478bb94014a0a66b139d985f9bd74749fd775cb39b84e0eb0f42c6fa181
Decisive axes: server_create_allocations, wrapper_minus_common_allocations, wrapper_minus_common_runtime
Non-decisive axes: clean_compile_time, controlled_incremental_time, private_diagnostic_shape, production_binary_sections, production_source_footprint, public_header_parse_time
Fairness caveat: closed-variant production inventory is larger and the public headers are byte-identical, so compile, sections, footprint, diagnostics, and header-parse results cannot select the winner.

## What the measurements show

The three contenders differ only in how a `Server` privately owns its backend.
Their public headers are byte-identical, so nothing here changes what a caller
writes.

Server construction separates them once. The closed variant stores its backend
inline in the shared state and allocates once per `Server`; the abstract and
function-table designs allocate twice. Across seven controlled samples the
counts never varied, and the difference in requested bytes is eight per server.

Wrapper dispatch separates them consistently but not usefully. Over seven
samples of 100,000 dispatches each the ordering never changed and the ranges
never overlapped: the function table costs about 12.8 ns per call, the abstract
backend about 18.8 ns, and the closed variant about 26.6 ns. No contender
allocates in dispatch beyond the shared validation baseline.

## Why those differences do not select a winner

Every dispatched command launches a process through the shared kernel. The
`dispatch_overhead_scale` follow-up measures the cheapest launch that kernel can
perform, which lower-bounds every real command, at a median near 790
microseconds. The widest wrapper gap between contenders is under 14 nanoseconds
per call, roughly one part in fifty thousand of a single launch, and one extra
`malloc` is paid once per `Server` value rather than once per command.

So the decisive axes are real, reproducible, and too small to observe through
the transport they implement. An unweighted, evidence-led selection therefore
cannot promote them, and the choice falls to the properties the design froze
before measuring.

## Why the abstract backend wins

It is the only contender with no template declarations in the private ownership
machinery, it has the smallest production source, and its production backend
inventory contains only the subprocess backend. The closed variant ships a
recording backend that only tests need and turns every future backend into a
rewrite of its closed alternative set; the function table reintroduces the
manual erasure machinery that a virtual call already provides, for a saving
that no caller can observe.

## Where the reasoning for each limitation lives

The structured limitations below are identifiers and dispositions. Each one
states its contract-impact rationale and the document it was read from in
`decision.json`, which is the machine-checked record for this axis.

## Rejected tradeoffs

- function_table: its lower wrapper dispatch time is four orders of magnitude below one process launch, and it is the only contender that puts template declarations in the private ownership machinery [transport.measurement.function_table, follow_up.dispatch_overhead_scale.result]
- closed_variant: its single server allocation saves one malloc per Server value while shipping a recording backend in production and making every new backend a rewrite of the closed alternative set [transport.measurement.closed_variant, follow_up.dispatch_overhead_scale.result]

## Structured limitations

- dispatch_overhead_scale: material; follow_up_complete; evidence=follow_up.dispatch_overhead_scale.result
- concurrent_hostile_build_mutation: non_material; accepted_non_material; evidence=transport.measurements.v1
- engine_ops_lifecycle: non_material; accepted_non_material; evidence=graft.engine_ops
- engine_ops_materializer_publication: non_material; accepted_non_material; evidence=graft.engine_ops
- engine_ops_not_claimed_transport_selection: non_material; accepted_non_material; evidence=graft.engine_ops
- engine_ops_not_claimed_performance: non_material; accepted_non_material; evidence=graft.engine_ops
- engine_ops_not_claimed_cross_version_tmux_behavior: non_material; accepted_non_material; evidence=graft.engine_ops
- engine_ops_not_claimed_concrete_python_operation_parity: non_material; accepted_non_material; evidence=graft.engine_ops
- engine_ops_process_adapter: non_material; accepted_non_material; evidence=graft.engine_ops
- engine_ops_pre_3_7_real_runtime: non_material; accepted_non_material; evidence=graft.engine_ops
- engine_ops_warning_channel_parity: non_material; accepted_non_material; evidence=graft.engine_ops
- control_mode_release_deadline: non_material; accepted_non_material; evidence=graft.control_mode
