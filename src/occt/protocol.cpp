#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace tekla::db1::detail {
namespace {

constexpr std::uint32_t kRequestMagic = 0x5254434fU;
constexpr std::uint32_t kResponseMagic = 0x5354434fU;
constexpr std::uint32_t kVersion = 6;
constexpr std::uint64_t kNodeHeaderSize = 112U;
constexpr std::uint32_t kMaximumNodeCount = 4'096U;
constexpr std::uint32_t kMaximumRecipeLoopCount = 4'096U;
constexpr std::uint32_t kMaximumSweepSectionCount = 262'144U;
constexpr std::uint32_t kMaximumRecipeCoordinateCount = 16U * 1024U * 1024U;

void u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void u64(std::vector<std::byte>& bytes, std::uint64_t value) {
  u32(bytes, static_cast<std::uint32_t>(value));
  u32(bytes, static_cast<std::uint32_t>(value >> 32U));
}

void f32(std::vector<std::byte>& bytes, float value) {
  u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void f64(std::vector<std::byte>& bytes, double value) {
  u64(bytes, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
         static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1])) << 8U |
         static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2])) << 16U |
         static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3])) << 24U;
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::uint64_t>(get_u32(bytes, offset)) |
         static_cast<std::uint64_t>(get_u32(bytes, offset + 4)) << 32U;
}

[[nodiscard]] double get_f64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return std::bit_cast<double>(get_u64(bytes, offset));
}

void box(std::vector<std::byte>& bytes, const OcctBox& value) {
  f64(bytes, value.x);
  f64(bytes, value.y);
  f64(bytes, value.z);
  f64(bytes, value.size_x);
  f64(bytes, value.size_y);
  f64(bytes, value.size_z);
}

[[nodiscard]] OcctBox get_box(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return {get_f64(bytes, offset),      get_f64(bytes, offset + 8),  get_f64(bytes, offset + 16),
          get_f64(bytes, offset + 24), get_f64(bytes, offset + 32), get_f64(bytes, offset + 40)};
}

void half_space(std::vector<std::byte>& bytes, const OcctHalfSpace& value) {
  f64(bytes, value.origin_x);
  f64(bytes, value.origin_y);
  f64(bytes, value.origin_z);
  f64(bytes, value.normal_x);
  f64(bytes, value.normal_y);
  f64(bytes, value.normal_z);
}

[[nodiscard]] OcctHalfSpace get_half_space(std::span<const std::byte> bytes,
                                           std::size_t offset) noexcept {
  return {get_f64(bytes, offset),      get_f64(bytes, offset + 8),  get_f64(bytes, offset + 16),
          get_f64(bytes, offset + 24), get_f64(bytes, offset + 32), get_f64(bytes, offset + 40)};
}

[[nodiscard]] bool mesh_counts_fit(const OcctTriangleMesh& mesh) noexcept {
  return mesh.positions.size() <= std::numeric_limits<std::uint32_t>::max() &&
         mesh.indices.size() <= std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] bool extrusion_counts_fit(const OcctExtrusion& extrusion) noexcept {
  std::uint64_t coordinate_count = 0U;
  return extrusion.loops.size() <= kMaximumRecipeLoopCount &&
         std::all_of(extrusion.loops.begin(), extrusion.loops.end(), [&](const OcctLoop& loop) {
           coordinate_count += loop.positions.size();
           return coordinate_count <= kMaximumRecipeCoordinateCount;
         });
}

[[nodiscard]] std::uint64_t extrusion_payload_size(const OcctExtrusion& extrusion) noexcept {
  std::uint64_t total = 0U;
  for (const auto& loop : extrusion.loops) {
    total += 4U + static_cast<std::uint64_t>(loop.positions.size()) * 8U;
  }
  return total;
}

[[nodiscard]] bool sweep_counts_fit(const OcctRuledSweep& sweep) noexcept {
  std::uint64_t section_count = 0U;
  std::uint64_t coordinate_count = 0U;
  return sweep.loops.size() <= kMaximumRecipeLoopCount &&
         std::all_of(sweep.loops.begin(), sweep.loops.end(),
                     [](const OcctSweepLoop& loop) {
                       return loop.sections.size() <= kMaximumSweepSectionCount &&
                              std::all_of(loop.sections.begin(), loop.sections.end(),
                                          [](const OcctLoop& section) {
                                            return section.positions.size() <=
                                                   kMaximumRecipeCoordinateCount;
                                          });
                     }) &&
         std::all_of(sweep.loops.begin(), sweep.loops.end(), [&](const OcctSweepLoop& loop) {
           section_count += loop.sections.size();
           for (const auto& section : loop.sections) coordinate_count += section.positions.size();
           return section_count <= kMaximumSweepSectionCount &&
                  coordinate_count <= kMaximumRecipeCoordinateCount;
         });
}

