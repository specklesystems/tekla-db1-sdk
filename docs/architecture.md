# Architecture

## Module boundary

tekla_db1::core is the deep module. It owns model-package validation,
container decoding, versioned record access, semantic evaluation, and geometry
processing. Geometry requests can be divided into deterministic internal-id
windows; dependency closure and an in-process concurrent scheduler remain
planned behind the same boundary.

Its public surface is:

1. immutable package assets,
2. open,
3. Model::process,
4. BatchReader::next,
5. stable errors and diagnostics.

Database records, catalog layouts, operation evaluators, caches, OCCT objects,
and scheduling internals never cross this boundary.

## Processing flow

~~~text
ModelPackage
    |
    v
open -> immutable Model
    |
    v
process(ProcessRequest) -> BatchReader
    |
    +-- object/property/relation batches
    +-- definition-geometry batches
    +-- material/mesh/curve/instance batches
    |     `-- output-neutral catalog and evaluated report scalars
    +-- deterministic diagnostics
~~~

Batch views are valid until the next call to BatchReader::next. Adapters must
consume or copy them before advancing. This permits the processor to reuse
bounded arenas without imposing an object graph on callers.

## Core implementation layers

- Package sources: owned memory and native read-only mappings behind the same
  immutable ByteSource interface. Callback-backed buffers remain future work.
- Container: raw and concatenated-gzip decoding, header validation, and a
  physical-section directory with bounded decompression.
- Schema: generated, format-specific record views over immutable bytes.
- Semantics: identities, typed values, raw persisted relationships, normalized
  parent/assembly, component-connectivity, and reinforcement-host graph edges,
  persisted component occurrences, and support status.
- Geometry definitions: positioning, persisted contours and paths, literal and
  model-local profiles, imported shapes, cuts, fittings, and operation graphs.
- Evaluation: analytic geometry first, OCCT for topology-heavy operations.
- Scheduler: planned bounded work queues, deterministic merge, and
  content-addressed caches.

The semantic topology route retains straight and contour extrusions as planar
boundary loops plus an extrusion vector. Polyline and circular sweeps retain
their 2D section loops, ordered path stations, and transported section frames;
topology is never rediscovered from triangle adjacency or copied back out of a
tessellated mesh. Unmodified objects still use the direct tessellator. When
persisted features require OCCT, the adapter builds prisms, ruled sweeps, or
circular revolutions from those recipes rather than reconstructing the visible
host from triangles. Circular revolution is an optimization only after every
retained station is proven to be the rigid rotation of the first. Otherwise all
stations enter the ruled-sweep evaluator. A hollow circular member is one
revolved face containing inner wires, not an outer-minus-inner synthetic
Boolean.

Unsupported or tolerance-sensitive recipe families continue through the
explicitly named guarded mesh fallback. Straight and contour-prism Boolean
operands are promoted after their tolerance envelope is applied. Circular
Boolean operands remain faceted for now because their host-relative tolerance
envelope is defined at retained stations; revolving that adjusted envelope
would change its between-station boundary. If a semantic CSG request still
fails or times out, the planner rebuilds the same graph once through the
guarded faceted tier and records the downgrade as an object-scoped diagnostic.
Valid multi-body Boolean results remain one display result containing every
material body; the evaluator does not discard bodies with a largest-volume
heuristic.

Definition geometry carries nominal/catalog section measurements and fitted
member length. The final evaluated mesh carries double-precision mass and
bounding measurements computed before its public vertices are narrowed to
float. Adapters own report naming, units, density fallbacks, and tolerances;
they do not rebuild engineering scalars from the transport mesh. This subset
does not make the separate `Stage::report_geometry` path available.

## Output neutrality

The core emits both definition geometry and evaluated display geometry.
Speckle EAV, Arrow, Parquet, glTF, OpenUSD, and USDZ concepts belong to adapters.

The GLTF adapter is kept in this repository because it is small, neutral, and
provides an immediate visual regression surface. Other adapters may be
submodules during development or completely independent packages.

The analytic evaluator is deliberately conservative. It meshes supported
straight, contour, persisted-polyline, circular-arc, and imported-shape
definitions. It exposes renderer lines and physical-radius centerlines without
forcing adapters to accept a core tessellation policy; the built-in GLTF
adapter chooses line primitives for grids and capped tubes for reinforcement.
An unresolved catalog profile is a definition plus a diagnostic, never a
guessed box. Persisted cuts and fitting planes are evaluated only when the
caller opts into the optional OCCT backend.

Model-local `xslib.db1` is a keyed subset of the same DBdict rather than a model
database with matching physical ordinals. The component-definition stage
strictly projects each required semantic table by stable key, tuple width, and
descriptor vector before reusing semantic record decoders. Catalog
decompression and indexing are opt-in, decompression-bounded, and never occur
during ordinary identity, property, relation, or geometry processing. The
current contract resolves definition presence only; it does not expose or
execute component recipes.

## Compatibility

The checked-in schema directory currently recognizes model database formats
8.74, 8.95, 9.08, 9.21, 9.52, and 9.66. Schema binding verifies the complete
physical table count, tuple widths, descriptor vectors, and available table
keys before semantic processing. Files with the same textual format but a
different database role, such as catalog databases, remain valid package
assets and are decoded only through an explicit role-specific stage.

The C++ interface may evolve before 1.0. Version 0.1 builds the core and built-in
glTF adapter as static libraries only; it does not provide a shared-library ABI.
A stable C ABI will be introduced before WASM bindings, plug-in binary
distribution, or cross-compiler ABI compatibility are promised.
