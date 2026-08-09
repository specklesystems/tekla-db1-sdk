#pragma once

#include <optional>
#include <span>
#include <tekla/db1/process.hpp>
#include <vector>

namespace tekla::db1::detail {

struct ProjectedMeshPositions {
  std::vector<float> positions;
  MeshCoordinateMode coordinate_space = MeshCoordinateMode::model_space;
  RigidPlacementView placement;
};

// Shared trust boundary for every feature that relies on a persisted frame as
// a rigid transform rather than merely carrying it as descriptive metadata.
[[nodiscard]] bool trustworthy_rigid_placement(const RigidPlacementView& placement) noexcept;

// The only double-to-float display-coordinate seam. A requested local
// representation falls back for this mesh alone when its candidate frame is
// not a finite, orthonormal, right-handed rigid placement.
[[nodiscard]] ProjectedMeshPositions project_mesh_positions(
    std::span<const double> model_positions, MeshCoordinateMode requested_mode,
    const std::optional<RigidPlacementView>& candidate_placement);

}  // namespace tekla::db1::detail