[[nodiscard]] std::uint64_t sweep_payload_size(const OcctRuledSweep& sweep) noexcept {
  std::uint64_t total = 0U;
  for (const auto& loop : sweep.loops) {
    total += 4U;
    for (const auto& section : loop.sections) {
      total += 4U + static_cast<std::uint64_t>(section.positions.size()) * 8U;
    }
  }
  return total;
}

[[nodiscard]] bool node_counts_fit(const OcctShapeNode& node, std::size_t node_count) noexcept {
  return node.subtract.size() <= std::numeric_limits<std::uint32_t>::max() &&
         node.keep_half_spaces.size() <= std::numeric_limits<std::uint32_t>::max() &&
         node.subtract_nodes.size() <= std::numeric_limits<std::uint32_t>::max() &&
         extrusion_counts_fit(node.base_extrusion) && sweep_counts_fit(node.base_ruled_sweep) &&
         mesh_counts_fit(node.base_mesh) &&
         std::all_of(node.subtract_nodes.begin(), node.subtract_nodes.end(),
                     [node_count](std::uint32_t index) { return index < node_count; });
}

[[nodiscard]] bool request_recipe_counts_fit(const OcctRequest& request) noexcept {
  std::uint64_t loops = 0U;
  std::uint64_t sections = 0U;
  std::uint64_t coordinates = 0U;
  for (const auto& node : request.nodes) {
    loops += node.base_extrusion.loops.size() + node.base_ruled_sweep.loops.size();
    for (const auto& loop : node.base_extrusion.loops) coordinates += loop.positions.size();
    for (const auto& loop : node.base_ruled_sweep.loops) {
      sections += loop.sections.size();
      for (const auto& section : loop.sections) coordinates += section.positions.size();
    }
    if (loops > kMaximumRecipeLoopCount || sections > kMaximumSweepSectionCount ||
        coordinates > kMaximumRecipeCoordinateCount) {
      return false;
    }
  }
  return true;
}

void mesh_payload(std::vector<std::byte>& bytes, const OcctTriangleMesh& mesh) {
  for (const auto value : mesh.positions) f64(bytes, value);
  for (const auto value : mesh.indices) u32(bytes, value);
}

void translate_positions(std::vector<double>& positions, const std::array<double, 3>& origin) {
  for (std::size_t index = 0U; index + 2U < positions.size(); index += 3U) {
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      positions[index + axis] =
          std::round((positions[index + axis] - origin[axis]) * 1.0e6) * 1.0e-6;
      if (positions[index + axis] == 0.0) positions[index + axis] = 0.0;
    }
  }
}

void translate_box(OcctBox& box, const std::array<double, 3>& origin) {
  box.x = std::round((box.x - origin[0]) * 1.0e6) * 1.0e-6;
  box.y = std::round((box.y - origin[1]) * 1.0e6) * 1.0e-6;
  box.z = std::round((box.z - origin[2]) * 1.0e6) * 1.0e-6;
  if (box.x == 0.0) box.x = 0.0;
  if (box.y == 0.0) box.y = 0.0;
  if (box.z == 0.0) box.z = 0.0;
}

void translate_half_spaces(std::vector<OcctHalfSpace>& planes,
                           const std::array<double, 3>& origin) {
  for (auto& plane : planes) {
    plane.origin_x = std::round((plane.origin_x - origin[0]) * 1.0e6) * 1.0e-6;
    plane.origin_y = std::round((plane.origin_y - origin[1]) * 1.0e6) * 1.0e-6;
    plane.origin_z = std::round((plane.origin_z - origin[2]) * 1.0e6) * 1.0e-6;
    if (plane.origin_x == 0.0) plane.origin_x = 0.0;
    if (plane.origin_y == 0.0) plane.origin_y = 0.0;
    if (plane.origin_z == 0.0) plane.origin_z = 0.0;
  }
}

void translate_extrusion(OcctExtrusion& extrusion, const std::array<double, 3>& origin) {
  for (auto& loop : extrusion.loops) translate_positions(loop.positions, origin);
}

void translate_sweep(OcctRuledSweep& sweep, const std::array<double, 3>& origin) {
  for (auto& loop : sweep.loops) {
    for (auto& section : loop.sections) translate_positions(section.positions, origin);
  }
}

