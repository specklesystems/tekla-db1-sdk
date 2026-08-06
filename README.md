# tekla-db1-sdk

A native-first C++20 SDK for read-only processing of Tekla model databases into
output-neutral object, property, relationship, definition-geometry, and
display-geometry batches.

The library is designed for interoperable, read-only access and does not require
a Tekla installation at build time or runtime.

> Status: version 0.1.0 release candidate. Native mapped-file input, raw and multi-member gzip
> decoding, validated physical-table framing, the public batch interface, and
> bounded ZIP discovery, generated schemas for model formats 8.74, 8.95,
> 9.08, 9.21, 9.52, and 9.66
> are implemented. Processing streams modern and historical identities,
> typed user-defined and native part properties, material assignments, and
> relationships and persisted joint/macro occurrences in bounded batches.
> Model-local `xslib.db1` catalogs can be indexed on demand to report whether
> an occurrence name has a stored custom-component definition. Definition and
> display geometry cover
> straight, persisted polyline, legacy circular-arc, variable rectangular,
> contour-plate, lofted-plate, and imported-shape parts. Modern classic single
> bars, planar groups, subtype-6 polygon meshes, subtype-8 bent meshes, grid
> lines, and patterned bolt shanks are also available as output-neutral curve
> or mesh batches. Sections resolve from
> self-describing names, a built-in nominal steel subset, and model-local LIS
> catalogs (including catalogs packaged beside the DB1). Optional OCCT evaluation
> applies persisted Boolean cuts, recursively nested operative graphs, and fitting
> planes behind a contained worker. Straight, contour, polyline-sweep, and
> circular-sweep recipes retain their 2D section loops, path stations, and
> transported frames through topology planning instead of being reconstructed
> from triangle adjacency. Supported recipes enter OCCT as faces, prisms,
> ruled lofts, or revolutions; triangle sewing is a named, bounded compatibility
> fallback. Definition batches expose nominal catalog/report section scalars
> and analytically fitted length; evaluated display meshes expose
> double-precision surface, cover-surface, volume, longitudinal, and transverse
> measurements for output adapters.

The SDK does not yet provide full Tekla visual parity. Unsupported custom and
variable profiles, alternate rebar-mesh encodings, tapered and non-planar
reinforcement groups, complete bolt assemblies, component recipe
evaluation/regeneration, some relation families, active-view filtering, and
the standalone report-geometry stage remain explicit gaps. Exact Tekla cover
area for cut hollow catalog sections is currently tolerance-bounded rather
than byte-identical. See the
[support matrix](docs/support-matrix.md) before choosing it for a production
conversion path.

Release-level changes are recorded in the [changelog](CHANGELOG.md).

## Design

The main interface is intentionally small:

~~~cpp
auto model = tekla::db1::open(std::move(package)).value();
auto stream = model.process(request).value();

while (auto batch = stream->next()) {
  if (batch.value().kind == tekla::db1::BatchKind::end) break;
  adapter.consume(batch.value());
}
~~~

The currently implemented processing requests are `Stage::identities`,
`Stage::properties`, `Stage::relations`, `Stage::semantic_relations`, `Stage::instances`,
`Stage::definition_geometry`, and `Stage::display_geometry`, in any combination.
`Stage::component_definitions` additionally indexes a model-local `xslib.db1`
and annotates persisted component occurrences with `available` or `unavailable`;
this opt-in work is kept off the ordinary processing hot path. Display and
definition batches already carry the output-neutral measurements needed for
the implemented adapter report subset. `Stage::report_geometry` remains an
unfinished independent report-only path and fails explicitly with
`ErrorCode::decoder_unavailable`.

`Stage::relations` retains raw persisted rows for diagnostics and specialized
adapters. `Stage::semantic_relations` emits normalized output-neutral graph
edges: `subelement` is parent-to-child, while `in_assembly` is
member-to-assembly with ordinal zero reserved for the persisted main member,
`connects_to` follows persisted component primary-to-secondary roles, and
`hosted_on` is reinforcement-to-structural-host.
Every semantic edge has two live object endpoints; self-edges and dangling
references are omitted. Boolean parts and cut planes remain available in raw
identity and relationship batches for geometry evaluation and diagnostics, but
are classified as evaluation features and are not semantic endpoints.

