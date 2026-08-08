#include "mesh_placement.hpp"

#include <cmath>

namespace tekla::db1::detail {
namespace {

constexpr double rigid_tolerance = 1.0e-9;

[[nodiscard]] bool finite(Vector3d value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] double dot(Vector3d lhs, Vector3d rhs) noexcept {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] Vector3d cross(Vector3d lhs, Vector3d rhs) noexcept {
  return {lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z,
          lhs.x * rhs.y - lhs.y * rhs.x};
}

[[nodiscard]] bool unit(Vector3d axis) noexcept {
  return finite(axis) && std::abs(dot(axis, axis) - 1.0) <= rigid_tolerance;
}

}  // namespace

bool trustworthy_rigid_placement(const RigidPlacementView& placement) noexcept {
  if (!finite(placement.origin) || !unit(placement.x_axis) || !unit(placement.y_axis) ||
      !unit(placement.z_axis)) {
    return false;
  }
  if (std::abs(dot(placement.x_axis, placement.y_axis)) > rigid_tolerance ||
      std::abs(dot(placement.x_axis, placement.z_axis)) > rigid_tolerance ||
      std::abs(dot(placement.y_axis, placement.z_axis)) > rigid_tolerance) {
    return false;
  }
  return dot(cross(placement.x_axis, placement.y_axis), placement.z_axis) >= 1.0 - rigid_tolerance;
}

namespace {

[[nodiscard]] bool finite_positions(std::span<const double> positions) noexcept {
  if (positions.size() % 3U != 0U) return false;
  for (const auto coordinate : positions) {
    if (!std::isfinite(coordinate)) return false;
  }
  return true;
}

}  // namespace

ProjectedMeshPositions project_mesh_positions(
    std::span<const double> model_positions, MeshCoordinateMode requested_mode,
    const std::optional<RigidPlacementView>& candidate_placement) {
  ProjectedMeshPositions result;
  result.positions.reserve(model_positions.size());
  if (requested_mode == MeshCoordinateMode::local_with_rigid_placement && candidate_placement &&
      trustworthy_rigid_placement(*candidate_placement) && finite_positions(model_positions)) {
    result.coordinate_space = MeshCoordinateMode::local_with_rigid_placement;
    result.placement = *candidate_placement;
    for (std::size_t index = 0U; index < model_positions.size(); index += 3U) {
      const Vector3d relative{model_positions[index] - result.placement.origin.x,
                              model_positions[index + 1U] - result.placement.origin.y,
                              model_positions[index + 2U] - result.placement.origin.z};
      result.positions.push_back(static_cast<float>(dot(relative, result.placement.x_axis)));
      result.positions.push_back(static_cast<float>(dot(relative, result.placement.y_axis)));
      result.positions.push_back(static_cast<float>(dot(relative, result.placement.z_axis)));
    }
    return result;
  }
  for (const auto coordinate : model_positions) {
    result.positions.push_back(static_cast<float>(coordinate));
  }
  return result;
}

}  // namespace tekla::db1::detail