[[nodiscard]] std::optional<std::array<double, 3>> request_origin(
    const OcctRequest& request) noexcept {
  const auto mesh_origin =
      [](const OcctTriangleMesh& mesh) -> std::optional<std::array<double, 3>> {
    if (mesh.positions.size() < 3U) return std::nullopt;
    return std::array<double, 3>{mesh.positions[0], mesh.positions[1], mesh.positions[2]};
  };
  const auto extrusion_origin =
      [](const OcctExtrusion& extrusion) -> std::optional<std::array<double, 3>> {
    for (const auto& loop : extrusion.loops) {
      if (loop.positions.size() >= 3U) {
        return std::array<double, 3>{loop.positions[0], loop.positions[1], loop.positions[2]};
      }
    }
    return std::nullopt;
  };
  const auto sweep_origin =
      [](const OcctRuledSweep& sweep) -> std::optional<std::array<double, 3>> {
    for (const auto& loop : sweep.loops) {
      for (const auto& section : loop.sections) {
        if (section.positions.size() >= 3U) {
          return std::array<double, 3>{section.positions[0], section.positions[1],
                                       section.positions[2]};
        }
      }
    }
    return std::nullopt;
  };
  if (!request.nodes.empty()) {
    const auto& root = request.nodes.front();
    if (auto origin = mesh_origin(root.base_mesh)) return origin;
    if (auto origin = extrusion_origin(root.base_extrusion)) return origin;
    if (auto origin = sweep_origin(root.base_ruled_sweep)) return origin;
    return std::array<double, 3>{root.base.x, root.base.y, root.base.z};
  }
  if (auto origin = mesh_origin(request.base_mesh)) return origin;
  return std::array<double, 3>{request.base.x, request.base.y, request.base.z};
}

void normalize_node_translation(OcctShapeNode& node, const std::array<double, 3>& origin) {
  node.object_id = 0U;
  const bool uses_mesh = !node.base_mesh.positions.empty() || !node.base_mesh.indices.empty();
  const bool uses_extrusion = !node.base_extrusion.loops.empty();
  const bool uses_sweep = !node.base_ruled_sweep.loops.empty();
  if (!uses_mesh && !uses_extrusion && !uses_sweep) translate_box(node.base, origin);
  translate_positions(node.base_mesh.positions, origin);
  translate_extrusion(node.base_extrusion, origin);
  translate_sweep(node.base_ruled_sweep, origin);
  for (auto& cutter : node.subtract) translate_box(cutter, origin);
  translate_half_spaces(node.keep_half_spaces, origin);
}

}  // namespace

