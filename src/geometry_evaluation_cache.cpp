#include "geometry_evaluation_cache.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "occt/protocol.hpp"

namespace tekla::db1::detail {
namespace {

constexpr std::size_t kMaximumEntries = 4096U;
constexpr std::size_t kMaximumBytes = 256U * 1024U * 1024U;

struct CacheKeyHash {
  std::size_t operator()(const std::vector<std::byte>& bytes) const noexcept {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    for (const auto byte : bytes) {
      value ^= std::to_integer<std::uint8_t>(byte);
      value *= 0x100000001b3ULL;
    }
    return static_cast<std::size_t>(value ^ (value >> 32U));
  }
};

struct CacheEntry {
  std::array<double, 3> origin{};
  OcctMesh mesh;
};

}  // namespace

struct GeometryEvaluationCache::Impl {
  [[nodiscard]] std::optional<OcctMesh> find(const TranslationNormalizedOcctRequest& key,
                                             std::uint64_t object_id) const {
    if (std::getenv("TEKLA_DB1_DISABLE_OCCT_CACHE") != nullptr) return std::nullopt;
    const auto found = entries.find(key.bytes);
    if (found == entries.end()) return std::nullopt;
    OcctMesh mesh = found->second.mesh;
    mesh.object_id = object_id;
    for (std::size_t index = 0U; index + 2U < mesh.positions.size(); index += 3U) {
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const double translated = static_cast<double>(mesh.positions[index + axis]) +
                                  key.origin[axis] - found->second.origin[axis];
        mesh.positions[index + axis] = static_cast<float>(translated);
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

  void insert(TranslationNormalizedOcctRequest key, const OcctMesh& mesh) {
    if (std::getenv("TEKLA_DB1_DISABLE_OCCT_CACHE") != nullptr) return;
    const std::size_t entry_bytes = key.bytes.size() + mesh.positions.size() * sizeof(float) +
                                    mesh.indices.size() * sizeof(std::uint32_t) +
                                    sizeof(CacheEntry);
    if (entries.size() >= kMaximumEntries || entry_bytes > kMaximumBytes - bytes) return;
    CacheEntry entry{key.origin, mesh};
    const bool inserted = entries.emplace(std::move(key.bytes), std::move(entry)).second;
    if (!inserted) return;
    bytes += entry_bytes;
    if (std::getenv("TEKLA_DB1_OCCT_CACHE_PROFILE") != nullptr) {
      std::fprintf(stderr,
                   "{\"occt_cache\":true,\"object_id\":%llu,\"outcome\":\"insert\","
                   "\"entry_bytes\":%zu,\"total_bytes\":%zu}\n",
                   static_cast<unsigned long long>(mesh.object_id), entry_bytes, bytes);
    }
  }

  std::unordered_map<std::vector<std::byte>, CacheEntry, CacheKeyHash> entries;
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
    if (auto cached = impl_->find(key.value(), request.object_id)) {
      return Result<OcctMesh>::success(std::move(*cached));
    }
  }
  auto evaluated = evaluator(request);
  if (evaluated && key) impl_->insert(std::move(key.value()), evaluated.value());
  return evaluated;
}

}  // namespace tekla::db1::detail