ModelPackage owns or shares immutable source buffers. A processed model emits
bounded, columnar views. Output adapters depend on those views and never on
database records or geometry implementation types.

The built-in tekla_db1::gltf target writes GLB. Speckle, USD, USDZ, and other
adapters can live in independent repositories and release on their own cadence.
See [the architecture](docs/architecture.md) and
[external adapter guide](docs/external-adapters.md).

## Build

Requirements:

- CMake 3.25 or newer
- A C++20 compiler
- Ninja for the supplied presets
- zlib

~~~sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
~~~

Inspect a model database without creating an output format:

~~~sh
build/dev/tools/tekla-db1-inspect /path/to/model.db1
build/dev/tools/tekla-db1-inspect /path/to/model.db1 --semantics
build/dev/tools/tekla-db1-inspect /path/to/model.db1 --instances
build/dev/tools/tekla-db1-inspect /path/to/model.db1 --component-definitions
build/dev/tools/tekla-db1-inspect /path/to/model.db1 --geometry
build/dev/tools/tekla-db1-glb /path/to/model.db1 preview.glb
build/dev/tools/tekla-db1-inventory /path/to/model.db1
~~~

The line-delimited inventory includes per-mesh bounds, triangle/vertex counts,
signed-volume magnitude, and surface area, plus per-curve kind, point count,
radius, polyline length, and bounds. With an OCCT build, pass
`--topology-worker build/occt/tools/tekla-db1-occt-worker` to audit evaluated
cuts through the contained worker.

For an optimized warning-strict build:

~~~sh
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
~~~

An ASan/UBSan preset is also available on supported Clang and GCC platforms:

~~~sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
~~~

The redistributable release gates also include deterministic-output checks,
malformed-input contracts, an opt-in libFuzzer harness, a synthetic container
benchmark, and installed-package consumers. Extended model compatibility tests
remain a separate release gate. See the exact commands and acceptance
criteria in the [release checklist](docs/releasing.md).

## Install and consume

~~~sh
cmake --install build/release --prefix /your/prefix
~~~

Consumers can then use:

~~~cmake
find_package(tekla_db1 0.1 REQUIRED COMPONENTS core gltf)
target_link_libraries(my_converter PRIVATE tekla_db1::gltf)
~~~

Version 0.1 builds both `tekla_db1::core` and `tekla_db1::gltf` as static
libraries. A shared-library ABI is not provided or supported in this release;
the pre-1.0 C++ interface may change between releases.

`<tekla/db1/version.hpp>` exposes `tekla::db1::source_revision`, and the
installed CMake package exports the same value as `tekla_db1_SOURCE_REVISION`.
Ordinary Git checkouts detect the full configured `HEAD`. Source archives
without Git metadata report `unknown` unless configured with an exact
`-DTEKLA_DB1_SOURCE_REVISION_OVERRIDE=<hex-revision>` value.

## OCCT execution

There is one optional OCCT evaluator implementation, not two geometry forks. It
can be hosted directly in the converter or in a supervised persistent worker.
The POSIX supervisor turns worker crashes and hard timeouts into scoped errors;
later work can continue through an independently restarted worker. See
[OCCT execution and containment](docs/occt-execution.md).

~~~sh
cmake --preset occt
cmake --build --preset occt
ctest --preset occt
~~~

`Model::process` can route persisted type-9/type-12 fitting planes and type-11
Boolean subtraction relations through either host. Topology is opt-in so the
analytic and future WASM paths do not acquire an OCCT runtime dependency. The
inventory tool also accepts inclusive object-id windows for resumable and
parallel large-model passes. If semantic topology fails or exceeds the worker
deadline, the evaluator makes at most one changed-strategy retry using the
guarded faceted path and preserves the last valid display mesh with an explicit
diagnostic if that retry also fails. Nested circular cutters remain on that
faceted tier until their tolerance envelope can be represented analytically.

## Development policy

Production behavior is maintained through reviewed code, versioned
specifications, and repeatable tests. See
[repository hygiene](docs/repository-hygiene.md).

## License

No distribution license has been selected yet. Select one before publishing
the core or any adapter. External dependency terms and redistribution notes are
listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Tekla is a trademark of Trimble Inc. This project is independent and is not
affiliated with or endorsed by Trimble.
