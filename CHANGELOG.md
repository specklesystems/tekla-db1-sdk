# Changelog

All notable changes to this project are documented in this file. The project
uses semantic versioning for its public C++ package, subject to the pre-1.0 API
compatibility policy in the support matrix.

## Unreleased

### Added

- `ObjectRole`, `object_role`, and `is_model_element` distinguish independently
  publishable model elements from Boolean-part and cut-plane evaluation
  features while retaining all persisted identities for raw processing.

### Changed

- Evaluation features are no longer emitted as endpoints in normalized
  semantic relationships.
- Polyline-part sections retain their local transverse offset as the transported
  frame turns, and sharp stations use mitered section scaling.
- Fitted form-60/form-70 parts build their operative envelope through the
  persisted fitting planes before the final half-space clips are applied.
- Topology keeps non-planar cutter meshes and reversing polyline hosts on the
  exact faceted path instead of promoting them to a different analytic solid.

## 0.1.0 - 2026-08-04

Initial release candidate of the read-only, output-neutral C++20 SDK.

### Added

- Validated native and gzip-wrapped DB1 container access with generated schemas
  for formats 8.74, 8.95, 9.08, 9.21, 9.52, and 9.66.
- Bounded batch streams for identities, typed properties, relationships,
  definition geometry, display meshes, curves, and diagnostics.
- Output-neutral object-family and weld-location classifications while retaining
  raw type/subtype values as compatibility metadata.
- Output-neutral assembly identities and deterministic semantic relationship
  batches for parent-child, member-to-assembly, component-connectivity, and
  reinforcement-host topology, while retaining raw persisted relation rows as
  a separate stage.
- Deterministic canonicalization of colliding typed user-defined attributes to
  one value per object and property path.
- Persisted joint/macro occurrence batches with parent-child ownership and an
  opt-in, decompression-bounded `xslib.db1` definition-name index.
- Analytic part reconstruction for straight, circular-arc, polyline, variable
  rectangular, contour-plate, lofted-plate, and supported imported-shape parts.
- Local LIS profile-catalog and bounded adjacent-package shape discovery.
- Optional contained OCCT evaluation for persisted fittings, cuts, and nested
  Boolean operative graphs.
- Output-neutral report inputs: nominal/catalog section area, cover perimeter,
  height and width, analytically fitted length, material density/type, and
  double-precision evaluated surface area, cover area, volume, longitudinal
  bounds, and transverse bounds. Evaluated transverse dimensions are mapped
  to each profile's nominal catalog orientation before operation reductions.
- Semantic straight/contour extrusion and polyline/circular sweep recipes that
  retain profile loops, path stations, and transported frames through optional
  topology instead of reconstructing them from triangle adjacency.
- A versioned, allocation-bounded OCCT recipe protocol, rigid circular-sweep
  validation, multi-body result retention, and one changed-strategy faceted
  retry for non-terminal semantic topology failures.
- Classic single/group rebar curves, subtype-6 polygon and subtype-8 bent
  rebar-mesh curves, grid lines, patterned modern bolt shanks, and the persisted
  8.95 `UNDEFINED_STUD20*175` headed-stud family.
- Curve-aware JSONL inventory records with kind, radius, point count, length,
  and bounds alongside mesh metrics and diagnostics.
- Built-in deterministic GLB adapter plus an external-adapter CMake seam.
- Cross-platform warning-strict compilation of a public-header-only external
  adapter fixture through that CMake seam.
- Installed CMake package, standalone consumer tests, sanitizer tests, bounded
  fuzzing, deterministic-output checks, and synthetic performance records.
- Configured Git source revision metadata in the public version header and
  installed CMake package, with an explicit source-archive override.

### Known limitations

- This release does not claim full Tekla visual or semantic parity.
- The independent `Stage::report_geometry` path, component recipe evaluation/regeneration,
  active-view filtering, alternate rebar-mesh encodings, tapered and non-planar
  reinforcement groups, complete bolt assemblies, and some alternate contour,
  custom profile, and shape encodings remain explicit unsupported paths.
- Exact Tekla cover area for operated hollow catalog sections remains a
  tolerance-bounded adapter parity case rather than an exact SDK scalar.
- OCCT topology evaluation is optional and excluded from the WASM-oriented core
  path.

See `docs/support-matrix.md` for the precise implemented contract and
`docs/releasing.md` for the release gates.