Result<std::vector<std::byte>> encode_occt_request(const OcctRequest& request) {
  if (request.subtract.size() > std::numeric_limits<std::uint32_t>::max() ||
      request.keep_half_spaces.size() > std::numeric_limits<std::uint32_t>::max() ||
      request.subtract_meshes.size() > std::numeric_limits<std::uint32_t>::max() ||
      request.nodes.size() > kMaximumNodeCount || !mesh_counts_fit(request.base_mesh) ||
      !std::all_of(request.subtract_meshes.begin(), request.subtract_meshes.end(),
                   mesh_counts_fit) ||
      !std::all_of(
          request.nodes.begin(), request.nodes.end(),
          [&](const OcctShapeNode& node) { return node_counts_fit(node, request.nodes.size()); }) ||
      !request_recipe_counts_fit(request)) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::resource_limit, "The OCCT worker request is too large."});
  }
  std::uint64_t total = kOcctRequestHeaderSize +
                        static_cast<std::uint64_t>(request.subtract.size()) * 48U +
                        static_cast<std::uint64_t>(request.keep_half_spaces.size()) * 48U +
                        static_cast<std::uint64_t>(request.base_mesh.positions.size()) * 8U +
                        static_cast<std::uint64_t>(request.base_mesh.indices.size()) * 4U;
  for (const auto& mesh : request.subtract_meshes) {
    total += 8U + static_cast<std::uint64_t>(mesh.positions.size()) * 8U +
             static_cast<std::uint64_t>(mesh.indices.size()) * 4U;
  }
  for (const auto& node : request.nodes) {
    total += kNodeHeaderSize + static_cast<std::uint64_t>(node.subtract.size()) * 48U +
             static_cast<std::uint64_t>(node.keep_half_spaces.size()) * 48U +
             static_cast<std::uint64_t>(node.base_mesh.positions.size()) * 8U +
             static_cast<std::uint64_t>(node.base_mesh.indices.size()) * 4U +
             static_cast<std::uint64_t>(node.subtract_nodes.size()) * 4U +
             extrusion_payload_size(node.base_extrusion) +
             sweep_payload_size(node.base_ruled_sweep);
  }
  if (total > kOcctMaxMessageBytes || total > std::numeric_limits<std::size_t>::max()) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::resource_limit, "The OCCT worker request is too large."});
  }
  std::vector<std::byte> bytes;
  bytes.reserve(static_cast<std::size_t>(total));
  u32(bytes, kRequestMagic);
  u32(bytes, kVersion);
  u64(bytes, request.object_id);
  box(bytes, request.base);
  f64(bytes, request.linear_deflection);
  f64(bytes, request.angular_deflection);
  u32(bytes, static_cast<std::uint32_t>(request.subtract.size()));
  u32(bytes, static_cast<std::uint32_t>(request.keep_half_spaces.size()));
  u32(bytes, static_cast<std::uint32_t>(request.base_mesh.positions.size()));
  u32(bytes, static_cast<std::uint32_t>(request.base_mesh.indices.size()));
  u32(bytes, static_cast<std::uint32_t>(request.subtract_meshes.size()));
  u32(bytes, static_cast<std::uint32_t>(request.nodes.size()));
  u64(bytes, total);
  for (const auto& cutter : request.subtract) box(bytes, cutter);
  for (const auto& plane : request.keep_half_spaces) half_space(bytes, plane);
  mesh_payload(bytes, request.base_mesh);
  for (const auto& mesh : request.subtract_meshes) {
    u32(bytes, static_cast<std::uint32_t>(mesh.positions.size()));
    u32(bytes, static_cast<std::uint32_t>(mesh.indices.size()));
    mesh_payload(bytes, mesh);
  }
  for (const auto& node : request.nodes) {
    u64(bytes, node.object_id);
    box(bytes, node.base);
    u32(bytes, static_cast<std::uint32_t>(node.subtract.size()));
    u32(bytes, static_cast<std::uint32_t>(node.keep_half_spaces.size()));
    u32(bytes, static_cast<std::uint32_t>(node.base_mesh.positions.size()));
    u32(bytes, static_cast<std::uint32_t>(node.base_mesh.indices.size()));
    u32(bytes, static_cast<std::uint32_t>(node.subtract_nodes.size()));
    u32(bytes, static_cast<std::uint32_t>(node.base_extrusion.loops.size()));
    f64(bytes, node.base_extrusion.vector_x);
    f64(bytes, node.base_extrusion.vector_y);
    f64(bytes, node.base_extrusion.vector_z);
    u32(bytes, static_cast<std::uint32_t>(node.base_ruled_sweep.loops.size()));
    u32(bytes, node.base_ruled_sweep.circular_spine ? 1U : 0U);
    for (const auto& cutter : node.subtract) box(bytes, cutter);
    for (const auto& plane : node.keep_half_spaces) half_space(bytes, plane);
    mesh_payload(bytes, node.base_mesh);
    for (const auto& loop : node.base_extrusion.loops) {
      u32(bytes, static_cast<std::uint32_t>(loop.positions.size()));
      for (const auto value : loop.positions) f64(bytes, value);
    }
    for (const auto& loop : node.base_ruled_sweep.loops) {
      u32(bytes, static_cast<std::uint32_t>(loop.sections.size()));
      for (const auto& section : loop.sections) {
        u32(bytes, static_cast<std::uint32_t>(section.positions.size()));
        for (const auto value : section.positions) f64(bytes, value);
      }
    }
    for (const auto child : node.subtract_nodes) u32(bytes, child);
  }
  if (std::getenv("TEKLA_DB1_OCCT_PROFILE") != nullptr) {
    std::fprintf(stderr,
                 "{\"occt_protocol\":\"request\",\"object_id\":%llu,"
                 "\"bytes\":%zu}\n",
                 static_cast<unsigned long long>(request.object_id), bytes.size());
  }
  return Result<std::vector<std::byte>>::success(std::move(bytes));
}

Result<TranslationNormalizedOcctRequest> encode_translation_normalized_occt_request(
    const OcctRequest& request) {
  const auto origin = request_origin(request);
  if (!origin || !std::all_of(origin->begin(), origin->end(),
                              [](double value) { return std::isfinite(value); })) {
    return Result<TranslationNormalizedOcctRequest>::failure(
        {ErrorCode::invalid_argument, "The OCCT request has no finite translation origin."});
  }
  OcctRequest normalized = request;
  normalized.object_id = 0U;
  if (!normalized.nodes.empty()) {
    for (auto& node : normalized.nodes) normalize_node_translation(node, *origin);
  } else {
    const bool uses_mesh =
        !normalized.base_mesh.positions.empty() || !normalized.base_mesh.indices.empty();
    if (!uses_mesh) translate_box(normalized.base, *origin);
    translate_positions(normalized.base_mesh.positions, *origin);
    for (auto& cutter : normalized.subtract) translate_box(cutter, *origin);
    for (auto& cutter : normalized.subtract_meshes) translate_positions(cutter.positions, *origin);
    translate_half_spaces(normalized.keep_half_spaces, *origin);
  }
  auto encoded = encode_occt_request(normalized);
  if (!encoded) {
    return Result<TranslationNormalizedOcctRequest>::failure(std::move(encoded.error()));
  }
  return Result<TranslationNormalizedOcctRequest>::success({std::move(encoded.value()), *origin});
}

