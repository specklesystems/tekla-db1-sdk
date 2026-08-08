#include "geometry_evaluation_cache.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "occt/protocol.hpp"

namespace tekla::db1::detail {
namespace {

constexpr std::size_t kMaximumEntries = 4096U;
constexpr std::size_t kMaximumBytes = 256U * 1024U * 1024U;
constexpr double kPlacementTolerance = 1.0e-6;

enum class CanonicalizationKind : std::uint8_t { translation, rigid };

struct CacheKey {
  std::vector<std::byte> bytes;
  CanonicalizationKind kind = CanonicalizationKind::translation;
  bool operator==(const CacheKey&) const = default;
};

struct CacheKeyHash {
  std::size_t operator()(const CacheKey& key) const noexcept {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    value ^= static_cast<std::uint8_t>(key.kind);
    value *= 0x100000001b3ULL;
    for (const auto byte : key.bytes) {
      value ^= std::to_integer<std::uint8_t>(byte);
      value *= 0x100000001b3ULL;
    }
    return static_cast<std::size_t>(value ^ (value >> 32U));
  }
};

struct CacheEntry {
  GeometryEvaluationPlacement placement;
  OcctMesh mesh;
};

// Retains the exact accounting used by the extracted translation-only cache.
struct TranslationAccountingEntry {
  std::array<double, 3> origin{};
  OcctMesh mesh;
};

[[nodiscard]] double dot(std::array<double, 3> left, std::array<double, 3> right) noexcept {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] std::array<double, 3> cross(std::array<double, 3> left,
                                          std::array<double, 3> right) noexcept {
  return {left[1] * right[2] - left[2] * right[1], left[2] * right[0] - left[0] * right[2],
          left[0] * right[1] - left[1] * right[0]};
}

[[nodiscard]] bool finite(std::array<double, 3> value) noexcept {
  return std::all_of(value.begin(), value.end(), [](double item) { return std::isfinite(item); });
}

[[nodiscard]] bool valid(const GeometryEvaluationPlacement& placement) noexcept {
  if (!finite(placement.origin) || !finite(placement.x_axis) || !finite(placement.y_axis) ||
      !finite(placement.z_axis)) {
    return false;
  }
  const auto unit = [](std::array<double, 3> axis) {
    return std::abs(dot(axis, axis) - 1.0) <= kPlacementTolerance;
  };
  return unit(placement.x_axis) && unit(placement.y_axis) && unit(placement.z_axis) &&
         std::abs(dot(placement.x_axis, placement.y_axis)) <= kPlacementTolerance &&
         std::abs(dot(placement.x_axis, placement.z_axis)) <= kPlacementTolerance &&
         std::abs(dot(placement.y_axis, placement.z_axis)) <= kPlacementTolerance &&
         dot(cross(placement.x_axis, placement.y_axis), placement.z_axis) >=
             1.0 - kPlacementTolerance;
}

[[nodiscard]] double canonical(double value) noexcept {
  const double rounded = std::round(value * 1.0e6) * 1.0e-6;
  return rounded == 0.0 ? 0.0 : rounded;
}

[[nodiscard]] std::array<double, 3> local_point_unrounded(
    std::array<double, 3> world, const GeometryEvaluationPlacement& placement) noexcept {
  const std::array<double, 3> relative{world[0] - placement.origin[0],
                                       world[1] - placement.origin[1],
                                       world[2] - placement.origin[2]};
  return {dot(relative, placement.x_axis), dot(relative, placement.y_axis),
          dot(relative, placement.z_axis)};
}

[[nodiscard]] std::array<double, 3> local_point(
    std::array<double, 3> world, const GeometryEvaluationPlacement& placement) noexcept {
  auto local = local_point_unrounded(world, placement);
  for (auto& coordinate : local) coordinate = canonical(coordinate);
  return local;
}

[[nodiscard]] std::array<double, 3> local_vector(
    std::array<double, 3> world, const GeometryEvaluationPlacement& placement) noexcept {
  return {canonical(dot(world, placement.x_axis)), canonical(dot(world, placement.y_axis)),
          canonical(dot(world, placement.z_axis))};
}

[[nodiscard]] std::array<double, 3> world_point(
    std::array<double, 3> local, const GeometryEvaluationPlacement& placement) noexcept {
  std::array<double, 3> world = placement.origin;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    world[axis] += placement.x_axis[axis] * local[0] + placement.y_axis[axis] * local[1] +
                   placement.z_axis[axis] * local[2];
  }
  return world;
}

bool localize_positions(std::vector<double>& positions,
                        const GeometryEvaluationPlacement& placement) {
  if (positions.size() % 3U != 0U) return false;
  for (std::size_t index = 0U; index < positions.size(); index += 3U) {
    const auto local =
        local_point({positions[index], positions[index + 1U], positions[index + 2U]}, placement);
    positions[index] = local[0];
    positions[index + 1U] = local[1];
    positions[index + 2U] = local[2];
  }
  return true;
}

void localize_vector(double& x, double& y, double& z,
                     const GeometryEvaluationPlacement& placement) {
  const auto local = local_vector({x, y, z}, placement);
  x = local[0];
  y = local[1];
  z = local[2];
}

bool localize_extrusion(OcctExtrusion& extrusion, const GeometryEvaluationPlacement& placement) {
  for (auto& loop : extrusion.loops) {
    if (!localize_positions(loop.positions, placement)) return false;
  }
  localize_vector(extrusion.vector_x, extrusion.vector_y, extrusion.vector_z, placement);
  return true;
}

bool localize_sweep(OcctRuledSweep& sweep, const GeometryEvaluationPlacement& placement) {
  for (auto& loop : sweep.loops) {
    for (auto& section : loop.sections) {
      if (!localize_positions(section.positions, placement)) return false;
    }
  }
  return true;
}

void localize_half_spaces(std::vector<OcctHalfSpace>& planes,
                          const GeometryEvaluationPlacement& placement) {
  for (auto& plane : planes) {
    const auto origin = local_point({plane.origin_x, plane.origin_y, plane.origin_z}, placement);
    const auto normal = local_vector({plane.normal_x, plane.normal_y, plane.normal_z}, placement);
    plane.origin_x = origin[0];
    plane.origin_y = origin[1];
    plane.origin_z = origin[2];
    plane.normal_x = normal[0];
    plane.normal_y = normal[1];
    plane.normal_z = normal[2];
  }
}

[[nodiscard]] bool box_has_volume(const OcctBox& box) noexcept {
  return box.size_x != 0.0 || box.size_y != 0.0 || box.size_z != 0.0;
}

[[nodiscard]] bool axis_aligned(const GeometryEvaluationPlacement& placement) noexcept {
  const auto aligned = [](std::array<double, 3> axis) {
    return std::count_if(axis.begin(), axis.end(),
                         [](double value) {
                           return std::abs(std::abs(value) - 1.0) <= kPlacementTolerance;
                         }) == 1 &&
           std::count_if(axis.begin(), axis.end(),
                         [](double value) { return std::abs(value) <= kPlacementTolerance; }) == 2;
  };
  return aligned(placement.x_axis) && aligned(placement.y_axis) && aligned(placement.z_axis);
}

bool localize_box(OcctBox& box, const GeometryEvaluationPlacement& placement) {
  if (!box_has_volume(box)) return true;
  if (!axis_aligned(placement)) return false;
  std::array<double, 3> minimum{std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::infinity()};
  std::array<double, 3> maximum{-std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity()};
  for (const double dx : {0.0, box.size_x}) {
    for (const double dy : {0.0, box.size_y}) {
      for (const double dz : {0.0, box.size_z}) {
        const auto local = local_point({box.x + dx, box.y + dy, box.z + dz}, placement);
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          minimum[axis] = std::min(minimum[axis], local[axis]);
          maximum[axis] = std::max(maximum[axis], local[axis]);
        }
      }
    }
  }
  box = {.x = minimum[0],
         .y = minimum[1],
         .z = minimum[2],
         .size_x = canonical(maximum[0] - minimum[0]),
         .size_y = canonical(maximum[1] - minimum[1]),
         .size_z = canonical(maximum[2] - minimum[2])};
  return true;
}

