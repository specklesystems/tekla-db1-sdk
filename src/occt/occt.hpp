#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <tekla/db1/result.hpp>
#include <vector>

namespace tekla::db1::detail {

struct OcctBox {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double size_x = 0.0;
  double size_y = 0.0;
  double size_z = 0.0;
};

struct OcctTriangleMesh {
  std::vector<double> positions;
  std::vector<std::uint32_t> indices;
};

struct OcctHalfSpace {
  double origin_x = 0.0;
  double origin_y = 0.0;
  double origin_z = 0.0;
  double normal_x = 0.0;
  double normal_y = 0.0;
  double normal_z = 0.0;
};

// A retained planar loop. Positions are model-space XYZ triples and the loop
// is implicitly closed. The first loop of an extrusion is its outer boundary;
// subsequent loops are holes.
struct OcctLoop {
  std::vector<double> positions;
  bool operator==(const OcctLoop&) const = default;
};

// Semantic prism input for the topology backend. Keeping loops and the sweep
// vector intact avoids the triangle -> face -> sewing reconstruction used by
// the compatibility mesh path.
struct OcctExtrusion {
  std::vector<OcctLoop> loops;
  double vector_x = 0.0;
  double vector_y = 0.0;
  double vector_z = 0.0;
  bool operator==(const OcctExtrusion&) const = default;
};

// A solid sweep retained as an ordered sequence of closed section wires.
// Adjacent sections are joined by ruled faces. This preserves the exact
// frames chosen by the DB1 geometry decoder without reconstructing topology
// from its display triangles.
struct OcctSweepLoop {
  std::vector<OcctLoop> sections;
  bool operator==(const OcctSweepLoop&) const = default;
};

struct OcctRuledSweep {
  // The first loop is the material boundary; following loops are voids.
  std::vector<OcctSweepLoop> loops;
  bool circular_spine = false;
  bool operator==(const OcctRuledSweep&) const = default;
};

// A flat, index-addressed CSG graph keeps every nested Boolean operative as an
// OCCT shape until the visible root is complete.  In particular, child results
// are never tessellated and sewn again before their parent subtraction.
struct OcctShapeNode {
  std::uint64_t object_id = 0;
  OcctBox base;
  OcctExtrusion base_extrusion;
  OcctRuledSweep base_ruled_sweep;
  OcctTriangleMesh base_mesh;
  std::vector<OcctBox> subtract;
  std::vector<OcctHalfSpace> keep_half_spaces;
  std::vector<std::uint32_t> union_nodes;
  std::vector<std::uint32_t> subtract_nodes;
};

struct OcctRequest {
  std::uint64_t object_id = 0;
  OcctBox base;
  std::vector<OcctBox> subtract;
  // Triangle soups are accepted at this internal boundary so every analytic
  // SDK geometry family can share one contained topology evaluator. The
  // evaluator sews each closed soup into a solid before applying operations.
  OcctTriangleMesh base_mesh;
  std::vector<OcctTriangleMesh> subtract_meshes;
  std::vector<OcctHalfSpace> keep_half_spaces;
  // When non-empty, node zero is the visible root and the legacy fields above
  // are ignored.  Children are indices into this vector and must form an
  // acyclic graph whose edges point away from the root.
  std::vector<OcctShapeNode> nodes;
  double linear_deflection = 0.5;
  double angular_deflection = 0.5;
};

struct OcctMesh {
  std::uint64_t object_id = 0;
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
  // Engineering quantities are evaluated from the retained OCCT shape before
  // display tessellation and float conversion.  They are intentionally kept
  // separate from the triangle payload.
  double exact_surface_area = 0.0;
  double exact_volume = 0.0;
  bool has_exact_metrics = false;
};

using OcctEvaluatorFunction = Result<OcctMesh> (*)(const OcctRequest&);

// Translates kernel and standard-library exceptions at the single evaluator
// boundary. Kept separate from the OCCT implementation so containment can be
// exercised deterministically without relying on kernel-version-specific bad
// topology.
[[nodiscard]] Result<OcctMesh> invoke_occt_guarded(const OcctRequest& request,
                                                   OcctEvaluatorFunction evaluator);

// This is the only OCCT geometry implementation. Hosts delegate to it rather
// than carrying independent Boolean or tessellation code paths.
[[nodiscard]] Result<OcctMesh> evaluate_occt(const OcctRequest& request);

class DirectOcctHost {
 public:
  [[nodiscard]] Result<OcctMesh> evaluate(const OcctRequest& request) const;
};

class SupervisedOcctHost {
 public:
  SupervisedOcctHost(std::filesystem::path worker, std::uint32_t timeout_milliseconds);
  ~SupervisedOcctHost();
  SupervisedOcctHost(SupervisedOcctHost&&) noexcept;
  SupervisedOcctHost& operator=(SupervisedOcctHost&&) noexcept;

  SupervisedOcctHost(const SupervisedOcctHost&) = delete;
  SupervisedOcctHost& operator=(const SupervisedOcctHost&) = delete;

  [[nodiscard]] Result<OcctMesh> evaluate(const OcctRequest& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tekla::db1::detail
