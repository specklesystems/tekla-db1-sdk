# OCCT execution and containment

## Decision

The optional OCCT module maintains one evaluator and two execution hosts:

- **Direct host**: calls the evaluator in-process with minimal overhead.
- **Supervised host**: sends the same work request to persistent worker
  processes.

These are not separate geometry implementations. Both execute the same
operation model, tolerances, and OCCT code and must produce the same result
hashes.

Topology requests distinguish semantic extrusion and sweep nodes from
compatibility mesh nodes. An extrusion carries one outer loop, zero or more
inner loops, and a model-space vector through the worker protocol. A sweep
carries the ordered outer/inner sections evaluated from the retained 2D loop,
path stations, and transported frames, plus its spine kind. The evaluator
constructs faces and prisms directly and uses ruled OCCT sections for persisted
polyline sweeps. It revolves one face-with-holes only when every transmitted
section satisfies the same rigid circular transform; a mismatched circular hint
falls back to the complete ruled sections without discarding intermediate
geometry. Mesh nodes remain supported for imported or not-yet-promoted
families, and are reported as `mesh_fallback` when profiling is enabled.

For Boolean operands, the planner recognizes the final bounded two-ring prism
after tangent tolerances have been applied and transmits that exact loop and
vector. Circular cutters currently stay on the guarded mesh path: their
host-relative tolerance envelope has not yet been expressed as an analytic
feature. Lofted, imported, and otherwise non-prismatic cutters also remain on
that tier. A failed or timed-out semantic graph is retried once as the same
guarded faceted graph; a successful retry preserves display geometry and emits
an explicit downgrade diagnostic.

## Why a process boundary remains necessary

An exception or ordinary failure result can be handled in-process. An infinite
loop, deadlock, stack overflow, allocator corruption, or access violation
cannot be reliably cancelled or recovered from inside the same C++ process.

A watchdog thread is insufficient: standard C++ has no safe mechanism to kill
an arbitrary stuck thread while preserving process integrity.

The POSIX supervised host currently:

1. assign one identifiable work item to a persistent worker,
2. enforces a wall-clock limit,
3. kill and replace the worker after a timeout or crash,
4. emit an object-scoped diagnostic,
5. retain the object's identity and properties,
6. continue processing unrelated objects.

This turns terminal native failures into non-terminal conversion failures.

The worker protocol rejects graph, recipe-loop, section, and coordinate counts
against explicit request-wide limits before reserving decoded vectors. Protocol
messages remain capped at 512 MiB. The direct host deliberately has no process
deadline and is therefore a trusted-input/high-throughput option; production
processing of external models should use the supervised host.

## Performance strategy

The earlier process bridge paid startup and serialization cost per operation.
Each supervised-host instance owns one warm worker and uses a compact binary
protocol. A scheduler can own multiple hosts to form a bounded pool. Each
worker processes many successful requests during its lifetime.

Version 0.1 processes one geometry stream serially. High-throughput callers
should partition work with the inclusive object-ID window in `ProcessRequest`,
run a bounded number of independent streams, and merge by object ID. The SDK
does not yet provide that scheduler or deterministic multi-stream merge itself;
the bundled `tekla-db1-glb` preview tool also uses one stream.

The intended default is hybrid:

- analytic operations and proven low-risk OCCT work execute directly;
- historically risky Boolean, healing, and imported-shape operations execute
  in supervised workers;
- strict mode can route every OCCT operation through workers;
- trusted/high-throughput mode can use direct execution exclusively.

Work items remain individually attributable. Batching may amortize transport,
but the supervisor checkpoints each object so that a killed worker does not
silently discard an unknown range.

With `TEKLA_DB1_OCCT_PROFILE` enabled, each graph node reports `base_kind` and
the aggregate event reports operation and definition kind, input and output
bounds, tessellation tolerances, outcome, output counts, analytic-extrusion,
analytic-sweep, mesh-fallback, and box-node counts. The binary protocol also
reports request/response sizes. This makes representation regressions
observable in production benchmarks instead of inferring them only from
runtime stalls.

Type-11 operative parts can themselves own fittings, cuts, or further
operatives. That graph is evaluated depth-first before the resulting cutter is
submitted to its host. Recursion is bounded to 32 levels, 4,096 visited nodes,
and 65,536 relation edges per top-level object. Active-path cycle detection
fails the offending edge explicitly. A nested worker failure or graph-limit
failure omits that cutter, retains the last valid host mesh, and allows later
objects to continue through the same supervised-host restart contract.
Successful operative meshes are memoized within one processing stream by
operative identity, topology mode, and tessellation parameters. Deterministic
geometry/topology failures use the same cache, while active-path cycles,
timeouts, worker crashes, and I/O failures are always retried in their current
context. The cache retains at most 4,096 entries and 256 MiB of mesh/error
payload; after either limit it continues uncached rather than evicting live
results or failing model processing.

Completed top-level topology results have a second, placement-independent
cache. Its key is the complete worker request after object identifiers have
been removed and every spatial position has been translated to the root
origin. Coordinates in the key are rounded to one nanometre in model units to
discard arithmetic noise introduced by otherwise identical placements;
directions, dimensions, topology, indices, tolerances, and operation order stay
byte-exact. A hit translates the retained display mesh to the current origin
and reuses the exact OCCT surface-area and volume metrics. Rotated, scaled, or
otherwise changed requests therefore remain distinct. This cache has the same
4,096-entry and 256 MiB fail-open bounds as the operative cache.

The cache is tested with repeated placements and non-repetitive workloads.
Validation checks object, vertex, triangle, mesh, curve, and diagnostic counts,
plus bounded differences in mesh bounds and derived measurements.
`TEKLA_DB1_OCCT_CACHE_PROFILE` emits cache insert/hit records without changing
ordinary output. Set
`TEKLA_DB1_DISABLE_OCCT_CACHE` to bypass both lookup and insertion for a
current-code differential run or as an operational escape hatch; ordinary
processing keeps the cache enabled.

## Failure contract

Timeouts, crashes, invalid topology, and unsupported operations have distinct
diagnostic codes. Unless the caller requests fail-fast processing, they do not
abort the model:

- semantic output remains available,
- definition geometry remains available when decoded,
- the last valid display mesh is retained when a changed-strategy fallback also
  fails,
- the diagnostic records object identity, stage, backend, and failure class.

The worker protocol and direct host are tested against the same evaluator
contract, including exact result parity, warm-worker reuse, forced timeout, and
forced process termination. The supervised host currently has a POSIX
implementation; other platforms fail explicitly until their process host is
implemented.

Completed OCCT shapes are checked with `BRepCheck_Analyzer` and must contain at
least one material solid before tessellation. Disconnected valid solids remain
in the one display result. The supervisor's deadline applies to one complete
object-graph transaction because a native OCCT Boolean cannot be interrupted
inside the worker; expiry kills the process and bounds every operation in that
transaction together.