Result<std::size_t> occt_request_size(std::span<const std::byte> header) {
  if (header.size() < kOcctRequestHeaderSize || get_u32(header, 0) != kRequestMagic ||
      get_u32(header, 4) != kVersion) {
    return Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "The OCCT worker request header is invalid."});
  }
  const auto encoded_size = get_u64(header, 104);
  if (encoded_size < kOcctRequestHeaderSize || encoded_size > kOcctMaxMessageBytes ||
      encoded_size > std::numeric_limits<std::size_t>::max())
    return Result<std::size_t>::failure(
        {ErrorCode::resource_limit, "The OCCT worker request is too large."});
  return Result<std::size_t>::success(static_cast<std::size_t>(encoded_size));
}

Result<OcctRequest> decode_occt_request(std::span<const std::byte> bytes) {
  auto size = occt_request_size(bytes);
  if (!size) return Result<OcctRequest>::failure(size.error());
  if (bytes.size() != size.value())
    return Result<OcctRequest>::failure(
        {ErrorCode::invalid_argument, "The OCCT worker request size is inconsistent."});
  OcctRequest request;
  request.object_id = get_u64(bytes, 8);
  request.base = get_box(bytes, 16);
  request.linear_deflection = get_f64(bytes, 64);
  request.angular_deflection = get_f64(bytes, 72);
  const auto box_count = get_u32(bytes, 80);
  const auto plane_count = get_u32(bytes, 84);
  const auto base_position_count = get_u32(bytes, 88);
  const auto base_index_count = get_u32(bytes, 92);
  const auto mesh_count = get_u32(bytes, 96);
  const auto node_count = get_u32(bytes, 100);
  if (node_count > kMaximumNodeCount) {
    return Result<OcctRequest>::failure(
        {ErrorCode::resource_limit, "The OCCT worker CSG-node count exceeds its limit."});
  }
  std::uint64_t decoded_recipe_loops = 0U;
  std::uint64_t decoded_sweep_sections = 0U;
  std::uint64_t decoded_recipe_coordinates = 0U;
  std::size_t offset = kOcctRequestHeaderSize;
  const auto available = [&](std::uint64_t count) { return count <= bytes.size() - offset; };
  if (!available(static_cast<std::uint64_t>(box_count) * 48U))
    return Result<OcctRequest>::failure(
        {ErrorCode::invalid_argument, "The OCCT worker box payload is truncated."});
  request.subtract.reserve(box_count);
  for (std::uint32_t index = 0; index < box_count; ++index) {
    if (!available(48U))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker box payload is truncated."});
    request.subtract.push_back(get_box(bytes, offset));
    offset += 48U;
  }
  if (!available(static_cast<std::uint64_t>(plane_count) * 48U))
    return Result<OcctRequest>::failure(
        {ErrorCode::invalid_argument, "The OCCT worker plane payload is truncated."});
  request.keep_half_spaces.reserve(plane_count);
  for (std::uint32_t index = 0; index < plane_count; ++index) {
    if (!available(48U))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker plane payload is truncated."});
    request.keep_half_spaces.push_back(get_half_space(bytes, offset));
    offset += 48U;
  }
  const auto read_mesh = [&](std::uint32_t position_count, std::uint32_t index_count,
                             OcctTriangleMesh& mesh) -> bool {
    const std::uint64_t payload = static_cast<std::uint64_t>(position_count) * 8U +
                                  static_cast<std::uint64_t>(index_count) * 4U;
    if (!available(payload)) return false;
    mesh.positions.reserve(position_count);
    for (std::uint32_t index = 0; index < position_count; ++index, offset += 8U) {
      mesh.positions.push_back(get_f64(bytes, offset));
    }
    mesh.indices.reserve(index_count);
    for (std::uint32_t index = 0; index < index_count; ++index, offset += 4U) {
      mesh.indices.push_back(get_u32(bytes, offset));
    }
    return true;
  };
  if (!read_mesh(base_position_count, base_index_count, request.base_mesh)) {
    return Result<OcctRequest>::failure(
        {ErrorCode::invalid_argument, "The OCCT worker base-mesh payload is truncated."});
  }
  if (!available(static_cast<std::uint64_t>(mesh_count) * 8U))
    return Result<OcctRequest>::failure(
        {ErrorCode::invalid_argument, "The OCCT worker mesh headers are truncated."});
  request.subtract_meshes.reserve(mesh_count);
  for (std::uint32_t index = 0; index < mesh_count; ++index) {
    if (!available(8U))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker mesh header is truncated."});
    const auto position_count = get_u32(bytes, offset);
    const auto index_count = get_u32(bytes, offset + 4U);
    offset += 8U;
    request.subtract_meshes.emplace_back();
    if (!read_mesh(position_count, index_count, request.subtract_meshes.back())) {
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker cutter-mesh payload is truncated."});
    }
  }
  if (!available(static_cast<std::uint64_t>(node_count) * kNodeHeaderSize))
    return Result<OcctRequest>::failure(
        {ErrorCode::invalid_argument, "The OCCT worker CSG-node headers are truncated."});
  request.nodes.reserve(node_count);
  for (std::uint32_t index = 0; index < node_count; ++index) {
    if (!available(kNodeHeaderSize))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker CSG-node header is truncated."});
    OcctShapeNode node;
    node.object_id = get_u64(bytes, offset);
    node.base = get_box(bytes, offset + 8U);
    const auto node_box_count = get_u32(bytes, offset + 56U);
    const auto node_plane_count = get_u32(bytes, offset + 60U);
    const auto position_count = get_u32(bytes, offset + 64U);
    const auto index_count = get_u32(bytes, offset + 68U);
    const auto child_count = get_u32(bytes, offset + 72U);
    const auto loop_count = get_u32(bytes, offset + 76U);
    node.base_extrusion.vector_x = get_f64(bytes, offset + 80U);
    node.base_extrusion.vector_y = get_f64(bytes, offset + 88U);
    node.base_extrusion.vector_z = get_f64(bytes, offset + 96U);
    const auto sweep_loop_count = get_u32(bytes, offset + 104U);
    const auto sweep_flags = get_u32(bytes, offset + 108U);
    if ((sweep_flags & ~1U) != 0U)
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker sweep flags are invalid."});
    node.base_ruled_sweep.circular_spine = (sweep_flags & 1U) != 0U;
    offset += kNodeHeaderSize;
    if (decoded_recipe_loops + loop_count + sweep_loop_count > kMaximumRecipeLoopCount) {
      return Result<OcctRequest>::failure(
          {ErrorCode::resource_limit, "The OCCT worker recipe-loop count exceeds its limit."});
    }
    decoded_recipe_loops += loop_count + sweep_loop_count;
    if (!available(static_cast<std::uint64_t>(node_box_count) * 48U))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker CSG box payload is truncated."});
    node.subtract.reserve(node_box_count);
    for (std::uint32_t cutter = 0; cutter < node_box_count; ++cutter) {
      if (!available(48U))
        return Result<OcctRequest>::failure(
            {ErrorCode::invalid_argument, "The OCCT worker CSG box payload is truncated."});
      node.subtract.push_back(get_box(bytes, offset));
      offset += 48U;
    }
    if (!available(static_cast<std::uint64_t>(node_plane_count) * 48U))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker CSG plane payload is truncated."});
    node.keep_half_spaces.reserve(node_plane_count);
    for (std::uint32_t plane = 0; plane < node_plane_count; ++plane) {
      if (!available(48U))
        return Result<OcctRequest>::failure(
            {ErrorCode::invalid_argument, "The OCCT worker CSG plane payload is truncated."});
      node.keep_half_spaces.push_back(get_half_space(bytes, offset));
      offset += 48U;
    }
    if (!read_mesh(position_count, index_count, node.base_mesh)) {
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker CSG mesh payload is truncated."});
    }
    if (!available(static_cast<std::uint64_t>(loop_count) * 4U))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker extrusion-loop headers are truncated."});
    node.base_extrusion.loops.reserve(loop_count);
    for (std::uint32_t loop = 0; loop < loop_count; ++loop) {
      if (!available(4U))
        return Result<OcctRequest>::failure(
            {ErrorCode::invalid_argument, "The OCCT worker extrusion-loop header is truncated."});
      const auto coordinate_count = get_u32(bytes, offset);
      offset += 4U;
      if (decoded_recipe_coordinates + coordinate_count > kMaximumRecipeCoordinateCount) {
        return Result<OcctRequest>::failure(
            {ErrorCode::resource_limit,
             "The OCCT worker recipe-coordinate count exceeds its limit."});
      }
      decoded_recipe_coordinates += coordinate_count;
      const auto payload = static_cast<std::uint64_t>(coordinate_count) * 8U;
      if (!available(payload))
        return Result<OcctRequest>::failure(
            {ErrorCode::invalid_argument, "The OCCT worker extrusion-loop payload is truncated."});
      auto& positions = node.base_extrusion.loops.emplace_back().positions;
      positions.reserve(coordinate_count);
      for (std::uint32_t coordinate = 0; coordinate < coordinate_count;
           ++coordinate, offset += 8U) {
        positions.push_back(get_f64(bytes, offset));
      }
    }
    if (!available(static_cast<std::uint64_t>(sweep_loop_count) * 4U))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker sweep-loop headers are truncated."});
    node.base_ruled_sweep.loops.reserve(sweep_loop_count);
    for (std::uint32_t loop = 0; loop < sweep_loop_count; ++loop) {
      if (!available(4U))
        return Result<OcctRequest>::failure(
            {ErrorCode::invalid_argument, "The OCCT worker sweep-loop header is truncated."});
      const auto section_count = get_u32(bytes, offset);
      offset += 4U;
      if (decoded_sweep_sections + section_count > kMaximumSweepSectionCount) {
        return Result<OcctRequest>::failure(
            {ErrorCode::resource_limit, "The OCCT worker sweep-section count exceeds its limit."});
      }
      decoded_sweep_sections += section_count;
      if (!available(static_cast<std::uint64_t>(section_count) * 4U))
        return Result<OcctRequest>::failure(
            {ErrorCode::invalid_argument, "The OCCT worker sweep-section headers are truncated."});
      auto& sections = node.base_ruled_sweep.loops.emplace_back().sections;
      sections.reserve(section_count);
      for (std::uint32_t section = 0; section < section_count; ++section) {
        if (!available(4U))
          return Result<OcctRequest>::failure(
              {ErrorCode::invalid_argument, "The OCCT worker sweep-section header is truncated."});
        const auto coordinate_count = get_u32(bytes, offset);
        offset += 4U;
        if (decoded_recipe_coordinates + coordinate_count > kMaximumRecipeCoordinateCount) {
          return Result<OcctRequest>::failure(
              {ErrorCode::resource_limit,
               "The OCCT worker recipe-coordinate count exceeds its limit."});
        }
        decoded_recipe_coordinates += coordinate_count;
        const auto payload = static_cast<std::uint64_t>(coordinate_count) * 8U;
        if (!available(payload))
          return Result<OcctRequest>::failure(
              {ErrorCode::invalid_argument, "The OCCT worker sweep-section payload is truncated."});
        auto& positions = sections.emplace_back().positions;
        positions.reserve(coordinate_count);
        for (std::uint32_t coordinate = 0; coordinate < coordinate_count;
             ++coordinate, offset += 8U) {
          positions.push_back(get_f64(bytes, offset));
        }
      }
    }
    if (!available(static_cast<std::uint64_t>(child_count) * 4U))
      return Result<OcctRequest>::failure(
          {ErrorCode::invalid_argument, "The OCCT worker CSG edges are truncated."});
    node.subtract_nodes.reserve(child_count);
    for (std::uint32_t child = 0; child < child_count; ++child) {
      if (!available(4U))
        return Result<OcctRequest>::failure(
            {ErrorCode::invalid_argument, "The OCCT worker CSG edge payload is truncated."});
      const auto child_index = get_u32(bytes, offset);
      offset += 4U;
      if (child_index >= node_count)
        return Result<OcctRequest>::failure(
            {ErrorCode::invalid_argument, "The OCCT worker CSG edge target is invalid."});
      node.subtract_nodes.push_back(child_index);
    }
    request.nodes.push_back(std::move(node));
  }
  if (offset != bytes.size())
    return Result<OcctRequest>::failure(
        {ErrorCode::invalid_argument, "The OCCT worker request has trailing bytes."});
  return Result<OcctRequest>::success(std::move(request));
}

