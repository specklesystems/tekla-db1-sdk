# Support matrix

This document describes implemented behavior, not the intended final
architecture. Unsupported data is either retained as lower-level semantics or
reported with a machine-readable error; the SDK does not silently substitute
guessed geometry.

## Inputs and containers

| Capability | Status | Notes |
| --- | --- | --- |
| Native file mapping | Implemented | Read-only mapping on POSIX and Windows, with a portable owned-buffer fallback. |
| Raw model database payload | Implemented | Zero-copy after mapping. |
| Gzip-wrapped payload | Implemented | Concatenated members and a configurable decompressed-size limit. |
| Adjacent project archive | Implemented subset | Adjacent ZIP assets are bounded, CRC-checked, and inspected for stored or deflated LIS profile catalogs plus `Shapes` metadata and `ShapeGeometries` meshes. One mapped package accepts at most 64 archives, 65,536 central-directory entries, and 256 MiB of declared inflated catalog bytes. Encrypted and ZIP64 entries fail closed. |
| Physical table directory | Implemented | Validates table framing, descriptors, record alignment, and known footers. |
| Model database formats | Implemented subset | Exact schemas for 8.74, 8.95, 9.08, 9.21, 9.52, and 9.66. Unknown structural variants fail explicitly. |
| Model-local profile catalogs | Implemented subset | Text LIS snapshots are resolved from package assets, `Environment/Profiles`, and project `PG.lis` entries; unrelated catalog database roles remain opaque assets. |
| Model-local component catalog | Implemented subset | A readable adjacent `xslib.db1` is discovered as a lazy optional component-catalog asset without making ordinary model opening map or decode the sidecar. `Stage::component_definitions` validates its model UUID/format, enforces a decompressed-size cap, strictly projects its required keyed tables onto the generated schema, and builds a transient definition-name index proportional to definition rows. Recipe execution is not implied. |

## Semantic processing

| Capability | Status | Notes |
| --- | --- | --- |
| Object identities | Implemented | Modern binary GUIDs and historical text-GUID joins. Stable `ObjectKind` classifications cover persisted part, assembly, component, grid, bolt, Boolean, weld, and reinforcement families while raw type/subtype values remain available as compatibility metadata. `ObjectRole` distinguishes independently meaningful model elements from Boolean-part and cut-plane evaluation features without removing either from raw identity batches. Welds additionally expose persisted workshop/site location semantics. |
| Native part properties | Implemented subset | Name, profile, profile parameters, material, finish, class, and form type. |
| User-defined attributes | Implemented | Integer, floating-point, text, and reference values. If multiple persisted definitions produce the same object/group/name path, one value is emitted deterministically: references outrank numeric values, which outrank text, with stable source/row order breaking equal-type ties. |
| Relations | Implemented subset | `Stage::relations` streams stored rows and visibility state. `Stage::semantic_relations` normalizes persisted parent and evidenced stored-relation routes into parent-to-child `subelement` edges, assembly membership into member-to-assembly `in_assembly` edges, persisted joint primary/secondary roles into directed `connects_to` edges, and persisted host/reinforcement links into reinforcement-to-host `hosted_on` edges. Main assembly members use ordinal zero; secondary and nested members use deterministic positive ordinals. Self-edges, duplicates, dangling endpoints, and evaluation-feature endpoints are omitted. Boolean parts and cut planes remain available through raw identities and relations. Weld, bolt, and geometry-proximity connectivity are not inferred. |
| Component occurrences | Implemented subset | Streams persisted joint and macro occurrences, native names/descriptions/input IDs, visibility, and persisted child counts. Child objects retain independent identities and point to the occurrence through `ObjectView::parent_id`. Optional catalog-name resolution reports `not_checked`, `unavailable`, or `available`. |
| Material assignments | Implemented | Emits object-scoped material, finish, and class views. |
| Report/computed properties | Implemented subset | Definition geometry exposes nominal/catalog section area, cover perimeter, height, width, and analytically fitted length. Evaluated meshes expose double-precision surface area, hollow-section cover area, volume, longitudinal bounds, and transverse bounds; material batches expose report type and density. Adapters can derive AREA, VOLUME, WEIGHT, LENGTH, HEIGHT, WIDTH, PROFILE_TYPE, and MATERIAL_TYPE for supported parts. The independent `Stage::report_geometry` path still fails with `decoder_unavailable`; exact Tekla cover area for operated hollow catalog sections is tolerance-bounded. |
| View filtering and dependency closure | Not implemented | Stored object visibility is retained, but active-view rules are not evaluated. |
| Resumable geometry scheduling | Implemented | Inclusive internal-id windows allow deterministic sharding; in-process scheduling remains caller-owned. |

## Geometry processing