bool localize_node(OcctShapeNode& node, const GeometryEvaluationPlacement& placement) {
  node.object_id = 0U;
  if (!localize_box(node.base, placement) ||
      !localize_positions(node.base_mesh.positions, placement) ||
      !localize_extrusion(node.base_extrusion, placement) ||
      !localize_sweep(node.base_ruled_sweep, placement)) {
    return false;
  }
  for (auto& box : node.subtract) {
    if (!localize_box(box, placement)) return false;
  }
  localize_half_spaces(node.keep_half_spaces, placement);
  return true;
}

[[nodiscard]] std::optional<CacheKey> rigid_key(const OcctRequest& request,
                                                const GeometryEvaluationPlacement& placement) {
  if (!valid(placement)) return std::nullopt;
  OcctRequest normalized = request;
  normalized.object_id = 0U;
  if (!localize_box(normalized.base, placement) ||
      !localize_positions(normalized.base_mesh.positions, placement)) {
    return std::nullopt;
  }
  for (auto& box : normalized.subtract) {
    if (!localize_box(box, placement)) return std::nullopt;
  }
  for (auto& mesh : normalized.subtract_meshes) {
    if (!localize_positions(mesh.positions, placement)) return std::nullopt;
  }
  localize_half_spaces(normalized.keep_half_spaces, placement);
  for (auto& node : normalized.nodes) {
    if (!localize_node(node, placement)) return std::nullopt;
  }
  auto bytes = encode_occt_request(normalized);
  if (!bytes) return std::nullopt;
  return CacheKey{std::move(bytes.value()), CanonicalizationKind::rigid};
}

[[nodiscard]] GeometryEvaluationPlacement translation_placement(
    std::array<double, 3> origin) noexcept {
  GeometryEvaluationPlacement placement;
  placement.origin = origin;
  return placement;
}

}  // namespace