Result<std::vector<std::byte>> encode_occt_response(const Result<OcctMesh>& response) {
  const bool success = response.has_value();
  const auto position_count = success ? response.value().positions.size() : 0U;
  const auto index_count = success ? response.value().indices.size() : 0U;
  const std::string message = success ? std::string{} : response.error().message;
  if (position_count > std::numeric_limits<std::uint32_t>::max() ||
      index_count > std::numeric_limits<std::uint32_t>::max() ||
      message.size() > std::numeric_limits<std::uint32_t>::max() ||
      position_count > (kOcctMaxMessageBytes - kOcctResponseHeaderSize) / sizeof(float) ||
      index_count >
          (kOcctMaxMessageBytes - kOcctResponseHeaderSize - position_count * sizeof(float)) /
              sizeof(std::uint32_t) ||
      message.size() > kOcctMaxMessageBytes - kOcctResponseHeaderSize -
                           position_count * sizeof(float) - index_count * sizeof(std::uint32_t)) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::resource_limit, "The OCCT worker response is too large."});
  }
  std::vector<std::byte> bytes;
  bytes.reserve(kOcctResponseHeaderSize + position_count * sizeof(float) +
                index_count * sizeof(std::uint32_t) + message.size());
  u32(bytes, kResponseMagic);
  u32(bytes, kVersion);
  u32(bytes, success ? 0U : static_cast<std::uint32_t>(response.error().code));
  u32(bytes, success && response.value().has_exact_metrics ? 1U : 0U);
  u64(bytes, success ? response.value().object_id : 0U);
  u32(bytes, static_cast<std::uint32_t>(position_count));
  u32(bytes, static_cast<std::uint32_t>(index_count));
  u32(bytes, static_cast<std::uint32_t>(message.size()));
  u32(bytes, 0);
  f64(bytes, success ? response.value().exact_surface_area : 0.0);
  f64(bytes, success ? response.value().exact_volume : 0.0);
  if (success) {
    for (const auto value : response.value().positions) f32(bytes, value);
    for (const auto value : response.value().indices) u32(bytes, value);
  } else {
    const auto* first = reinterpret_cast<const std::byte*>(message.data());
    bytes.insert(bytes.end(), first, first + message.size());
  }
  if (std::getenv("TEKLA_DB1_OCCT_PROFILE") != nullptr) {
    std::fprintf(stderr,
                 "{\"occt_protocol\":\"response\",\"object_id\":%llu,"
                 "\"bytes\":%zu}\n",
                 static_cast<unsigned long long>(success ? response.value().object_id : 0U),
                 bytes.size());
  }
  return Result<std::vector<std::byte>>::success(std::move(bytes));
}