| Capability | Status | Notes |
| --- | --- | --- |
| Straight-part definitions | Implemented | Emits stored coordinate frame, length, profile spelling, and form type. |
| Variable rectangular parts | Implemented subset | PL_V forms 51 and 61 are emitted as rectangular frusta; persisted type-11 socket children are subtracted when topology evaluation is enabled. |
| Literal rectangular and plate profiles | Implemented | Analytic extrusion. |
| Literal angle, round, and hollow profiles | Implemented | Analytic extrusion for self-describing dimensions. |
| Catalog-backed profiles | Implemented subset | Model-local LIS outlines and a built-in nominal steel subset; unresolved profiles remain explicit diagnostics. |
| Curved beams and polybeams | Implemented subset | Persisted zero-radius polyline paths and legacy circular-arc members retain their 2D section, ordered path stations, and transported frames through direct tessellation and optional topology. Circular revolution is used only after rigid-section validation; otherwise all retained sections form a ruled sweep. |
| Contour plates | Implemented subset | Persisted chamfered contours are triangulated and extruded. Alternate contour encodings that do not expose the supported persisted polygon path remain explicit diagnostics. |
| Legacy lofted plates | Implemented | Type/subtype 1000/1000 rail trees are decoded from six-value segment arrays and thickened as ruled surfaces. |
| Imported shapes | Implemented subset | Model-local and bounded adjacent-package polygon meshes are decoded and instantiated; project shapes without supplied metadata and unsupported shape encodings remain explicit diagnostics. |
| Stored cuts, fittings, and Boolean graphs | Implemented subset | Visible relation types 9 and 12 become retained half-spaces; type 11 subtracts a flat, nested CSG graph through optional OCCT. Straight/contour hosts and eligible cutters remain semantic prisms; visible polyline and circular hosts remain semantic sweeps. Circular, lofted, imported, and other tolerance-sensitive cutters use the guarded faceted tier. Operatives recursively evaluate their own persisted operation graphs with cycle, depth, node, and edge limits. Evaluated operatives and deterministic failures are memoized per processing stream with a 4,096-entry limit and a 256 MiB mesh/error-payload limit; active-path cycles and transient worker failures are never cached. A semantic graph failure is retried once through the faceted tier; if that also fails, the edge is diagnosed and omitted without suppressing the top-level host. Simple persisted type-79 straight-corner chamfers are replayed. Analytic prism cutters receive host-tangent tolerance envelopes before OCCT evaluation. |
| Classic reinforcement | Implemented subset | Modern 9.52/9.66 single bars, planar groups, subtype-6 polygon meshes, and subtype-8 bent meshes become physical-radius `CurveView` batches. Bent-mesh curves apply persisted offsets, layer separation, physical bend radii, and cover adjustment along supported planar paths. Alternate mesh encodings, tapered and non-planar groups, splices, and legacy identity layouts remain explicit diagnostics. |
| Bolt groups | Implemented subset | Modern patterned bolt shanks become closed cylinder meshes from persisted diameter, frame, length, and pattern rows. Historical 8.95 object/attribute joins and the persisted `UNDEFINED_STUD20*175` headed-stud family are also evaluated, including the stored non-zero shank eligibility rule. Other heads, nuts, washers, and special anchors are not yet evaluated. |
| Grid lines | Implemented subset | Modern child grid-plane relations become zero-radius renderer lines at the parent grid elevation. Labels and legacy identity layouts are not emitted. |
| Component evaluation | Not implemented | Catalog recipe equations, parameter/input binding, generated-object regeneration, and expansion of definitions whose output is not already persisted are not evaluated. Persisted child geometry continues through the ordinary part, bolt, and reinforcement paths. |
| GLB output | Implemented for emitted meshes and curves | Built-in adapter preserves zero-radius curves as line primitives and tessellates physical-radius curves into capped tubes. Visual completeness is limited by the geometry rows above. |

## OCCT containment

With `TEKLA_DB1_ENABLE_OCCT=ON`, the build contains one Boolean/tessellation
evaluator and two hosts. The direct host runs in process. The POSIX supervised
host uses a persistent worker, a bounded binary protocol, a wall-clock
deadline, and process replacement after a timeout or crash. Non-POSIX
supervised hosting currently fails explicitly. Topology evaluation is opt-in;
the base library and WASM-oriented path remain free of an OCCT dependency.
Every completed OCCT Boolean result is retained because an operand may validly
intersect only a fraction of its persisted volume. Crash and timeout isolation,
rather than a volume heuristic, is the non-terminal failure boundary.

## Compatibility promises

- The C++ API is pre-1.0 and may change.
- Version 0.1 provides static core and glTF libraries only, with no supported
  shared-library ABI.
- The installed headers expose the package version through
  `tekla::db1::version_major`, `version_minor`, `version_patch`, and `version`,
  plus the configured source revision through `source_revision`.
- No stable C ABI or WASM binding is promised yet.
- Unsupported format variants, database roles, and geometry classes produce
  explicit errors or diagnostics rather than best-effort guessed output.
- Extended model compatibility tests are maintained separately; public CI
  remains synthetic and redistributable.
- Public release candidates must pass the warning-strict, sanitizer,
  malformed-input, deterministic-output, install/export, and benchmark-smoke
  gates documented in [releasing](releasing.md).
