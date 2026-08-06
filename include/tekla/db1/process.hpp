#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <tekla/db1/result.hpp>

namespace tekla::db1 {

enum class Stage : std::uint32_t {
  none = 0,
  identities = 1U << 0U,
  properties = 1U << 1U,
  relations = 1U << 2U,
  definition_geometry = 1U << 3U,
  display_geometry = 1U << 4U,
  report_geometry = 1U << 5U,
  instances = 1U << 6U,
  component_definitions = 1U << 7U,
  semantic_relations = 1U << 8U,
};

enum class TopologyMode {
  disabled,
  direct,
  supervised,
};

constexpr Stage operator|(Stage lhs, Stage rhs) noexcept {
  return static_cast<Stage>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

struct ProcessRequest {
  Stage stages = Stage::identities | Stage::properties | Stage::relations | Stage::display_geometry;
  // Soft limit used to size each reusable output batch. Persistent lookup
  // indexes and adapter-owned copies are outside this budget.
  std::uint64_t batch_memory_budget_bytes = 0;
  // Hard aggregate cap for linked-data decode caches and geometry retained by
  // the non-part geometry stage outside reusable output batches. Part geometry
  // and topology caches are not included. Zero derives an O(source-payload)
  // limit for this stage.
  std::uint64_t geometry_memory_budget_bytes = 0;
  // Hard decompressed-size cap for an explicitly requested model-local
  // component database. Zero uses the SDK default of 1 GiB. The transient
  // definition-name index is proportional to catalog definition rows and is
  // outside this decompression cap. Component catalogs stay off the ordinary
  // semantic and geometry hot paths.
  std::uint64_t component_catalog_memory_budget_bytes = 0;
  // Analytic geometry remains the default hot path. Enable topology only when
  // persisted cuts or fitting planes must be evaluated. Supervised mode keeps
  // backend crashes and hangs object-scoped.
  TopologyMode topology_mode = TopologyMode::disabled;
  std::string_view topology_worker_path;
  std::uint32_t topology_timeout_milliseconds = 10'000;
  // Inclusive internal-id window for resumable or parallel geometry passes.
  // Semantic stages are intentionally unaffected: adapters can still resolve
  // names and properties for every selected geometry object.
  std::uint64_t geometry_object_id_min = 0;
  std::uint64_t geometry_object_id_max = std::numeric_limits<std::uint64_t>::max();
};

struct MeshView {
  std::uint64_t object_id = 0;
  std::span<const float> positions;
  std::span<const std::uint32_t> indices;
  // Scalar measurements are evaluated from the final double-precision mesh
  // before public float vertices are produced. They let output adapters emit
  // engineering/display reports without rebuilding topology from a lossy
  // transport representation.
  double surface_area = 0.0;
  // Surface area visible from outside a hollow section. Unlike total mesh
  // area, this excludes the wall of the section's internal void while keeping
  // real end, fitting, and cut faces.
  double cover_surface_area = 0.0;
  double volume = 0.0;
  double longitudinal_min = 0.0;
  double longitudinal_max = 0.0;
  double section_y_min = 0.0;
  double section_y_max = 0.0;
  double section_z_min = 0.0;
  double section_z_max = 0.0;
  bool has_report_metrics = false;
  bool has_cover_surface_area = false;
  bool has_section_extents = false;
};

struct Vector3d {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

enum class CurveGeometryKind {
  polyline,
  line_segment,
};

// Output-neutral line geometry. A positive radius describes a physical
// circular sweep (for example reinforcement); zero is a renderer line (for
// example a grid). Points are in model coordinates and units.
struct CurveView {
  std::uint64_t object_id = 0;
  CurveGeometryKind kind = CurveGeometryKind::polyline;
  std::span<const Vector3d> points;
  double radius = 0.0;
};

enum class DefinitionGeometryKind {
  straight_extrusion,
  circular_arc_extrusion,
  polyline_extrusion,
  lofted_plate,
  imported_shape,
};

struct DefinitionGeometryView {
  std::uint64_t object_id = 0;
  DefinitionGeometryKind kind = DefinitionGeometryKind::straight_extrusion;
  std::string_view profile;
  Vector3d origin;
  Vector3d x_axis;
  Vector3d y_axis;
  Vector3d z_axis;
  double length = 0.0;
  std::uint32_t form_type = 0;
  double radius = 0.0;
  std::uint32_t segment_count = 0;
  std::span<const Vector3d> path;
  // Nominal section measurements are evaluated before fittings and Boolean
  // operations. Zero with has_section_metrics=false means unavailable rather
  // than a physical zero-size section.
  double section_area = 0.0;
  double section_height = 0.0;
  double section_width = 0.0;
  bool has_section_metrics = false;
  // Nominal catalog/report values. These can differ from measurements of the
  // high-accuracy display contour (for example IPE200 is nominally 2850 mm2
  // while its faceted contour measures about 2859.95 mm2).
  double report_section_area = 0.0;
  double report_cover_perimeter = 0.0;
  double report_section_height = 0.0;
  double report_section_width = 0.0;
  bool has_report_section_metrics = false;
  // Longitudinal extent of the nominal section after persisted fitting
  // planes. This is evaluated analytically in double precision rather than
  // recovered from tessellated display bounds. An oblique fitting can leave a
  // section corner beyond the reference-line intersection, so reported length
  // includes that retained solid extent.
  double report_length = 0.0;
  bool has_report_length = false;
  // Catalog area/perimeter times report_length is valid for an unmodified
  // prism and for fitting-only members. Boolean cuts and chamfers require the
  // evaluated report solid instead.
  bool catalog_report_scalars_eligible = false;
  bool has_operations = false;
};

// Stable, output-neutral semantic classification. Raw database type/subtype
// numbers are dialect implementation details and intentionally remain available
// only as compatibility metadata on ObjectView; adapters should switch on this value.
enum class ObjectKind {
  unknown,
  beam,
  contour_plate,
  poly_beam,
  brep,
  lofted_plate,
  connection,
  component,
  grid,
  bolt_array,
  boolean_part,
  cut_plane,
  weld,
  grid_plane,
  single_rebar,
  rebar_group,
  rebar_mesh,
  assembly,
};

enum class WeldLocation {
  not_applicable,
  unknown,
  workshop,
  site,
};

struct ObjectView {
  std::uint64_t internal_id = 0;
  std::string_view application_id;
  std::uint64_t parent_id = 0;
  std::uint64_t assembly_id = 0;
  std::uint32_t type = 0;
  std::uint32_t subtype = 0;
  ObjectKind kind = ObjectKind::unknown;
  WeldLocation weld_location = WeldLocation::not_applicable;
  std::uint32_t object_flags = 0;
  std::uint32_t row_id = 0;
  std::uint32_t event_id = 0;
  bool visible = true;
};

enum class PropertyValueKind {
  integer,
  floating,
  text,
  reference,
};

struct PropertyView {
  std::uint64_t object_id = 0;
  std::string_view group;
  std::string_view name;
  PropertyValueKind kind = PropertyValueKind::text;
  std::int64_t integer_value = 0;
  double floating_value = 0.0;
  std::string_view text_value;
  std::uint64_t reference_id = 0;
};

struct RelationView {
  std::uint64_t relation_id = 0;
  std::uint32_t type = 0;
  std::uint64_t source_id = 0;
  std::uint64_t target_id = 0;
  std::uint32_t flags = 0;
  std::uint32_t row_id = 0;
  std::uint32_t event_id = 0;
  bool visible = true;
};

enum class SemanticRelationKind {
  subelement,
  in_assembly,
};

enum class SemanticRelationOrigin {
  object_parent,
  stored_relation,
  assembly_membership,
};

// Output-neutral graph semantics reconstructed from persisted DB1 state.
// SUBELEMENT is parent -> child. IN_ASSEMBLY is member -> assembly, with
// ordinal zero reserved for the assembly's persisted main member.
struct SemanticRelationView {
  SemanticRelationKind kind = SemanticRelationKind::subelement;
  std::uint64_t source_id = 0;
  std::uint64_t target_id = 0;
  std::uint32_t ordinal = 0;
  SemanticRelationOrigin origin = SemanticRelationOrigin::object_parent;
  std::uint64_t source_relation_id = 0;
};

enum class InstanceKind {
  joint,
  macro,
};

enum class ComponentDefinitionStatus {
  not_checked,
  unavailable,
  available,
};

// A persisted component occurrence. Child objects keep their own identities
// and geometry; ObjectView::parent_id points back to object_id. This avoids
// duplicating meshes while allowing graph-oriented adapters to reconstruct the
// component hierarchy. Definition evaluation is deliberately separate from
// this stored-instance contract.
struct InstanceView {
  std::uint64_t object_id = 0;
  InstanceKind kind = InstanceKind::joint;
  std::string_view name;
  std::string_view description;
  std::uint32_t number = 0;
  std::uint32_t type = 0;
  std::uint64_t primary_object_id = 0;
  std::uint64_t secondary_object_id = 0;
  std::uint32_t secondary_object_count = 0;
  std::uint32_t persisted_child_count = 0;
  ComponentDefinitionStatus definition_status = ComponentDefinitionStatus::not_checked;
  bool visible = true;
};

struct MaterialView {
  std::uint64_t object_id = 0;
  std::string_view name;
  std::string_view finish;
  std::string_view object_class;
  // Model-local catalog values used by output adapters for engineering
  // reports. Empty/zero means the optional catalog did not classify the
  // material; adapters may apply format-specific fallbacks or omit the row.
  std::string_view report_type;
  double density_kg_m3 = 0.0;
};

struct Diagnostic {
  ErrorCode code = ErrorCode::none;
  std::uint64_t object_id = 0;
  std::string_view message;
};

enum class BatchKind {
  objects,
  properties,
  relations,
  semantic_relations,
  materials,
  definition_geometry,
  meshes,
  curves,
  instances,
  diagnostics,
  end,
};

struct BatchView {
  BatchKind kind = BatchKind::end;
  std::span<const ObjectView> objects;
  std::span<const PropertyView> properties;
  std::span<const RelationView> relations;
  std::span<const SemanticRelationView> semantic_relations;
  std::span<const InstanceView> instances;
  std::span<const MaterialView> materials;
  std::span<const DefinitionGeometryView> definitions;
  std::span<const MeshView> meshes;
  std::span<const CurveView> curves;
  std::span<const Diagnostic> diagnostics;
};

class BatchReader {
 public:
  virtual ~BatchReader() = default;

  // Views returned by next() remain valid until the next call to next().
  [[nodiscard]] virtual Result<BatchView> next() = 0;
};

using ProcessStream = std::unique_ptr<BatchReader>;

}  // namespace tekla::db1