Result<std::size_t> occt_response_size(std::span<const std::byte> header) {
  if (header.size() < kOcctResponseHeaderSize || get_u32(header, 0) != kResponseMagic ||
      get_u32(header, 4) != kVersion)
    return Result<std::size_t>::failure(
        {ErrorCode::geometry_backend_crash, "The OCCT worker response header is invalid."});
  const std::uint64_t payload = static_cast<std::uint64_t>(get_u32(header, 24)) * 4U +
                                static_cast<std::uint64_t>(get_u32(header, 28)) * 4U +
                                get_u32(header, 32);
  if (payload > std::numeric_limits<std::size_t>::max() - kOcctResponseHeaderSize) {
    return Result<std::size_t>::failure(
        {ErrorCode::resource_limit, "The OCCT worker response is too large."});
  }
  const auto size = kOcctResponseHeaderSize + static_cast<std::size_t>(payload);
  if (size > kOcctMaxMessageBytes)
    return Result<std::size_t>::failure(
        {ErrorCode::resource_limit, "The OCCT worker response is too large."});
  return Result<std::size_t>::success(size);
}

Result<OcctMesh> decode_occt_response(std::span<const std::byte> bytes) {
  auto size = occt_response_size(bytes);
  if (!size) return Result<OcctMesh>::failure(size.error());
  if (bytes.size() != size.value())
    return Result<OcctMesh>::failure(
        {ErrorCode::geometry_backend_crash, "The OCCT worker response size is inconsistent."});
  const auto status = get_u32(bytes, 8);
  const auto flags = get_u32(bytes, 12);
  const auto position_count = get_u32(bytes, 24);
  const auto index_count = get_u32(bytes, 28);
  const auto message_count = get_u32(bytes, 32);
  if (status > static_cast<std::uint32_t>(ErrorCode::internal_error) || flags > 1U ||
      (status == 0 && message_count != 0) ||
      (status != 0 && (position_count != 0 || index_count != 0))) {
    return Result<OcctMesh>::failure(
        {ErrorCode::geometry_backend_crash, "The OCCT worker response fields are invalid."});
  }
  if (status != 0) {
    const auto message_offset = kOcctResponseHeaderSize;
    return Result<OcctMesh>::failure(
        {static_cast<ErrorCode>(status),
         std::string(reinterpret_cast<const char*>(bytes.data() + message_offset), message_count)});
  }
  OcctMesh mesh;
  mesh.object_id = get_u64(bytes, 16);
  mesh.has_exact_metrics = (flags & 1U) != 0U;
  mesh.exact_surface_area = get_f64(bytes, 40);
  mesh.exact_volume = get_f64(bytes, 48);
  if (mesh.has_exact_metrics &&
      (!std::isfinite(mesh.exact_surface_area) || mesh.exact_surface_area <= 0.0 ||
       !std::isfinite(mesh.exact_volume) || mesh.exact_volume <= 0.0)) {
    return Result<OcctMesh>::failure(
        {ErrorCode::geometry_backend_crash, "The OCCT worker returned invalid exact metrics."});
  }
  mesh.positions.reserve(position_count);
  std::size_t offset = kOcctResponseHeaderSize;
  for (std::uint32_t index = 0; index < position_count; ++index, offset += 4) {
    mesh.positions.push_back(std::bit_cast<float>(get_u32(bytes, offset)));
  }
  mesh.indices.reserve(index_count);
  for (std::uint32_t index = 0; index < index_count; ++index, offset += 4) {
    mesh.indices.push_back(get_u32(bytes, offset));
  }
  return Result<OcctMesh>::success(std::move(mesh));
}

}  // namespace tekla::db1::detail
