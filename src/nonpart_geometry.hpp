#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tekla/db1/process.hpp>
#include <tekla/db1/result.hpp>
#include <vector>

#include "schema.hpp"
#include "storage.hpp"

namespace tekla::db1::detail {

struct FastenerDimensions {
  double across_flats = 0.0;
  double height = 0.0;
};

// Returns the first vertex index for an indexed-mesh append when both position
// arrays are complete XYZ triples and the aggregate remains uint32-addressable.
[[nodiscard]] std::optional<std::uint32_t> checked_indexed_mesh_append_base(
    std::size_t existing_position_count, std::size_t appended_position_count) noexcept;

[[nodiscard]] std::optional<FastenerDimensions> known_fastener_dimensions(std::string_view standard,
                                                                          double diameter) noexcept;

[[nodiscard]] Result<std::vector<std::vector<Vector3d>>>
evaluate_tapered_straight_group_centerlines(
    std::array<Vector3d, 2> start_segment, std::array<Vector3d, 2> end_segment,
    Vector3d distribution_start, Vector3d distribution_end, double start_from_plane_offset,
    double end_from_plane_offset, std::uint32_t spacing_type, std::span<const double> spacings,
    std::uint32_t exclude_type, std::size_t maximum_count);

[[nodiscard]] Result<std::vector<std::vector<Vector3d>>>
evaluate_nonplanar_group_reference_centerlines(
    std::span<const Vector3d> polygon, Vector3d distribution_start, Vector3d distribution_end,
    double on_plane_offset, double start_from_plane_offset, double end_from_plane_offset,
    double start_point_offset, double end_point_offset, std::uint32_t spacing_type,
    std::span<const double> spacings, std::uint32_t exclude_type, bool align_distribution_anchor,
    std::size_t maximum_count);

[[nodiscard]] Result<ProcessStream> make_nonpart_geometry_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request);

}  // namespace tekla::db1::detail