struct GeometryEvaluationCache::Impl {
  [[nodiscard]] std::optional<OcctMesh> find(const CacheKey& key,
                                             const GeometryEvaluationPlacement& placement,
                                             std::uint64_t object_id) const {
    if (std::getenv("TEKLA_DB1_DISABLE_OCCT_CACHE") != nullptr) return std::nullopt;
    const auto found = entries.find(key);
    if (found == entries.end()) return std::nullopt;
    OcctMesh mesh = found->second.mesh;
    mesh.object_id = object_id;
    for (std::size_t index = 0U; index + 2U < mesh.positions.size(); index += 3U) {
      if (key.kind == CanonicalizationKind::translation) {
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          const double translated = static_cast<double>(mesh.positions[index + axis]) +
                                    placement.origin[axis] - found->second.placement.origin[axis];
          mesh.positions[index + axis] = static_cast<float>(translated);
        }
      } else if (placement.x_axis == found->second.placement.x_axis &&
                 placement.y_axis == found->second.placement.y_axis &&
                 placement.z_axis == found->second.placement.z_axis) {
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          const double translated = static_cast<double>(mesh.positions[index + axis]) +
                                    placement.origin[axis] - found->second.placement.origin[axis];
          mesh.positions[index + axis] = static_cast<float>(translated);
        }
      } else {
        const auto local = local_point_unrounded({static_cast<double>(mesh.positions[index]),
                                                  static_cast<double>(mesh.positions[index + 1U]),
                                                  static_cast<double>(mesh.positions[index + 2U])},
                                                 found->second.placement);
        const auto world = world_point(local, placement);
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          mesh.positions[index + axis] = static_cast<float>(world[axis]);
        }
      }
    }
    if (std::getenv("TEKLA_DB1_OCCT_CACHE_PROFILE") != nullptr) {
      std::fprintf(stderr,
                   "{\"occt_cache\":true,\"object_id\":%llu,\"outcome\":\"hit\","
                   "\"key_bytes\":%zu}\n",
                   static_cast<unsigned long long>(object_id), key.bytes.size());
    }
    return mesh;
  }

  void insert(CacheKey key, GeometryEvaluationPlacement placement, const OcctMesh& mesh) {
    if (std::getenv("TEKLA_DB1_DISABLE_OCCT_CACHE") != nullptr) return;
    const std::size_t accounting_entry = key.kind == CanonicalizationKind::translation
                                             ? sizeof(TranslationAccountingEntry)
                                             : sizeof(CacheEntry);
    const std::size_t entry_bytes = key.bytes.size() + mesh.positions.size() * sizeof(float) +
                                    mesh.indices.size() * sizeof(std::uint32_t) + accounting_entry;
    if (entries.size() >= kMaximumEntries || entry_bytes > kMaximumBytes - bytes) return;
    CacheEntry entry{placement, mesh};
    const bool inserted = entries.emplace(std::move(key), std::move(entry)).second;
    if (!inserted) return;
    bytes += entry_bytes;
    if (std::getenv("TEKLA_DB1_OCCT_CACHE_PROFILE") != nullptr) {
      std::fprintf(stderr,
                   "{\"occt_cache\":true,\"object_id\":%llu,\"outcome\":\"insert\","
                   "\"entry_bytes\":%zu,\"total_bytes\":%zu}\n",
                   static_cast<unsigned long long>(mesh.object_id), entry_bytes, bytes);
    }
  }

  std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> entries;
  std::size_t bytes = 0U;
};

GeometryEvaluationCache::GeometryEvaluationCache() : impl_(std::make_unique<Impl>()) {}
GeometryEvaluationCache::~GeometryEvaluationCache() = default;
GeometryEvaluationCache::GeometryEvaluationCache(GeometryEvaluationCache&&) noexcept = default;
GeometryEvaluationCache& GeometryEvaluationCache::operator=(GeometryEvaluationCache&&) noexcept =
    default;

Result<OcctMesh> GeometryEvaluationCache::evaluate(const OcctRequest& request,
                                                   const Evaluator& evaluator) {
  auto key = encode_translation_normalized_occt_request(request);
  if (key) {
    CacheKey cache_key{key.value().bytes, CanonicalizationKind::translation};
    const auto placement = translation_placement(key.value().origin);
    if (auto cached = impl_->find(cache_key, placement, request.object_id)) {
      return Result<OcctMesh>::success(std::move(*cached));
    }
  }
  auto evaluated = evaluator(request);
  if (evaluated && key) {
    CacheKey cache_key{std::move(key.value().bytes), CanonicalizationKind::translation};
    impl_->insert(std::move(cache_key), translation_placement(key.value().origin),
                  evaluated.value());
  }
  return evaluated;
}

Result<OcctMesh> GeometryEvaluationCache::evaluate(const OcctRequest& request,
                                                   const GeometryEvaluationPlacement& placement,
                                                   const Evaluator& evaluator) {
  auto rigid = rigid_key(request, placement);
  if (!rigid) return evaluate(request, evaluator);
  if (auto cached = impl_->find(*rigid, placement, request.object_id)) {
    return Result<OcctMesh>::success(std::move(*cached));
  }
  auto evaluated = evaluator(request);
  if (evaluated) impl_->insert(std::move(*rigid), placement, evaluated.value());
  return evaluated;
}

}  // namespace tekla::db1::detail
