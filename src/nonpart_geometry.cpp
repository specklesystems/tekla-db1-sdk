#include "nonpart_geometry.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "record.hpp"

namespace tekla::db1::detail {
namespace {

struct ObjectType {
  std::uint32_t type = 0;
  std::uint32_t subtype = 0;
};

struct Axes {
  Vector3d x;
  Vector3d y;
};

struct CoordinateSystem {
  std::uint32_t axes_id = 0;
  Vector3d origin;
  double length = 0.0;
};

struct NumericArray {
  std::uint32_t next_id = 0;
  std::vector<double> values;
};

struct RebarAttribute {
  std::uint32_t profile_id = 0;
  std::uint32_t bar_type_info_id = 0;
};

struct MeshAttribute {
  std::uint32_t diameters_id = 0;
  std::uint32_t spacings_id = 0;
  double width = 0.0;
  double height = 0.0;
  double longitudinal_overhang_left = 0.0;
  double cross_overhang_left = 0.0;
  std::uint32_t flags = 0;
};

struct MeshCenterlines {
  std::vector<std::vector<Vector3d>> cross;
  std::vector<std::vector<Vector3d>> longitudinal;
};

struct CurveData {
  std::uint64_t object_id = 0;
  CurveGeometryKind kind = CurveGeometryKind::polyline;
  std::vector<Vector3d> points;
  double radius = 0.0;
};

struct MeshData {
  std::uint64_t object_id = 0;
  std::vector<float> positions;
  std::vector<std::uint32_t> indices;
};

struct WeldDefinition {
  std::uint32_t common_attribute_id = 0;
  std::uint32_t above_attribute_id = 0;
  std::uint32_t below_attribute_id = 0;
};

struct WeldCommonGeometry {
  bool compound = false;
  bool logical = false;
};

struct WeldSeamGeometry {
  double size = 0.0;
  std::uint32_t type = 0;
};

struct WeldPolygonRow {
  std::uint32_t number = 0;
  std::uint32_t type = 0;
  std::vector<Vector3d> values;
};

struct DiagnosticData {
  ErrorCode code = ErrorCode::decoder_unavailable;
  std::uint64_t object_id = 0;
  std::string message;
};

class RetainedGeometryBudget {
 public:
  RetainedGeometryBudget(std::size_t payload_bytes, std::uint64_t requested_bytes) {
    constexpr std::size_t default_payload_multiplier = 64U;
    if (requested_bytes != 0U) {
      limit_ = requested_bytes > std::numeric_limits<std::size_t>::max()
                   ? std::numeric_limits<std::size_t>::max()
                   : static_cast<std::size_t>(requested_bytes);
    } else if (payload_bytes >
               std::numeric_limits<std::size_t>::max() / default_payload_multiplier) {
      limit_ = std::numeric_limits<std::size_t>::max();
    } else {
      limit_ = std::max<std::size_t>(1U, payload_bytes * default_payload_multiplier);
    }
  }

  [[nodiscard]] std::size_t maximum_curve_count(std::size_t point_count) const noexcept {
    const auto bytes = curve_bytes(point_count);
    return bytes && *bytes != 0U ? remaining() / *bytes : 0U;
  }

  [[nodiscard]] bool can_consume_curve(std::size_t point_count) const noexcept {
    const auto bytes = curve_bytes(point_count);
    return bytes && *bytes <= remaining();
  }

  [[nodiscard]] bool can_consume_curve_sets(
      std::span<const std::pair<std::size_t, std::size_t>> sets) const noexcept {
    std::size_t total = 0U;
    for (const auto [curve_count, point_count] : sets) {
      const auto per_curve = curve_bytes(point_count);
      if (!per_curve) return false;
      const auto bytes = checked_product(curve_count, *per_curve);
      if (!bytes) return false;
      const auto next = checked_sum(total, *bytes);
      if (!next) return false;
      total = *next;
    }
    return total <= remaining();
  }

  [[nodiscard]] bool consume_curve(std::size_t point_count) noexcept {
    const auto bytes = curve_bytes(point_count);
    return bytes && consume(*bytes);
  }

  [[nodiscard]] bool can_consume_mesh(std::size_t position_count, std::size_t index_count,
                                      bool include_record = true) const noexcept {
    const auto bytes = mesh_bytes(position_count, index_count, include_record);
    return bytes && *bytes <= remaining();
  }

  [[nodiscard]] bool consume_mesh(std::size_t position_count, std::size_t index_count,
                                  bool include_record = true) noexcept {
    const auto bytes = mesh_bytes(position_count, index_count, include_record);
    return bytes && consume(*bytes);
  }

  [[nodiscard]] bool can_consume_decode_bytes(std::size_t bytes) const noexcept {
    return bytes <= remaining();
  }

  [[nodiscard]] bool consume_decode_bytes(std::size_t bytes) noexcept { return consume(bytes); }

  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

  void exhaust() noexcept { used_ = limit_; }

 private:
  [[nodiscard]] static std::optional<std::size_t> checked_product(std::size_t lhs,
                                                                  std::size_t rhs) noexcept {
    if (lhs != 0U && rhs > std::numeric_limits<std::size_t>::max() / lhs) return std::nullopt;
    return lhs * rhs;
  }

  [[nodiscard]] static std::optional<std::size_t> checked_sum(std::size_t lhs,
                                                              std::size_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) return std::nullopt;
    return lhs + rhs;
  }

  [[nodiscard]] static std::optional<std::size_t> curve_bytes(std::size_t point_count) noexcept {
    const auto points = checked_product(point_count, sizeof(Vector3d));
    return points ? checked_sum(sizeof(CurveData), *points) : std::nullopt;
  }

  [[nodiscard]] static std::optional<std::size_t> mesh_bytes(std::size_t position_count,
                                                             std::size_t index_count,
                                                             bool include_record) noexcept {
    const auto positions = checked_product(position_count, sizeof(float));
    const auto indices = checked_product(index_count, sizeof(std::uint32_t));
    if (!positions || !indices) return std::nullopt;
    const auto arrays = checked_sum(*positions, *indices);
    return !arrays          ? std::nullopt
           : include_record ? checked_sum(sizeof(MeshData), *arrays)
                            : arrays;
  }

  [[nodiscard]] std::size_t remaining() const noexcept { return limit_ - used_; }

  [[nodiscard]] bool consume(std::size_t bytes) noexcept {
    if (bytes > remaining()) return false;
    used_ += bytes;
    return true;
  }

  std::size_t limit_ = 0U;
  std::size_t used_ = 0U;
};

enum class LinkedDecodeStatus {
  success,
  invalid,
  resource_limit,
};

template <typename Value>
struct LinkedDecodeResult {
  LinkedDecodeStatus status = LinkedDecodeStatus::invalid;
  Value value;
};

class LinkedRowVisitBudget {
 public:
  LinkedRowVisitBudget(std::size_t linked_row_count, std::size_t decode_budget_bytes) {
    const auto budget_visits = decode_budget_bytes / sizeof(std::uint32_t);
    limit_ = budget_visits > std::numeric_limits<std::size_t>::max() - linked_row_count
                 ? std::numeric_limits<std::size_t>::max()
                 : linked_row_count + budget_visits;
  }

  [[nodiscard]] bool consume() noexcept {
    if (used_ == limit_) return false;
    ++used_;
    return true;
  }

 private:
  std::size_t limit_ = 0U;
  std::size_t used_ = 0U;
};

struct PolygonChunk {
  std::uint32_t number = 0;
  std::vector<std::array<double, 2>> points;
};

struct BoltAxisLimits {
  double start = 0.0;
  double end = 0.0;
};

struct BoltDisplayAttribute {
  std::string standard;
  double diameter = 0.0;
  double length = 0.0;
  std::uint32_t structure = 0U;
  bool legacy = false;
};

[[nodiscard]] bool hidden(std::span<const std::byte> record) noexcept {
  return record.empty() || (std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U;
}

[[nodiscard]] double scalar(std::span<const std::byte> tuple, const FieldSchema& field) noexcept {
  return field.type == FieldType::f32 ? static_cast<double>(read_f32(tuple, field.offset))
                                      : read_f64(tuple, field.offset);
}

[[nodiscard]] double vector_length(Vector3d value) noexcept {
  return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

[[nodiscard]] Vector3d subtract(Vector3d lhs, Vector3d rhs) noexcept {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

[[nodiscard]] Vector3d add(Vector3d lhs, Vector3d rhs) noexcept {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] Vector3d scale(Vector3d value, double factor) noexcept {
  return {value.x * factor, value.y * factor, value.z * factor};
}

[[nodiscard]] Vector3d cross(Vector3d lhs, Vector3d rhs) noexcept {
  return {lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z,
          lhs.x * rhs.y - lhs.y * rhs.x};
}

[[nodiscard]] double dot(Vector3d lhs, Vector3d rhs) noexcept {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] std::optional<Vector3d> normalized(Vector3d value) noexcept {
  const double magnitude = vector_length(value);
  if (!std::isfinite(magnitude) || magnitude <= 1.0e-12) return std::nullopt;
  return scale(value, 1.0 / magnitude);
}

[[nodiscard]] Vector3d transform(Vector3d local, const CoordinateSystem& system,
                                 const Axes& axes) noexcept {
  const auto z = cross(axes.x, axes.y);
  return add(system.origin,
             add(scale(axes.x, local.x), add(scale(axes.y, local.y), scale(z, local.z))));
}

[[nodiscard]] const TableSchema* populated_table(const ModelStorage& storage, const Schema& schema,
                                                 std::span<const std::string_view> names) noexcept {
  for (const auto name : names) {
    const auto* table = schema.find_table(name);
    if (table != nullptr && table->ordinal < storage.layout.tables.size() &&
        storage.layout.tables[table->ordinal].info.row_count != 0U) {
      return table;
    }
  }
  return nullptr;
}

[[nodiscard]] bool has_legacy_nonpart_rows(const ModelStorage& storage,
                                           const Schema& schema) noexcept {
  constexpr std::array<std::string_view, 3> prefixes{"old_rebar_", "old_rebarset_", "old_bolt_"};
  for (const auto& table : schema.tables) {
    if (table.ordinal >= storage.layout.tables.size() ||
        storage.layout.tables[table.ordinal].info.row_count == 0U)
      continue;
    for (const auto prefix : prefixes) {
      if (!table.name.starts_with(prefix)) continue;
      const auto suffix = table.name.substr(prefix.size());
      if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(),
                                         [](char value) { return value >= '0' && value <= '9'; }))
        return true;
    }
  }
  return false;
}

[[nodiscard]] std::unordered_map<std::uint32_t, NumericArray> numeric_arrays(
    const ModelStorage& storage, const Schema& schema, const TableSchema* table) {
  std::unordered_map<std::uint32_t, NumericArray> result;
  if (table == nullptr) return result;
  const auto* id = find_field(schema, *table, "id");
  const auto* next = find_field(schema, *table, "next_id");
  const auto* count = find_field(schema, *table, "n_values");
  if (id == nullptr || next == nullptr || count == nullptr) return result;
  const auto& layout = storage.layout.tables[table->ordinal];
  result.reserve(static_cast<std::size_t>(layout.info.row_count));
  for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
    const auto record = layout.record(storage.payload.bytes(), row);
    if (hidden(record)) continue;
    const auto tuple = record.subspan(1U, table->tuple_size);
    NumericArray array{.next_id = read_u32(tuple, next->offset)};
    const auto persisted_count = static_cast<std::size_t>(read_u32(tuple, count->offset));
    const auto value_count = std::min(persisted_count, table->tuple_size / sizeof(std::uint32_t));
    array.values.reserve(value_count);
    for (std::size_t index = 0; index < value_count; ++index) {
      const auto* value = find_field(schema, *table, "value_" + std::to_string(index));
      if (value == nullptr) break;
      array.values.push_back(value->type == FieldType::u32
                                 ? static_cast<double>(read_u32(tuple, value->offset))
                                 : scalar(tuple, *value));
    }
    result.insert_or_assign(read_u32(tuple, id->offset), std::move(array));
  }
  return result;
}

using NumericDecodeCache =
    std::unordered_map<std::uint32_t, LinkedDecodeResult<std::vector<double>>>;

[[nodiscard]] const LinkedDecodeResult<std::vector<double>>& resolve_array(
    const std::unordered_map<std::uint32_t, NumericArray>& arrays, std::uint32_t id,
    NumericDecodeCache& cache, LinkedRowVisitBudget& visit_budget,
    RetainedGeometryBudget& memory_budget) {
  const auto [cached, inserted] = cache.try_emplace(id);
  if (!inserted) return cached->second;

  auto& result = cached->second;
  std::vector<const NumericArray*> chunks;
  std::unordered_set<std::uint32_t> seen;
  std::size_t value_count = 0U;
  auto current = id;
  while (current != 0U) {
    if (!visit_budget.consume()) {
      result.status = LinkedDecodeStatus::resource_limit;
      memory_budget.exhaust();
      return result;
    }
    if (!seen.insert(current).second) return result;
    const auto found = arrays.find(current);
    if (found == arrays.end()) return result;
    if (found->second.values.size() > std::numeric_limits<std::size_t>::max() - value_count) {
      result.status = LinkedDecodeStatus::resource_limit;
      memory_budget.exhaust();
      return result;
    }
    value_count += found->second.values.size();
    if (value_count > std::numeric_limits<std::size_t>::max() / sizeof(double) ||
        !memory_budget.can_consume_decode_bytes(value_count * sizeof(double))) {
      result.status = LinkedDecodeStatus::resource_limit;
      memory_budget.exhaust();
      return result;
    }
    chunks.push_back(&found->second);
    current = found->second.next_id;
  }

  result.value.reserve(value_count);
  for (const auto* chunk : chunks)
    result.value.insert(result.value.end(), chunk->values.begin(), chunk->values.end());
  if (!memory_budget.consume_decode_bytes(value_count * sizeof(double))) {
    result.value.clear();
    result.status = LinkedDecodeStatus::resource_limit;
    memory_budget.exhaust();
    return result;
  }
  result.status = LinkedDecodeStatus::success;
  return result;
}

using StringDecodeCache = std::unordered_map<std::uint32_t, LinkedDecodeResult<std::string>>;

[[nodiscard]] const LinkedDecodeResult<std::string>& resolve_string(
    const std::unordered_map<std::uint32_t, std::string>& strings,
    const std::unordered_map<std::uint32_t, std::uint32_t>& next_ids, std::uint32_t id,
    StringDecodeCache& cache, LinkedRowVisitBudget& visit_budget,
    RetainedGeometryBudget& memory_budget) {
  const auto [cached, inserted] = cache.try_emplace(id);
  if (!inserted) return cached->second;

  auto& result = cached->second;
  std::vector<const std::string*> chunks;
  std::unordered_set<std::uint32_t> seen;
  std::size_t byte_count = 0U;
  auto current = id;
  while (current != 0U) {
    if (!visit_budget.consume()) {
      result.status = LinkedDecodeStatus::resource_limit;
      memory_budget.exhaust();
      return result;
    }
    if (!seen.insert(current).second) return result;
    const auto value = strings.find(current);
    const auto next = next_ids.find(current);
    if (value == strings.end() || next == next_ids.end()) return result;
    if (value->second.size() > std::numeric_limits<std::size_t>::max() - byte_count) {
      result.status = LinkedDecodeStatus::resource_limit;
      memory_budget.exhaust();
      return result;
    }
    byte_count += value->second.size();
    if (!memory_budget.can_consume_decode_bytes(byte_count)) {
      result.status = LinkedDecodeStatus::resource_limit;
      memory_budget.exhaust();
      return result;
    }
    chunks.push_back(&value->second);
    current = next->second;
  }

  result.value.reserve(byte_count);
  for (const auto* chunk : chunks) result.value += *chunk;
  if (!memory_budget.consume_decode_bytes(byte_count)) {
    result.value.clear();
    result.status = LinkedDecodeStatus::resource_limit;
    memory_budget.exhaust();
    return result;
  }
  result.status = LinkedDecodeStatus::success;
  return result;
}

[[nodiscard]] std::optional<double> rebar_radius(std::string_view size) noexcept {
  const auto first = size.find_first_not_of(" \t\r\n");
  const auto last = size.find_last_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::nullopt;
  size = size.substr(first, last - first + 1U);
  for (const auto& [name, diameter] : std::array<std::pair<std::string_view, double>, 4>{
           {{"6", 6.9}, {"7", 8.1}, {"8", 10.0}, {"12", 14.0}}}) {
    if (size == name) return diameter / 2.0;
  }
  double diameter = 0.0;
  const auto parsed = std::from_chars(size.data(), size.data() + size.size(), diameter);
  if (parsed.ec != std::errc{} || parsed.ptr != size.data() + size.size() ||
      !std::isfinite(diameter) || diameter <= 0.0) {
    return std::nullopt;
  }
  return diameter / 2.0;
}

[[nodiscard]] std::optional<std::array<std::string_view, 2>> mesh_string_pair(
    std::string_view value) noexcept {
  const auto separator = value.find('\t');
  if (separator == std::string_view::npos) return std::nullopt;
  const auto second_start = separator + 1U;
  const auto second_end = value.find('\t', second_start);
  return std::array<std::string_view, 2>{
      value.substr(0U, separator),
      value.substr(second_start, second_end == std::string_view::npos ? std::string_view::npos
                                                                      : second_end - second_start)};
}

[[nodiscard]] std::optional<double> positive_number(std::string_view value) noexcept {
  const auto first = value.find_first_not_of(" \t\r\n");
  const auto last = value.find_last_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::nullopt;
  value = value.substr(first, last - first + 1U);
  double parsed_value = 0.0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                 std::isfinite(parsed_value) && parsed_value > 0.0
             ? std::optional<double>{parsed_value}
             : std::nullopt;
}

[[nodiscard]] Result<std::vector<double>> spacing_distances(double length,
                                                            std::uint32_t spacing_type,
                                                            std::span<const double> spacings,
                                                            std::size_t maximum_count) {
  length = std::max(0.0, length);
  if (spacings.empty()) {
    return Result<std::vector<double>>::failure(
        {ErrorCode::invalid_geometry, "Classic rebar-group spacing array is empty."});
  }
  std::vector<double> result;
  if (spacing_type == 2U) {
    if (!std::isfinite(spacings.front())) {
      return Result<std::vector<double>>::failure(
          {ErrorCode::invalid_geometry, "Classic rebar-group count is not finite."});
    }
    if (spacings.front() <= 0.0) return Result<std::vector<double>>::success({});
    if (spacings.front() > static_cast<double>(maximum_count)) {
      return Result<std::vector<double>>::failure(
          {ErrorCode::resource_limit,
           "Classic rebar-group expansion exceeds the aggregate geometry memory budget."});
    }
    const auto count = static_cast<std::int64_t>(std::llround(spacings.front()));
    if (count <= 0) return Result<std::vector<double>>::success({});
    if (count == 1) return Result<std::vector<double>>::success({0.0});
    result.reserve(static_cast<std::size_t>(count));
    for (std::int64_t index = 0; index < count; ++index) {
      result.push_back(length * static_cast<double>(index) / static_cast<double>(count - 1));
    }
    return Result<std::vector<double>>::success(std::move(result));
  }
  if (spacing_type == 1U) {
    if (spacings.size() >= maximum_count) {
      return Result<std::vector<double>>::failure(
          {ErrorCode::resource_limit,
           "Classic rebar-group expansion exceeds the aggregate geometry memory budget."});
    }
    result.push_back(0.0);
    for (const auto spacing : spacings) {
      if (!std::isfinite(spacing) || spacing <= 0.0) {
        return Result<std::vector<double>>::failure(
            {ErrorCode::invalid_geometry, "Classic rebar-group spacing is invalid."});
      }
      result.push_back(result.back() + spacing);
    }
    return Result<std::vector<double>>::success(std::move(result));
  }
  const double target = spacings.front();
  if (!std::isfinite(target) || target <= 0.0 || !std::isfinite(length)) {
    return Result<std::vector<double>>::failure(
        {ErrorCode::invalid_geometry, "Classic rebar-group spacing geometry is invalid."});
  }
  if (length <= 1.0e-7) return Result<std::vector<double>>::success({0.0});
  if (spacing_type == 3U) {
    const double interval_count = std::ceil(length / target);
    if (!std::isfinite(interval_count)) {
      return Result<std::vector<double>>::failure(
          {ErrorCode::invalid_geometry, "Classic rebar-group interval count is invalid."});
    }
    if (interval_count + 1.0 > static_cast<double>(maximum_count)) {
      return Result<std::vector<double>>::failure(
          {ErrorCode::resource_limit,
           "Classic rebar-group expansion exceeds the aggregate geometry memory budget."});
    }
    const auto intervals = std::max<std::int64_t>(1, static_cast<std::int64_t>(interval_count));
    result.reserve(static_cast<std::size_t>(intervals + 1));
    for (std::int64_t index = 0; index <= intervals; ++index) {
      result.push_back(length * static_cast<double>(index) / static_cast<double>(intervals));
    }
    return Result<std::vector<double>>::success(std::move(result));
  }
  if (spacing_type < 4U || spacing_type > 7U) {
    return Result<std::vector<double>>::failure(
        {ErrorCode::decoder_unavailable, "Classic rebar-group spacing type is unsupported."});
  }
  const double fixed_count = std::floor(length / target);
  if (!std::isfinite(fixed_count)) {
    return Result<std::vector<double>>::failure(
        {ErrorCode::invalid_geometry, "Classic rebar-group fixed spacing count is invalid."});
  }
  if (fixed_count + 2.0 > static_cast<double>(maximum_count)) {
    return Result<std::vector<double>>::failure(
        {ErrorCode::resource_limit,
         "Classic rebar-group expansion exceeds the aggregate geometry memory budget."});
  }
  const auto fixed = std::max<std::int64_t>(0, static_cast<std::int64_t>(fixed_count) - 1);
  const double remainder = length - static_cast<double>(fixed) * target;
  if (spacing_type == 4U) {
    result.push_back(0.0);
    for (std::int64_t index = 0; index <= fixed; ++index)
      result.push_back(remainder + static_cast<double>(index) * target);
  } else if (spacing_type == 5U) {
    for (std::int64_t index = 0; index <= fixed; ++index)
      result.push_back(static_cast<double>(index) * target);
    result.push_back(length);
  } else if (spacing_type == 6U) {
    result.push_back(0.0);
    const double edge = remainder / 2.0;
    for (std::int64_t index = 0; index <= fixed; ++index)
      result.push_back(edge + static_cast<double>(index) * target);
    result.push_back(length);
  } else if (spacing_type == 7U) {
    const auto left = fixed / 2;
    for (std::int64_t index = 0; index <= left; ++index)
      result.push_back(static_cast<double>(index) * target);
    const auto right = fixed - left;
    for (std::int64_t index = right; index >= 0; --index)
      result.push_back(length - static_cast<double>(index) * target);
  }
  return Result<std::vector<double>>::success(std::move(result));
}

[[nodiscard]] std::optional<std::vector<Vector3d>> offset_polyline(std::span<const Vector3d> points,
                                                                   std::span<const double> offsets,
                                                                   double start_trim,
                                                                   double end_trim) {
  if (points.size() < 2U || offsets.size() + 1U != points.size()) return std::nullopt;
  for (const auto point : points) {
    if (std::abs(point.z - points.front().z) > 0.002) return std::nullopt;
  }
  std::vector<std::array<double, 2>> directions;
  directions.reserve(points.size() - 1U);
  for (std::size_t index = 0; index + 1U < points.size(); ++index) {
    const double dx = points[index + 1U].x - points[index].x;
    const double dy = points[index + 1U].y - points[index].y;
    const double magnitude = std::hypot(dx, dy);
    if (magnitude <= 1.0e-9) return std::nullopt;
    directions.push_back({dx / magnitude, dy / magnitude});
  }
  std::vector<Vector3d> result;
  result.reserve(points.size());
  const auto normal = [](std::array<double, 2> direction) {
    return std::array<double, 2>{-direction[1], direction[0]};
  };
  auto first_normal = normal(directions.front());
  result.push_back(
      {points.front().x + first_normal[0] * offsets.front() + directions.front()[0] * start_trim,
       points.front().y + first_normal[1] * offsets.front() + directions.front()[1] * start_trim,
       points.front().z});
  for (std::size_t index = 1U; index + 1U < points.size(); ++index) {
    const auto previous = directions[index - 1U];
    const auto following = directions[index];
    const auto previous_normal = normal(previous);
    const auto following_normal = normal(following);
    const std::array<double, 2> previous_origin{
        points[index].x + previous_normal[0] * offsets[index - 1U],
        points[index].y + previous_normal[1] * offsets[index - 1U]};
    const std::array<double, 2> following_origin{
        points[index].x + following_normal[0] * offsets[index],
        points[index].y + following_normal[1] * offsets[index]};
    const double denominator = previous[0] * following[1] - previous[1] * following[0];
    double x = (previous_origin[0] + following_origin[0]) / 2.0;
    double y = (previous_origin[1] + following_origin[1]) / 2.0;
    if (std::abs(denominator) > 1.0e-9) {
      const std::array<double, 2> delta{following_origin[0] - previous_origin[0],
                                        following_origin[1] - previous_origin[1]};
      const double distance = (delta[0] * following[1] - delta[1] * following[0]) / denominator;
      x = previous_origin[0] + previous[0] * distance;
      y = previous_origin[1] + previous[1] * distance;
    }
    result.push_back({x, y, points[index].z});
  }
  auto last_normal = normal(directions.back());
  result.push_back(
      {points.back().x + last_normal[0] * offsets.back() - directions.back()[0] * end_trim,
       points.back().y + last_normal[1] * offsets.back() - directions.back()[1] * end_trim,
       points.back().z});
  return result;
}

[[nodiscard]] Result<std::vector<std::vector<Vector3d>>> group_centerlines(
    std::span<const Vector3d> polygon, std::span<const double> geometry, std::uint32_t spacing_type,
    std::span<const double> spacings, std::uint32_t exclude_type, std::size_t maximum_count) {
  if (polygon.size() < 2U || geometry.size() < 12U || (geometry.size() - 9U) % 3U != 0U) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Classic rebar-group geometry array is invalid."});
  }
  const auto segment_count = polygon.size() - 1U;
  auto stored_leg_count = (geometry.size() - 9U) / 3U;
  bool closed_extra = segment_count >= 3U &&
                      vector_length(subtract(polygon.front(), polygon.back())) <= 0.002 &&
                      stored_leg_count == segment_count + 1U;
  if (stored_leg_count != 1U && stored_leg_count != segment_count && !closed_extra) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Classic rebar-group leg count is invalid."});
  }
  if (closed_extra) --stored_leg_count;
  std::vector<double> offsets;
  offsets.reserve(segment_count);
  for (std::size_t index = 0; index < stored_leg_count; ++index) {
    offsets.push_back((geometry[6U + index * 3U] + geometry[7U + index * 3U]) / 2.0);
  }
  if (offsets.size() == 1U) offsets.resize(segment_count, offsets.front());
  auto offset = offset_polyline(polygon, offsets, geometry[geometry.size() - 3U],
                                geometry[geometry.size() - 2U]);
  if (!offset) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Classic rebar-group offset polygon is invalid."});
  }
  const Vector3d distribution_start{geometry[0], geometry[1], geometry[2]};
  const Vector3d distribution_end{geometry[3], geometry[4], geometry[5]};
  const auto vector = subtract(distribution_end, distribution_start);
  const double distribution_length = vector_length(vector);
  if (!std::isfinite(distribution_length) || distribution_length <= 1.0e-9) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Classic rebar-group distribution vector is invalid."});
  }
  const auto direction = scale(vector, 1.0 / distribution_length);
  const double start_from_plane = geometry[8U];
  const double end_from_plane = geometry.back();
  const auto usable_start = add(distribution_start, scale(direction, start_from_plane));
  const auto usable_end = subtract(distribution_end, scale(direction, end_from_plane));
  const double usable_length = (usable_end.x - usable_start.x) * direction.x +
                               (usable_end.y - usable_start.y) * direction.y +
                               (usable_end.z - usable_start.z) * direction.z;
  auto distances = spacing_distances(usable_length, spacing_type, spacings, maximum_count);
  if (!distances) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(distances.error());
  }
  if (exclude_type < 1U || exclude_type > 4U) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::decoder_unavailable, "Classic rebar-group exclusion type is unsupported."});
  }
  if ((exclude_type == 2U || exclude_type == 4U) && !distances.value().empty())
    distances.value().erase(distances.value().begin());
  if ((exclude_type == 3U || exclude_type == 4U) && !distances.value().empty())
    distances.value().pop_back();
  const Vector3d base{distribution_start.x, distribution_start.y, polygon.front().z};
  std::vector<std::vector<Vector3d>> result;
  result.reserve(distances.value().size());
  for (const double distance : distances.value()) {
    const auto translation = subtract(add(usable_start, scale(direction, distance)), base);
    result.emplace_back();
    result.back().reserve(offset->size());
    for (const auto point : *offset) result.back().push_back(add(point, translation));
  }
  return Result<std::vector<std::vector<Vector3d>>>::success(std::move(result));
}

[[nodiscard]] Result<std::size_t> regular_distance_count(double start, double limit,
                                                         double spacing) {
  if (!std::isfinite(start) || !std::isfinite(limit) || !std::isfinite(spacing) || spacing <= 0.0) {
    return Result<std::size_t>::failure(
        {ErrorCode::invalid_geometry, "Classic rebar-mesh spacing is invalid."});
  }
  if (start > limit + 1.0e-7) return Result<std::size_t>::success(0U);
  const double count_value = std::floor((limit - start) / spacing + 1.0e-9) + 1.0;
  if (!std::isfinite(count_value) || count_value < 0.0 ||
      count_value > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
    return Result<std::size_t>::failure(
        {ErrorCode::resource_limit,
         "Classic rebar-mesh expansion exceeds the aggregate geometry memory budget."});
  }
  return Result<std::size_t>::success(static_cast<std::size_t>(count_value));
}

[[nodiscard]] Result<std::vector<double>> regular_distances(double start, double limit,
                                                            double spacing,
                                                            std::size_t maximum_count) {
  auto evaluated_count = regular_distance_count(start, limit, spacing);
  if (!evaluated_count) return Result<std::vector<double>>::failure(evaluated_count.error());
  if (evaluated_count.value() > maximum_count) {
    return Result<std::vector<double>>::failure(
        {ErrorCode::resource_limit,
         "Classic rebar-mesh expansion exceeds the aggregate geometry memory budget."});
  }
  const auto count = evaluated_count.value();
  std::vector<double> result;
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index)
    result.push_back(start + static_cast<double>(index) * spacing);
  return Result<std::vector<double>>::success(std::move(result));
}

[[nodiscard]] Result<MeshCenterlines> polygon_mesh_centerlines(
    std::span<const Vector3d> polygon, std::span<const double> geometry,
    double longitudinal_spacing, double cross_spacing, double longitudinal_overhang_left,
    double cross_overhang_left, double longitudinal_radius, double cross_radius,
    std::uint32_t cross_bar_location, std::size_t maximum_count) {
  if (polygon.size() < 4U || geometry.size() < 12U) {
    return Result<MeshCenterlines>::failure(
        {ErrorCode::invalid_geometry, "Classic polygon rebar-mesh arrays are inconsistent."});
  }
  for (const auto point : polygon) {
    if (std::abs(point.z - polygon.front().z) > 0.002) {
      return Result<MeshCenterlines>::failure(
          {ErrorCode::invalid_geometry, "Classic polygon rebar-mesh is not planar in local XY."});
    }
  }
  double x_min = polygon.front().x;
  double x_max = polygon.front().x;
  double y_min = polygon.front().y;
  double y_max = polygon.front().y;
  for (const auto point : polygon) {
    x_min = std::min(x_min, point.x);
    x_max = std::max(x_max, point.x);
    y_min = std::min(y_min, point.y);
    y_max = std::max(y_max, point.y);
  }
  const double edge_offset = (geometry[6U] + geometry[7U]) / 2.0;
  const double z_cross = polygon.front().z - geometry.back();
  const double separation = longitudinal_radius + cross_radius;
  const bool cross_is_above = cross_bar_location == 0U || cross_bar_location == 2U;
  const double z_longitudinal = z_cross + (cross_is_above ? -separation : separation);
  auto longitudinal_positions =
      regular_distances(x_min + edge_offset + longitudinal_overhang_left, x_max - edge_offset,
                        longitudinal_spacing, maximum_count);
  if (!longitudinal_positions)
    return Result<MeshCenterlines>::failure(longitudinal_positions.error());
  const auto remaining = maximum_count - longitudinal_positions.value().size();
  auto cross_positions = regular_distances(y_min + edge_offset + cross_overhang_left,
                                           y_max - edge_offset, cross_spacing, remaining);
  if (!cross_positions) return Result<MeshCenterlines>::failure(cross_positions.error());
  MeshCenterlines result;
  result.longitudinal.reserve(longitudinal_positions.value().size());
  for (const auto x : longitudinal_positions.value()) {
    result.longitudinal.push_back(
        {{x, y_min + edge_offset, z_longitudinal}, {x, y_max - edge_offset, z_longitudinal}});
  }
  result.cross.reserve(cross_positions.value().size());
  for (const auto y : cross_positions.value()) {
    result.cross.push_back({{x_min + edge_offset, y, z_cross}, {x_max - edge_offset, y, z_cross}});
  }
  return Result<MeshCenterlines>::success(std::move(result));
}

struct FilletedPathPoint {
  Vector3d point;
  std::array<double, 2> tangent;
};

struct FilletedPath {
  double radius = 0.0;
  double length = 0.0;
  std::vector<std::array<double, 2>> directions;
  std::vector<Vector3d> entries;
  std::vector<Vector3d> exits;
  std::vector<std::array<double, 2>> centers;
  std::vector<double> turns;
};

[[nodiscard]] std::optional<FilletedPath> filleted_path_xy(std::span<const Vector3d> points,
                                                           double radius) {
  if (points.size() < 2U || !std::isfinite(radius) || radius <= 0.0) return std::nullopt;
  FilletedPath result;
  result.radius = radius;
  result.directions.reserve(points.size() - 1U);
  result.entries.resize(points.size());
  result.exits.resize(points.size());
  result.centers.resize(points.size());
  result.turns.resize(points.size(), 0.0);
  for (std::size_t index = 0; index + 1U < points.size(); ++index) {
    const double dx = points[index + 1U].x - points[index].x;
    const double dy = points[index + 1U].y - points[index].y;
    const double leg = std::hypot(dx, dy);
    if (!std::isfinite(leg) || leg <= 1.0e-7 ||
        std::abs(points[index + 1U].z - points.front().z) > 0.002)
      return std::nullopt;
    result.directions.push_back({dx / leg, dy / leg});
    result.length += leg;
  }
  for (std::size_t index = 1U; index + 1U < points.size(); ++index) {
    const auto incoming = result.directions[index - 1U];
    const auto outgoing = result.directions[index];
    const double turn = std::atan2(incoming[0] * outgoing[1] - incoming[1] * outgoing[0],
                                   incoming[0] * outgoing[0] + incoming[1] * outgoing[1]);
    const double tangent_length = radius * std::tan(std::abs(turn) / 2.0);
    const auto vertex = points[index];
    const Vector3d entry{vertex.x - incoming[0] * tangent_length,
                         vertex.y - incoming[1] * tangent_length, vertex.z};
    const Vector3d exit{vertex.x + outgoing[0] * tangent_length,
                        vertex.y + outgoing[1] * tangent_length, vertex.z};
    const double side = turn > 0.0 ? 1.0 : -1.0;
    result.entries[index] = entry;
    result.exits[index] = exit;
    result.centers[index] = {entry.x - incoming[1] * side * radius,
                             entry.y + incoming[0] * side * radius};
    result.turns[index] = turn;
    result.length -= 2.0 * tangent_length;
    result.length += radius * std::abs(turn);
  }
  if (!std::isfinite(result.length) || result.length < 0.0) return std::nullopt;
  return result;
}

[[nodiscard]] std::optional<FilletedPathPoint> filleted_path_point_xy(
    std::span<const Vector3d> points, const FilletedPath& path, double distance) {
  if (points.size() < 2U || path.directions.size() + 1U != points.size() ||
      !std::isfinite(distance))
    return std::nullopt;
  const auto count = points.size();
  double remaining = std::max(0.0, distance);
  auto current = points.front();
  for (std::size_t segment = 0; segment < path.directions.size(); ++segment) {
    const auto direction = path.directions[segment];
    const auto corner = segment + 1U;
    const auto line_end = corner + 1U < count ? path.entries[corner] : points.back();
    const double line_length = vector_length(subtract(line_end, current));
    if (remaining <= line_length + 1.0e-9) {
      return FilletedPathPoint{
          {current.x + direction[0] * remaining, current.y + direction[1] * remaining, current.z},
          direction};
    }
    remaining -= line_length;
    if (corner + 1U >= count) return FilletedPathPoint{points.back(), direction};
    const auto center = path.centers[corner];
    const auto entry = path.entries[corner];
    const auto exit = path.exits[corner];
    const double turn = path.turns[corner];
    const double arc_length = path.radius * std::abs(turn);
    if (remaining <= arc_length + 1.0e-9) {
      const double side = turn > 0.0 ? 1.0 : -1.0;
      const double start_angle = std::atan2(entry.y - center[1], entry.x - center[0]);
      const double angle = start_angle + side * remaining / path.radius;
      return FilletedPathPoint{{center[0] + path.radius * std::cos(angle),
                                center[1] + path.radius * std::sin(angle), entry.z},
                               {-std::sin(angle) * side, std::cos(angle) * side}};
    }
    remaining -= arc_length;
    current = exit;
  }
  return FilletedPathPoint{points.back(), path.directions.back()};
}

[[nodiscard]] Result<MeshCenterlines> bent_mesh_centerlines(
    std::span<const Vector3d> polygon, std::span<const double> source_geometry, double bend_radius,
    double mesh_width, double longitudinal_spacing, double cross_spacing,
    double longitudinal_overhang_left, double cross_overhang_left, double longitudinal_radius,
    double cross_radius, std::size_t maximum_count) {
  if (polygon.size() < 2U) {
    return Result<MeshCenterlines>::failure(
        {ErrorCode::invalid_geometry, "Classic bent rebar-mesh polygon is incomplete."});
  }
  const auto segment_count = polygon.size() - 1U;
  std::vector<double> normalized_geometry;
  std::span<const double> geometry = source_geometry;
  if (source_geometry.size() == 12U && segment_count > 1U) {
    normalized_geometry.insert(normalized_geometry.end(), source_geometry.begin(),
                               source_geometry.begin() + 6U);
    for (std::size_t index = 0; index < segment_count; ++index) {
      normalized_geometry.insert(normalized_geometry.end(), source_geometry.begin() + 6U,
                                 source_geometry.begin() + 9U);
    }
    normalized_geometry.insert(normalized_geometry.end(), source_geometry.begin() + 9U,
                               source_geometry.begin() + 12U);
    geometry = normalized_geometry;
  }
  if (geometry.size() != 9U + 3U * segment_count || !std::isfinite(mesh_width) ||
      mesh_width <= 0.0 || !std::isfinite(bend_radius) || bend_radius < 0.0) {
    return Result<MeshCenterlines>::failure(
        {ErrorCode::invalid_geometry, "Classic bent rebar-mesh arrays are inconsistent."});
  }
  std::vector<std::array<double, 2>> directions;
  directions.reserve(segment_count);
  for (std::size_t index = 0; index < segment_count; ++index) {
    const double dx = polygon[index + 1U].x - polygon[index].x;
    const double dy = polygon[index + 1U].y - polygon[index].y;
    const double length = std::hypot(dx, dy);
    if (!std::isfinite(length) || length <= 1.0e-7) {
      return Result<MeshCenterlines>::failure(
          {ErrorCode::invalid_geometry, "Classic bent rebar-mesh has a zero-length leg."});
    }
    directions.push_back({dx / length, dy / length});
  }
  double first_turn = 1.0;
  for (std::size_t index = 0; index + 1U < directions.size(); ++index) {
    const double turn = directions[index][0] * directions[index + 1U][1] -
                        directions[index][1] * directions[index + 1U][0];
    if (std::abs(turn) > 1.0e-8) {
      first_turn = turn;
      break;
    }
  }
  const double turn_sign = first_turn > 0.0 ? 1.0 : -1.0;
  const double physical_bend_radius = bend_radius + cross_radius;
  std::vector<double> stored_offsets;
  stored_offsets.reserve(segment_count);
  for (std::size_t index = 0; index < segment_count; ++index)
    stored_offsets.push_back((geometry[6U + 3U * index] + geometry[7U + 3U * index]) / 2.0);
  const auto shape_for_adjustment = [&](double adjustment) {
    std::vector<double> offsets;
    offsets.reserve(stored_offsets.size());
    for (const auto offset : stored_offsets) offsets.push_back(turn_sign * (offset - adjustment));
    return offset_polyline(polygon, offsets, 0.0, 0.0);
  };
  auto cross_shape = shape_for_adjustment(0.0);
  if (!cross_shape) {
    return Result<MeshCenterlines>::failure(
        {ErrorCode::invalid_geometry, "Classic bent rebar-mesh offset polygon is invalid."});
  }
  auto filleted_path = filleted_path_xy(*cross_shape, physical_bend_radius);
  if (!filleted_path) {
    return Result<MeshCenterlines>::failure(
        {ErrorCode::invalid_geometry, "Classic bent rebar-mesh fillet is invalid."});
  }
  if (std::abs(filleted_path->length - mesh_width) > 1.0e-6) {
    double low = -100.0;
    double high = *std::min_element(stored_offsets.begin(), stored_offsets.end()) - 1.0e-7;
    const auto length_error = [&](double adjustment) -> std::optional<double> {
      const auto shape = shape_for_adjustment(adjustment);
      if (!shape) return std::nullopt;
      const auto path = filleted_path_xy(*shape, physical_bend_radius);
      return path ? std::optional<double>{path->length - mesh_width} : std::nullopt;
    };
    auto low_error = length_error(low);
    auto high_error = length_error(high);
    if (!low_error || !high_error || *low_error * *high_error > 0.0) {
      return Result<MeshCenterlines>::failure(
          {ErrorCode::invalid_geometry,
           "Classic bent rebar-mesh physical cover adjustment is unresolved."});
    }
    for (std::size_t iteration = 0; iteration < 60U; ++iteration) {
      const double middle = (low + high) / 2.0;
      const auto middle_error = length_error(middle);
      if (!middle_error) {
        return Result<MeshCenterlines>::failure(
            {ErrorCode::invalid_geometry, "Classic bent rebar-mesh fillet is invalid."});
      }
      if (*low_error * *middle_error <= 0.0) {
        high = middle;
      } else {
        low = middle;
        low_error = middle_error;
      }
    }
    cross_shape = shape_for_adjustment((low + high) / 2.0);
    if (!cross_shape) {
      return Result<MeshCenterlines>::failure(
          {ErrorCode::invalid_geometry, "Classic bent rebar-mesh offset polygon is invalid."});
    }
    filleted_path = filleted_path_xy(*cross_shape, physical_bend_radius);
    if (!filleted_path) {
      return Result<MeshCenterlines>::failure(
          {ErrorCode::invalid_geometry, "Classic bent rebar-mesh fillet is invalid."});
    }
  }
  const Vector3d distribution_start{geometry[0U], geometry[1U], geometry[2U]};
  const Vector3d distribution_end{geometry[3U], geometry[4U], geometry[5U]};
  const auto vector = subtract(distribution_end, distribution_start);
  const double distribution_length = vector_length(vector);
  if (!std::isfinite(distribution_length) || distribution_length <= 1.0e-9) {
    return Result<MeshCenterlines>::failure(
        {ErrorCode::invalid_geometry, "Classic bent rebar-mesh distribution line has no length."});
  }
  const auto direction = scale(vector, 1.0 / distribution_length);
  auto cross_distances = regular_distances(longitudinal_overhang_left, distribution_length,
                                           cross_spacing, maximum_count);
  if (!cross_distances) return Result<MeshCenterlines>::failure(cross_distances.error());
  const auto remaining = maximum_count - cross_distances.value().size();
  auto longitudinal_distances =
      regular_distances(cross_overhang_left, mesh_width, longitudinal_spacing, remaining);
  if (!longitudinal_distances)
    return Result<MeshCenterlines>::failure(longitudinal_distances.error());
  MeshCenterlines result;
  const Vector3d base{distribution_start.x, distribution_start.y, polygon.front().z};
  result.cross.reserve(cross_distances.value().size());
  for (const auto distance : cross_distances.value()) {
    const auto translation = subtract(add(distribution_start, scale(direction, distance)), base);
    result.cross.emplace_back();
    result.cross.back().reserve(cross_shape->size());
    for (const auto point : *cross_shape) result.cross.back().push_back(add(point, translation));
  }
  const double separation = longitudinal_radius + cross_radius;
  result.longitudinal.reserve(longitudinal_distances.value().size());
  for (const auto distance : longitudinal_distances.value()) {
    const auto sampled = filleted_path_point_xy(*cross_shape, *filleted_path, distance);
    if (!sampled) {
      return Result<MeshCenterlines>::failure(
          {ErrorCode::invalid_geometry, "Classic bent rebar-mesh fillet is invalid."});
    }
    const std::array<double, 2> interior_normal{-sampled->tangent[1] * turn_sign,
                                                sampled->tangent[0] * turn_sign};
    const Vector3d layered{sampled->point.x + interior_normal[0] * separation,
                           sampled->point.y + interior_normal[1] * separation, sampled->point.z};
    const auto start = add(layered, subtract(distribution_start, base));
    result.longitudinal.push_back({start, add(start, vector)});
  }
  return Result<MeshCenterlines>::success(std::move(result));
}

void append_prism(MeshData& mesh, Vector3d start, Vector3d end, Vector3d first_axis,
                  Vector3d second_axis, double radius, std::size_t facets) {
  const auto axis = normalized(subtract(end, start));
  if (!axis || radius <= 0.0 || facets < 3U) return;
  const auto first = normalized(first_axis);
  const auto second = normalized(second_axis);
  if (!first || !second) return;
  const auto base = static_cast<std::uint32_t>(mesh.positions.size() / 3U);
  for (const auto center : {start, end}) {
    for (std::size_t index = 0; index < facets; ++index) {
      const double angle =
          static_cast<double>(index) * 2.0 * std::numbers::pi / static_cast<double>(facets);
      const auto point =
          add(center,
              scale(add(scale(*first, std::cos(angle)), scale(*second, std::sin(angle))), radius));
      mesh.positions.insert(
          mesh.positions.end(),
          {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)});
    }
  }
  const auto first_center = static_cast<std::uint32_t>(mesh.positions.size() / 3U);
  mesh.positions.insert(
      mesh.positions.end(),
      {static_cast<float>(start.x), static_cast<float>(start.y), static_cast<float>(start.z)});
  const auto second_center = static_cast<std::uint32_t>(mesh.positions.size() / 3U);
  mesh.positions.insert(mesh.positions.end(), {static_cast<float>(end.x), static_cast<float>(end.y),
                                               static_cast<float>(end.z)});
  for (std::uint32_t index = 0; index < facets; ++index) {
    const auto next = (index + 1U) % static_cast<std::uint32_t>(facets);
    mesh.indices.insert(
        mesh.indices.end(),
        {base + index, base + next, base + static_cast<std::uint32_t>(facets) + next, base + index,
         base + static_cast<std::uint32_t>(facets) + next,
         base + static_cast<std::uint32_t>(facets) + index, first_center, base + next, base + index,
         second_center, base + static_cast<std::uint32_t>(facets) + index,
         base + static_cast<std::uint32_t>(facets) + next});
  }
}

void append_cylinder(MeshData& mesh, Vector3d start, Vector3d end, double radius,
                     std::size_t facets = 12U) {
  const auto axis = normalized(subtract(end, start));
  if (!axis) return;
  Vector3d trial{0.0, 0.0, 1.0};
  if (std::abs(axis->z) > 0.9) trial = {0.0, 1.0, 0.0};
  const auto first = normalized(cross(*axis, trial));
  if (!first) return;
  append_prism(mesh, start, end, *first, cross(*axis, *first), radius, facets);
}

class WeldPolygonValueCursor {
 public:
  explicit WeldPolygonValueCursor(std::span<const WeldPolygonRow> rows) : rows_(rows) {}

  [[nodiscard]] std::optional<Vector3d> next() noexcept {
    while (row_ < rows_.size() && value_ == rows_[row_].values.size()) {
      ++row_;
      value_ = 0U;
    }
    if (row_ == rows_.size()) return std::nullopt;
    return rows_[row_].values[value_++];
  }

 private:
  std::span<const WeldPolygonRow> rows_;
  std::size_t row_ = 0U;
  std::size_t value_ = 0U;
};

struct WeldFilletSweepPlan {
  std::size_t segment_count = 0U;
  std::size_t position_growth = 0U;
  std::size_t index_growth = 0U;
  std::uint32_t first_vertex = 0U;
};

struct WeldFilletFrame {
  Vector3d first;
  Vector3d second;
};

// Returns the first vertex index for an indexed-mesh append when both position
// arrays are complete XYZ triples and the aggregate remains uint32-addressable.
[[nodiscard]] constexpr std::optional<std::uint32_t> checked_indexed_mesh_append_base(
    std::size_t existing_position_count, std::size_t appended_position_count) noexcept {
  if (existing_position_count % 3U != 0U || appended_position_count % 3U != 0U) {
    return std::nullopt;
  }
  const auto existing_vertices = existing_position_count / 3U;
  const auto appended_vertices = appended_position_count / 3U;
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  if (existing_vertices > maximum || appended_vertices > maximum - existing_vertices) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(existing_vertices);
}

constexpr auto kMaximumIndexedVertex = std::numeric_limits<std::uint32_t>::max();
static_assert(checked_indexed_mesh_append_base(12U, 9U) == 4U);
static_assert(
    checked_indexed_mesh_append_base(static_cast<std::size_t>(kMaximumIndexedVertex - 1U) * 3U,
                                     3U) == kMaximumIndexedVertex - 1U);
static_assert(!checked_indexed_mesh_append_base(
    static_cast<std::size_t>(kMaximumIndexedVertex - 1U) * 3U, 9U));
static_assert(!checked_indexed_mesh_append_base(4U, 3U));

[[nodiscard]] std::optional<WeldFilletFrame> weld_fillet_frame(Vector3d first_value,
                                                               Vector3d second_value,
                                                               Vector3d start,
                                                               Vector3d end) noexcept {
  const auto tangent = normalized(subtract(end, start));
  if (!tangent) return std::nullopt;
  const auto project_leg = [&](Vector3d value) -> std::optional<Vector3d> {
    // Persisted directions may carry small tangent noise, but a leg must still have at least a
    // one-microradian angular separation from the path. This is above float-direction resolution
    // while preserving the observed near-perpendicular frames after projection.
    constexpr double minimum_sine_to_tangent = 1.0e-6;
    const double source_length = vector_length(value);
    if (!std::isfinite(source_length) || source_length <= 1.0e-12) return std::nullopt;
    const auto projected = subtract(value, scale(*tangent, dot(value, *tangent)));
    const double projected_length = vector_length(projected);
    if (!std::isfinite(projected_length) ||
        projected_length <= source_length * minimum_sine_to_tangent) {
      return std::nullopt;
    }
    return scale(projected, 1.0 / projected_length);
  };
  auto first = project_leg(first_value);
  auto second = project_leg(second_value);
  if (!first || !second || std::abs(vector_length(cross(*first, *second)) - 1.0) > 1.0e-6) {
    return std::nullopt;
  }
  if (dot(cross(*first, *second), *tangent) < 0.0) std::swap(first, second);
  return WeldFilletFrame{*first, *second};
}

[[nodiscard]] std::optional<std::array<Vector3d, 3>> model_weld_ring(
    Vector3d origin, const WeldFilletFrame& frame, double size, const CoordinateSystem& system,
    const Axes& system_axes) noexcept {
  const std::array<Vector3d, 3> ring{
      origin,
      add(origin, scale(frame.first, size)),
      add(origin, scale(frame.second, size)),
  };
  std::array<Vector3d, 3> model_ring;
  for (std::size_t index = 0U; index < ring.size(); ++index) {
    const auto local = ring[index];
    const auto model = transform(local, system, system_axes);
    if (!std::isfinite(model.x) || !std::isfinite(model.y) || !std::isfinite(model.z) ||
        std::abs(model.x) > std::numeric_limits<float>::max() ||
        std::abs(model.y) > std::numeric_limits<float>::max() ||
        std::abs(model.z) > std::numeric_limits<float>::max()) {
      return std::nullopt;
    }
    // MeshData stores model positions as floats, so topology must be validated after the same
    // quantization. Large model origins can otherwise collapse valid double-space features.
    model_ring[index] = {
        static_cast<double>(static_cast<float>(model.x)),
        static_cast<double>(static_cast<float>(model.y)),
        static_cast<double>(static_cast<float>(model.z)),
    };
  }
  return model_ring;
}

[[nodiscard]] bool valid_model_triangle(Vector3d first, Vector3d second, Vector3d third) noexcept {
  const auto first_edge = subtract(second, first);
  const auto second_edge = subtract(third, first);
  const double first_length = vector_length(first_edge);
  const double second_length = vector_length(second_edge);
  if (!std::isfinite(first_length) || !std::isfinite(second_length) || first_length <= 1.0e-12 ||
      second_length <= 1.0e-12) {
    return false;
  }
  const double doubled_area = vector_length(cross(first_edge, second_edge));
  return std::isfinite(doubled_area) && doubled_area > first_length * second_length * 1.0e-9;
}

[[nodiscard]] bool valid_model_ring(const std::array<Vector3d, 3>& ring) noexcept {
  return valid_model_triangle(ring[0], ring[1], ring[2]);
}

[[nodiscard]] bool valid_weld_sides(const std::array<Vector3d, 3>& first_ring,
                                    const std::array<Vector3d, 3>& second_ring) noexcept {
  for (std::size_t side = 0U; side < first_ring.size(); ++side) {
    const auto adjacent = (side + 1U) % first_ring.size();
    if (!valid_model_triangle(first_ring[side], first_ring[adjacent], second_ring[adjacent]) ||
        !valid_model_triangle(first_ring[side], second_ring[adjacent], second_ring[side])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<WeldFilletSweepPlan> plan_weld_fillet_sweep(
    const MeshData& mesh, std::span<const WeldPolygonRow> rows, double size,
    const CoordinateSystem& system, const Axes& system_axes) {
  if (!std::isfinite(size) || size <= 0.0) return std::nullopt;
  std::size_t value_count = 0U;
  for (const auto& row : rows) {
    if (row.values.size() > std::numeric_limits<std::size_t>::max() - value_count) {
      return std::nullopt;
    }
    value_count += row.values.size();
  }
  if (value_count < 4U || (value_count - 1U) % 3U != 0U) return std::nullopt;
  const auto segment_count = (value_count - 1U) / 3U;
  if (segment_count == 0U || segment_count > (std::numeric_limits<std::size_t>::max() / 9U) - 1U ||
      segment_count > (std::numeric_limits<std::size_t>::max() - 6U) / 18U) {
    return std::nullopt;
  }
  const auto position_growth = (segment_count + 1U) * 9U;
  const auto index_growth = segment_count * 18U + 6U;
  if (position_growth > mesh.positions.max_size() - mesh.positions.size() ||
      index_growth > mesh.indices.max_size() - mesh.indices.size()) {
    return std::nullopt;
  }
  const auto first_vertex =
      checked_indexed_mesh_append_base(mesh.positions.size(), position_growth);
  if (!first_vertex) return std::nullopt;

  WeldPolygonValueCursor cursor(rows);
  auto origin = cursor.next();
  if (!origin) return std::nullopt;
  std::optional<std::array<Vector3d, 3>> previous_ring;
  for (std::size_t segment = 0U; segment < segment_count; ++segment) {
    const auto first = cursor.next();
    const auto second = cursor.next();
    const auto end = cursor.next();
    if (!first || !second || !end) return std::nullopt;
    const auto frame = weld_fillet_frame(*first, *second, *origin, *end);
    if (!frame) return std::nullopt;
    const auto current_ring = model_weld_ring(*origin, *frame, size, system, system_axes);
    if (!current_ring || !valid_model_ring(*current_ring) ||
        (previous_ring && !valid_weld_sides(*previous_ring, *current_ring))) {
      return std::nullopt;
    }
    previous_ring = current_ring;
    if (segment + 1U == segment_count) {
      const auto end_ring = model_weld_ring(*end, *frame, size, system, system_axes);
      if (!end_ring || !valid_model_ring(*end_ring) ||
          !valid_weld_sides(*current_ring, *end_ring)) {
        return std::nullopt;
      }
    }
    origin = end;
  }
  if (cursor.next()) return std::nullopt;
  return WeldFilletSweepPlan{segment_count, position_growth, index_growth, *first_vertex};
}

void append_weld_ring(MeshData& mesh, Vector3d origin, const WeldFilletFrame& frame, double size,
                      const CoordinateSystem& system, const Axes& system_axes) {
  const std::array<Vector3d, 3> ring{
      origin,
      add(origin, scale(frame.first, size)),
      add(origin, scale(frame.second, size)),
  };
  for (const auto local : ring) {
    const auto model = transform(local, system, system_axes);
    mesh.positions.insert(
        mesh.positions.end(),
        {static_cast<float>(model.x), static_cast<float>(model.y), static_cast<float>(model.z)});
  }
}

void append_weld_fillet_sweep(MeshData& mesh, std::span<const WeldPolygonRow> rows, double size,
                              const CoordinateSystem& system, const Axes& system_axes,
                              const WeldFilletSweepPlan& plan) {
  mesh.positions.reserve(mesh.positions.size() + plan.position_growth);
  mesh.indices.reserve(mesh.indices.size() + plan.index_growth);

  WeldPolygonValueCursor cursor(rows);
  auto origin = *cursor.next();
  for (std::size_t segment = 0U; segment < plan.segment_count; ++segment) {
    const auto first = *cursor.next();
    const auto second = *cursor.next();
    const auto end = *cursor.next();
    const auto frame = *weld_fillet_frame(first, second, origin, end);
    append_weld_ring(mesh, origin, frame, size, system, system_axes);
    if (segment + 1U == plan.segment_count) {
      append_weld_ring(mesh, end, frame, size, system, system_axes);
    }
    origin = end;
  }
  for (std::size_t segment = 0U; segment < plan.segment_count; ++segment) {
    const auto current = plan.first_vertex + static_cast<std::uint32_t>(segment * 3U);
    const auto next = current + 3U;
    for (std::uint32_t side = 0U; side < 3U; ++side) {
      const auto adjacent = (side + 1U) % 3U;
      mesh.indices.insert(mesh.indices.end(), {current + side, current + adjacent, next + adjacent,
                                               current + side, next + adjacent, next + side});
    }
  }
  const auto last = plan.first_vertex + static_cast<std::uint32_t>(plan.segment_count * 3U);
  mesh.indices.insert(mesh.indices.end(), {plan.first_vertex, plan.first_vertex + 2U,
                                           plan.first_vertex + 1U, last, last + 1U, last + 2U});
}

class NonPartReader final : public BatchReader {
 public:
  NonPartReader(std::vector<CurveData> curves, std::vector<MeshData> meshes,
                std::vector<DiagnosticData> diagnostics, std::size_t batch_size)
      : curve_data_(std::move(curves)),
        mesh_data_(std::move(meshes)),
        diagnostic_data_(std::move(diagnostics)),
        batch_size_(batch_size) {}

  Result<BatchView> next() override {
    curves_.clear();
    meshes_.clear();
    diagnostics_.clear();
    if (curve_index_ < curve_data_.size()) {
      const auto count = std::min(batch_size_, curve_data_.size() - curve_index_);
      curves_.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        const auto& curve = curve_data_[curve_index_ + index];
        curves_.push_back({curve.object_id, curve.kind, curve.points, curve.radius});
      }
      curve_index_ += count;
      return Result<BatchView>::success(BatchView{.kind = BatchKind::curves, .curves = curves_});
    }
    if (mesh_index_ < mesh_data_.size()) {
      const auto count = std::min(batch_size_, mesh_data_.size() - mesh_index_);
      meshes_.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        const auto& mesh = mesh_data_[mesh_index_ + index];
        meshes_.push_back(MeshView{
            .object_id = mesh.object_id, .positions = mesh.positions, .indices = mesh.indices});
      }
      mesh_index_ += count;
      return Result<BatchView>::success(BatchView{.kind = BatchKind::meshes, .meshes = meshes_});
    }
    if (diagnostic_index_ < diagnostic_data_.size()) {
      const auto count = std::min(batch_size_, diagnostic_data_.size() - diagnostic_index_);
      diagnostics_.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        const auto& diagnostic = diagnostic_data_[diagnostic_index_ + index];
        diagnostics_.push_back({diagnostic.code, diagnostic.object_id, diagnostic.message});
      }
      diagnostic_index_ += count;
      return Result<BatchView>::success(
          BatchView{.kind = BatchKind::diagnostics, .diagnostics = diagnostics_});
    }
    return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
  }

 private:
  std::vector<CurveData> curve_data_;
  std::vector<MeshData> mesh_data_;
  std::vector<DiagnosticData> diagnostic_data_;
  std::vector<CurveView> curves_;
  std::vector<MeshView> meshes_;
  std::vector<Diagnostic> diagnostics_;
  std::size_t batch_size_ = 256U;
  std::size_t curve_index_ = 0U;
  std::size_t mesh_index_ = 0U;
  std::size_t diagnostic_index_ = 0U;
};

}  // namespace

std::optional<FastenerDimensions> known_fastener_dimensions(std::string_view standard,
                                                            double diameter) noexcept {
  const auto first = standard.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::nullopt;
  const auto last = standard.find_last_not_of(" \t\r\n");
  standard = standard.substr(first, last - first + 1U);
  struct Entry {
    std::string_view standard;
    double diameter;
    FastenerDimensions dimensions;
  };
  constexpr std::array<Entry, 10> entries{{
      {"7990", 12.0, {19.0, 8.0}},
      {"7990", 16.0, {24.0, 10.0}},
      {"7990", 20.0, {30.0, 13.0}},
      {"7990-434-7989-5.6", 16.0, {24.0, 10.0}},
      {"EN-14399-4", 16.0, {27.0, 10.0}},
      {"EN-14399-4", 20.0, {32.0, 13.0}},
      {"FISCHER FAZ", 20.0, {27.0, 13.2}},
      {"FAZIIPLUS", 12.0, {19.0, 12.0}},
      {"FHII-S", 10.0, {10.0, 5.5}},
      {"SGKA_M", 12.0, {19.0, 7.5}},
  }};
  for (const auto& entry : entries) {
    if (standard == entry.standard && std::abs(diameter - entry.diameter) <= 1.0e-6)
      return entry.dimensions;
  }
  return std::nullopt;
}

Result<std::vector<std::vector<Vector3d>>> evaluate_tapered_straight_group_centerlines(
    std::array<Vector3d, 2> start_segment, std::array<Vector3d, 2> end_segment,
    Vector3d distribution_start, Vector3d distribution_end, double start_from_plane_offset,
    double end_from_plane_offset, std::uint32_t spacing_type, std::span<const double> spacings,
    std::uint32_t exclude_type, std::size_t maximum_count) {
  const auto vector = subtract(distribution_end, distribution_start);
  const double distribution_length = vector_length(vector);
  if (!std::isfinite(distribution_length) || distribution_length <= 1.0e-9) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Tapered rebar-group distribution line has no length."});
  }
  if (start_from_plane_offset < 0.0 || end_from_plane_offset < 0.0) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Tapered rebar-group plane offsets are negative."});
  }
  double usable_length = distribution_length - start_from_plane_offset - end_from_plane_offset;
  if (usable_length < -1.0e-7) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry,
         "Tapered rebar-group plane offsets exceed the distribution length."});
  }
  usable_length = std::max(0.0, usable_length);
  auto distances = spacing_distances(usable_length, spacing_type, spacings, maximum_count);
  if (!distances) return Result<std::vector<std::vector<Vector3d>>>::failure(distances.error());
  if (exclude_type < 1U || exclude_type > 4U) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::decoder_unavailable, "Tapered rebar-group exclusion type is unsupported."});
  }
  if ((exclude_type == 2U || exclude_type == 4U) && !distances.value().empty())
    distances.value().erase(distances.value().begin());
  if ((exclude_type == 3U || exclude_type == 4U) && !distances.value().empty())
    distances.value().pop_back();

  std::vector<std::vector<Vector3d>> result;
  result.reserve(distances.value().size());
  for (const double distance : distances.value()) {
    const double fraction =
        std::clamp((start_from_plane_offset + distance) / distribution_length, 0.0, 1.0);
    result.push_back({
        add(start_segment[0], scale(subtract(end_segment[0], start_segment[0]), fraction)),
        add(start_segment[1], scale(subtract(end_segment[1], start_segment[1]), fraction)),
    });
  }
  return Result<std::vector<std::vector<Vector3d>>>::success(std::move(result));
}

Result<std::vector<std::vector<Vector3d>>> evaluate_nonplanar_group_reference_centerlines(
    std::span<const Vector3d> polygon, Vector3d distribution_start, Vector3d distribution_end,
    double on_plane_offset, double start_from_plane_offset, double end_from_plane_offset,
    double start_point_offset, double end_point_offset, std::uint32_t spacing_type,
    std::span<const double> spacings, std::uint32_t exclude_type, bool align_distribution_anchor,
    std::size_t maximum_count) {
  if (polygon.size() < 2U) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Non-planar rebar-group path has fewer than two points."});
  }
  const auto vector = subtract(distribution_end, distribution_start);
  const double distribution_length = vector_length(vector);
  if (!std::isfinite(distribution_length) || distribution_length <= 1.0e-9) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Non-planar rebar-group distribution line has no length."});
  }
  const auto direction = scale(vector, 1.0 / distribution_length);
  double usable_length = distribution_length - start_from_plane_offset - end_from_plane_offset;
  if (usable_length < -1.0e-7) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry,
         "Non-planar rebar-group plane offsets exceed the distribution length."});
  }
  usable_length = std::max(0.0, usable_length);

  Vector3d cross_plane_translation{};
  if (align_distribution_anchor) {
    const auto anchor_delta = subtract(distribution_start, polygon[1]);
    const double along_anchor =
        anchor_delta.x * direction.x + anchor_delta.y * direction.y + anchor_delta.z * direction.z;
    cross_plane_translation = subtract(anchor_delta, scale(direction, along_anchor));
  }
  std::vector<Vector3d> base_path;
  base_path.reserve(polygon.size());
  for (std::size_t point_index = 0; point_index < polygon.size(); ++point_index) {
    auto shifted = add(polygon[point_index], cross_plane_translation);
    if (point_index + 1U < polygon.size()) {
      std::optional<Vector3d> side;
      for (std::size_t next_index = point_index + 1U; next_index < polygon.size(); ++next_index) {
        const auto segment = subtract(polygon[next_index], polygon[point_index]);
        const auto unit_segment = normalized(segment);
        if (!unit_segment) continue;
        const auto candidate = normalized(cross(*unit_segment, direction));
        if (candidate) {
          side = candidate;
          break;
        }
      }
      if (side) shifted = add(shifted, scale(*side, on_plane_offset));
    }
    base_path.push_back(shifted);
  }
  const auto first_direction = normalized(subtract(polygon[1], polygon[0]));
  const auto last_direction = normalized(subtract(polygon.back(), polygon[polygon.size() - 2U]));
  if (!first_direction || !last_direction) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::invalid_geometry, "Non-planar rebar-group has a zero-length end leg."});
  }
  base_path.front() = add(base_path.front(), scale(*first_direction, start_point_offset));
  base_path.back() = subtract(base_path.back(), scale(*last_direction, end_point_offset));

  auto distances = spacing_distances(usable_length, spacing_type, spacings, maximum_count);
  if (!distances) return Result<std::vector<std::vector<Vector3d>>>::failure(distances.error());
  if (exclude_type < 1U || exclude_type > 4U) {
    return Result<std::vector<std::vector<Vector3d>>>::failure(
        {ErrorCode::decoder_unavailable, "Non-planar rebar-group exclusion type is unsupported."});
  }
  if ((exclude_type == 2U || exclude_type == 4U) && !distances.value().empty())
    distances.value().erase(distances.value().begin());
  if ((exclude_type == 3U || exclude_type == 4U) && !distances.value().empty())
    distances.value().pop_back();

  std::vector<std::vector<Vector3d>> result;
  result.reserve(distances.value().size());
  for (const double distance : distances.value()) {
    result.emplace_back();
    result.back().reserve(base_path.size());
    const auto translation = scale(direction, start_from_plane_offset + distance);
    for (const auto point : base_path) result.back().push_back(add(point, translation));
  }
  return Result<std::vector<std::vector<Vector3d>>>::success(std::move(result));
}

Result<ProcessStream> make_nonpart_geometry_stream(std::shared_ptr<const ModelStorage> storage,
                                                   const Schema& schema,
                                                   const ProcessRequest& request) {
  std::vector<CurveData> curves;
  std::vector<MeshData> meshes;
  std::vector<DiagnosticData> diagnostics;
  const auto batch_size = request.batch_memory_budget_bytes == 0U
                              ? 256U
                              : static_cast<std::size_t>(std::clamp<std::uint64_t>(
                                    request.batch_memory_budget_bytes / 4096U, 1U, 4096U));
  RetainedGeometryBudget geometry_budget(storage->payload.bytes().size(),
                                         request.geometry_memory_budget_bytes);
  const auto* objects = populated_table(
      *storage, schema, std::array<std::string_view, 2>{"object", "old_object_948"});
  if (objects == nullptr) {
    if (has_legacy_nonpart_rows(*storage, schema)) {
      diagnostics.push_back({ErrorCode::decoder_unavailable, 0U,
                             "Non-part DISPLAY geometry requires the modern object table; "
                             "populated legacy non-part tables remain unsupported."});
    }
    return Result<ProcessStream>::success(std::make_unique<NonPartReader>(
        std::move(curves), std::move(meshes), std::move(diagnostics), batch_size));
  }
  const bool legacy_objects = objects->name == "old_object_948";
  const auto* object_id = find_field(schema, *objects, "id");
  const auto* object_type = find_field(schema, *objects, "type");
  const auto* object_subtype = find_field(schema, *objects, "subtype");
  const auto* object_attribute_id = find_field(schema, *objects, "object_attr_id");
  if (object_id == nullptr ||
      (legacy_objects ? object_attribute_id == nullptr
                      : object_type == nullptr || object_subtype == nullptr)) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The non-part object identity layout is incomplete."});
  }
  std::unordered_map<std::uint32_t, ObjectType> legacy_object_attributes;
  if (legacy_objects) {
    const auto* attributes = populated_table(
        *storage, schema,
        std::array<std::string_view, 4>{"old_object_attr_951", "old_object_attr_915",
                                        "old_object_attr_900", "old_object_attr_879"});
    if (attributes == nullptr) {
      return Result<ProcessStream>::failure(
          {ErrorCode::schema_mismatch, "The legacy object attribute table is unavailable."});
    }
    const auto* id = find_field(schema, *attributes, "id");
    const auto* type = find_field(schema, *attributes, "type");
    const auto* subtype = find_field(schema, *attributes, "subtype");
    if (id == nullptr || type == nullptr || subtype == nullptr) {
      return Result<ProcessStream>::failure(
          {ErrorCode::schema_mismatch, "The legacy object attribute layout is incomplete."});
    }
    const auto& layout = storage->layout.tables[attributes->ordinal];
    legacy_object_attributes.reserve(static_cast<std::size_t>(layout.info.row_count));
    for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
      const auto record = layout.record(storage->payload.bytes(), row);
      if (hidden(record)) continue;
      const auto tuple = record.subspan(1U, attributes->tuple_size);
      legacy_object_attributes.insert_or_assign(
          read_u32(tuple, id->offset),
          ObjectType{read_u32(tuple, type->offset), read_u32(tuple, subtype->offset)});
    }
  }
  std::unordered_map<std::uint32_t, ObjectType> object_types;
  const auto& object_layout = storage->layout.tables[objects->ordinal];
  object_types.reserve(static_cast<std::size_t>(object_layout.info.row_count));
  for (std::uint64_t row = 0; row < object_layout.info.row_count; ++row) {
    const auto record = object_layout.record(storage->payload.bytes(), row);
    if (hidden(record)) continue;
    const auto tuple = record.subspan(1U, objects->tuple_size);
    if (legacy_objects) {
      const auto attribute =
          legacy_object_attributes.find(read_u32(tuple, object_attribute_id->offset));
      if (attribute != legacy_object_attributes.end()) {
        object_types.insert_or_assign(read_u32(tuple, object_id->offset), attribute->second);
      }
    } else {
      object_types.insert_or_assign(read_u32(tuple, object_id->offset),
                                    ObjectType{read_u32(tuple, object_type->offset),
                                               read_u32(tuple, object_subtype->offset)});
    }
  }

  std::unordered_map<std::uint32_t, Axes> axes;
  const auto* axes_table = schema.find_table("coordsys_attr");
  if (axes_table != nullptr) {
    const auto* id = find_field(schema, *axes_table, "id");
    const auto* xx = find_field(schema, *axes_table, "xdir_x");
    const auto* xy = find_field(schema, *axes_table, "xdir_y");
    const auto* xz = find_field(schema, *axes_table, "xdir_z");
    const auto* yx = find_field(schema, *axes_table, "ydir_x");
    const auto* yy = find_field(schema, *axes_table, "ydir_y");
    const auto* yz = find_field(schema, *axes_table, "ydir_z");
    if (id != nullptr && xx != nullptr && xy != nullptr && xz != nullptr && yx != nullptr &&
        yy != nullptr && yz != nullptr) {
      const auto& layout = storage->layout.tables[axes_table->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, axes_table->tuple_size);
        axes.insert_or_assign(read_u32(tuple, id->offset),
                              Axes{{scalar(tuple, *xx), scalar(tuple, *xy), scalar(tuple, *xz)},
                                   {scalar(tuple, *yx), scalar(tuple, *yy), scalar(tuple, *yz)}});
      }
    }
  }
  std::unordered_map<std::uint32_t, CoordinateSystem> systems;
  const auto* systems_table = schema.find_table("coordsys");
  if (systems_table != nullptr) {
    const auto* id = find_field(schema, *systems_table, "id");
    const auto* axes_id = find_field(schema, *systems_table, "csys_attr_id");
    const auto* x = find_field(schema, *systems_table, "x1");
    const auto* y = find_field(schema, *systems_table, "y1");
    const auto* z = find_field(schema, *systems_table, "z1");
    const auto* length = find_field(schema, *systems_table, "length");
    if (id != nullptr && axes_id != nullptr && x != nullptr && y != nullptr && z != nullptr) {
      const auto& layout = storage->layout.tables[systems_table->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, systems_table->tuple_size);
        systems.insert_or_assign(
            read_u32(tuple, id->offset),
            CoordinateSystem{read_u32(tuple, axes_id->offset),
                             {scalar(tuple, *x), scalar(tuple, *y), scalar(tuple, *z)},
                             length == nullptr ? 0.0 : scalar(tuple, *length)});
      }
    }
  }

  // Modern polygon welds persist a local fillet path as repeating
  // [position, first-leg direction, second-leg direction] values followed by
  // the final position. Relation 40001 owns each polygon group from the weld
  // object; polygon types 1 and 2 select its above and below seam records.
  std::unordered_map<std::uint32_t, WeldDefinition> weld_definitions;
  if (const auto* welding =
          populated_table(*storage, schema, std::array<std::string_view, 1>{"welding"})) {
    const auto* id = find_field(schema, *welding, "id");
    const auto* common = find_field(schema, *welding, "weld_common_attr_id");
    const auto* above = find_field(schema, *welding, "weld_seam1_id");
    const auto* below = find_field(schema, *welding, "weld_seam2_id");
    if (id != nullptr && common != nullptr && above != nullptr && below != nullptr) {
      const auto& layout = storage->layout.tables[welding->ordinal];
      weld_definitions.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, welding->tuple_size);
        weld_definitions.insert_or_assign(
            read_u32(tuple, id->offset),
            WeldDefinition{read_u32(tuple, common->offset), read_u32(tuple, above->offset),
                           read_u32(tuple, below->offset)});
      }
    }
  }
  std::unordered_map<std::uint32_t, WeldCommonGeometry> weld_common_geometry;
  if (const auto* common = populated_table(
          *storage, schema, std::array<std::string_view, 1>{"welding_common_attr"})) {
    const auto* id = find_field(schema, *common, "id");
    const auto* compound = find_field(schema, *common, "compound_weld");
    const auto* logical = find_field(schema, *common, "logical_weld");
    if (id != nullptr && compound != nullptr && logical != nullptr) {
      const auto& layout = storage->layout.tables[common->ordinal];
      weld_common_geometry.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, common->tuple_size);
        weld_common_geometry.insert_or_assign(
            read_u32(tuple, id->offset),
            WeldCommonGeometry{read_u32(tuple, compound->offset) != 0U,
                               read_u32(tuple, logical->offset) != 0U});
      }
    }
  }
  std::unordered_map<std::uint32_t, WeldSeamGeometry> weld_seam_geometry;
  if (const auto* seams =
          populated_table(*storage, schema, std::array<std::string_view, 1>{"welding_attr"})) {
    const auto* id = find_field(schema, *seams, "id");
    const auto* size = find_field(schema, *seams, "size");
    const auto* type = find_field(schema, *seams, "type");
    if (id != nullptr && size != nullptr && type != nullptr) {
      const auto& layout = storage->layout.tables[seams->ordinal];
      weld_seam_geometry.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, seams->tuple_size);
        weld_seam_geometry.insert_or_assign(
            read_u32(tuple, id->offset),
            WeldSeamGeometry{scalar(tuple, *size), read_u32(tuple, type->offset)});
      }
    }
  }
  std::unordered_map<std::uint32_t, std::uint32_t> weld_polygon_owners;
  if (const auto* relations = schema.find_table("relation")) {
    const auto* type = find_field(schema, *relations, "type");
    const auto* source = find_field(schema, *relations, "id1");
    const auto* target = find_field(schema, *relations, "id2");
    if (type != nullptr && source != nullptr && target != nullptr) {
      const auto& layout = storage->layout.tables[relations->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, relations->tuple_size);
        if (read_u32(tuple, type->offset) != 40'001U) continue;
        const auto weld_id = read_u32(tuple, source->offset);
        const auto object = object_types.find(weld_id);
        if (weld_id < request.geometry_object_id_min || weld_id > request.geometry_object_id_max ||
            object == object_types.end() || object->second.type != 13U ||
            !weld_definitions.contains(weld_id))
          continue;
        weld_polygon_owners.insert_or_assign(read_u32(tuple, target->offset), weld_id);
      }
    }
  }
  std::unordered_map<std::uint32_t, std::vector<WeldPolygonRow>> weld_polygon_rows;
  std::unordered_set<std::uint32_t> invalid_weld_polygons;
  std::unordered_set<std::uint32_t> limited_weld_polygons;
  if (const auto* polygons =
          populated_table(*storage, schema, std::array<std::string_view, 1>{"weldingpolygon"})) {
    const auto* id = find_field(schema, *polygons, "id");
    const auto* number = find_field(schema, *polygons, "no");
    const auto* count = find_field(schema, *polygons, "number_of_points_in_row");
    const auto* type = find_field(schema, *polygons, "type");
    if (id != nullptr && number != nullptr && count != nullptr && type != nullptr) {
      const auto& layout = storage->layout.tables[polygons->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, polygons->tuple_size);
        const auto polygon_id = read_u32(tuple, id->offset);
        const auto owner = weld_polygon_owners.find(polygon_id);
        if (owner == weld_polygon_owners.end()) continue;
        const auto polygon_type = read_u32(tuple, type->offset);
        const auto weld = weld_definitions.find(owner->second);
        if (weld == weld_definitions.end()) continue;
        const auto common = weld_common_geometry.find(weld->second.common_attribute_id);
        if (common == weld_common_geometry.end() || common->second.compound ||
            common->second.logical)
          continue;
        const auto seam_id = polygon_type == 1U   ? weld->second.above_attribute_id
                             : polygon_type == 2U ? weld->second.below_attribute_id
                                                  : 0U;
        const auto seam = weld_seam_geometry.find(seam_id);
        if (seam == weld_seam_geometry.end() || seam->second.type != 10U ||
            !std::isfinite(seam->second.size) || seam->second.size <= 0.0)
          continue;
        const auto point_count = static_cast<std::size_t>(read_u32(tuple, count->offset));
        if (point_count == 0U || point_count > 10U) {
          invalid_weld_polygons.insert(polygon_id);
          continue;
        }
        const auto retained_bytes = sizeof(WeldPolygonRow) + point_count * sizeof(Vector3d);
        if (!geometry_budget.consume_decode_bytes(retained_bytes)) {
          limited_weld_polygons.insert(polygon_id);
          continue;
        }
        WeldPolygonRow decoded{.number = read_u32(tuple, number->offset), .type = polygon_type};
        decoded.values.reserve(point_count);
        bool complete = true;
        for (std::size_t index = 1U; index <= point_count; ++index) {
          const auto suffix = std::to_string(index);
          const auto* x = find_field(schema, *polygons, "x" + suffix);
          const auto* y = find_field(schema, *polygons, "y" + suffix);
          const auto* z = find_field(schema, *polygons, "z" + suffix);
          if (x == nullptr || y == nullptr || z == nullptr) {
            complete = false;
            break;
          }
          decoded.values.push_back({scalar(tuple, *x), scalar(tuple, *y), scalar(tuple, *z)});
        }
        if (!complete) {
          invalid_weld_polygons.insert(polygon_id);
          continue;
        }
        weld_polygon_rows[polygon_id].push_back(std::move(decoded));
      }
    }
  }
  std::unordered_map<std::uint32_t, std::size_t> weld_mesh_indices;
  for (auto& [polygon_id, rows] : weld_polygon_rows) {
    const auto owner = weld_polygon_owners.find(polygon_id);
    if (owner == weld_polygon_owners.end()) continue;
    if (limited_weld_polygons.contains(polygon_id)) {
      diagnostics.push_back({ErrorCode::resource_limit, owner->second,
                             "Polygon-weld rows exceed the aggregate geometry memory budget."});
      continue;
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.number < rhs.number; });
    const auto polygon_type = rows.empty() ? 0U : rows.front().type;
    bool valid = !rows.empty();
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      valid = valid && rows[index].number == index && rows[index].type == polygon_type;
    }
    if (invalid_weld_polygons.contains(polygon_id) || !valid) {
      diagnostics.push_back({ErrorCode::invalid_geometry, owner->second,
                             "Persisted polygon-weld rows are incomplete or out of sequence."});
      continue;
    }
    const auto weld = weld_definitions.find(owner->second);
    const auto seam_id =
        polygon_type == 1U ? weld->second.above_attribute_id : weld->second.below_attribute_id;
    const auto seam = weld_seam_geometry.find(seam_id);
    const auto system = systems.find(owner->second);
    if (seam == weld_seam_geometry.end() || system == systems.end() ||
        !axes.contains(system->second.axes_id)) {
      diagnostics.push_back({ErrorCode::decoder_unavailable, owner->second,
                             "Polygon-weld seam or coordinate frame is unavailable."});
      continue;
    }
    const auto found = weld_mesh_indices.find(owner->second);
    const MeshData empty{.object_id = owner->second};
    const auto& retained = found == weld_mesh_indices.end() ? empty : meshes[found->second];
    const auto plan = plan_weld_fillet_sweep(retained, rows, seam->second.size, system->second,
                                             axes.at(system->second.axes_id));
    if (!plan) {
      diagnostics.push_back({ErrorCode::invalid_geometry, owner->second,
                             "Persisted polygon-weld path or frame is invalid."});
      continue;
    }
    if (!geometry_budget.consume_mesh(plan->position_growth, plan->index_growth,
                                      found == weld_mesh_indices.end())) {
      diagnostics.push_back({ErrorCode::resource_limit, owner->second,
                             "Polygon-weld mesh exceeds the aggregate geometry memory budget."});
      continue;
    }
    std::size_t mesh_index = 0U;
    if (found == weld_mesh_indices.end()) {
      mesh_index = meshes.size();
      weld_mesh_indices.emplace(owner->second, mesh_index);
      meshes.push_back(empty);
    } else {
      mesh_index = found->second;
    }
    append_weld_fillet_sweep(meshes[mesh_index], rows, seam->second.size, system->second,
                             axes.at(system->second.axes_id), *plan);
  }
  for (const auto polygon_id : invalid_weld_polygons) {
    if (weld_polygon_rows.contains(polygon_id)) continue;
    const auto owner = weld_polygon_owners.find(polygon_id);
    if (owner != weld_polygon_owners.end()) {
      diagnostics.push_back({ErrorCode::invalid_geometry, owner->second,
                             "Persisted polygon-weld row has an invalid point count."});
    }
  }
  for (const auto polygon_id : limited_weld_polygons) {
    if (weld_polygon_rows.contains(polygon_id)) continue;
    const auto owner = weld_polygon_owners.find(polygon_id);
    if (owner != weld_polygon_owners.end()) {
      diagnostics.push_back({ErrorCode::resource_limit, owner->second,
                             "Polygon-weld rows exceed the aggregate geometry memory budget."});
    }
  }

  // Grids are the intersections of child vertical grid planes with the
  // persisted parent grid elevation.
  if (const auto* relations = schema.find_table("relation")) {
    const auto* type = find_field(schema, *relations, "type");
    const auto* source = find_field(schema, *relations, "id1");
    const auto* target = find_field(schema, *relations, "id2");
    if (type != nullptr && source != nullptr && target != nullptr) {
      const auto& layout = storage->layout.tables[relations->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, relations->tuple_size);
        if (read_u32(tuple, type->offset) != 7U) continue;
        const auto grid_id = read_u32(tuple, source->offset);
        const auto plane_id = read_u32(tuple, target->offset);
        if (grid_id < request.geometry_object_id_min || grid_id > request.geometry_object_id_max)
          continue;
        if (!object_types.contains(grid_id) || object_types[grid_id].type != 7U ||
            object_types[grid_id].subtype != 0U || !object_types.contains(plane_id) ||
            object_types[plane_id].type != 30U || object_types[plane_id].subtype != 3U)
          continue;
        const auto grid = systems.find(grid_id);
        const auto plane = systems.find(plane_id);
        if (grid == systems.end() || plane == systems.end()) continue;
        const auto basis = axes.find(plane->second.axes_id);
        if (basis == axes.end() || std::abs(std::abs(basis->second.y.z) - 1.0) > 1.0e-9 ||
            !std::isfinite(plane->second.length) || plane->second.length <= 0.0)
          continue;
        Vector3d start{plane->second.origin.x, plane->second.origin.y, grid->second.origin.z};
        if (!geometry_budget.consume_curve(2U)) {
          geometry_budget.exhaust();
          diagnostics.push_back(
              {ErrorCode::resource_limit, grid_id,
               "Grid display geometry exceeds the aggregate geometry memory budget."});
          continue;
        }
        curves.push_back({grid_id,
                          CurveGeometryKind::line_segment,
                          {start, add(start, scale(basis->second.x, plane->second.length))},
                          0.0});
      }
    }
  }

  std::unordered_map<std::uint32_t, std::string> strings;
  std::unordered_map<std::uint32_t, std::uint32_t> string_next;
  if (const auto* table = schema.find_table("string")) {
    const auto* id = find_field(schema, *table, "id");
    const auto* next = find_field(schema, *table, "next_id");
    const auto* value = find_field(schema, *table, "string");
    if (id != nullptr && next != nullptr && value != nullptr) {
      const auto& layout = storage->layout.tables[table->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, table->tuple_size);
        const auto key = read_u32(tuple, id->offset);
        strings.insert_or_assign(key, std::string(read_text(tuple, value->offset, value->size)));
        string_next.insert_or_assign(key, read_u32(tuple, next->offset));
      }
    }
  }
  const auto doubles = numeric_arrays(
      *storage, schema,
      populated_table(*storage, schema,
                      std::array<std::string_view, 2>{"double_array", "old_double_array_738"}));
  const auto integers = numeric_arrays(*storage, schema, schema.find_table("int_array"));
  std::size_t linked_row_count = strings.size();
  for (const auto count : std::array<std::size_t, 2>{doubles.size(), integers.size()}) {
    linked_row_count = count > std::numeric_limits<std::size_t>::max() - linked_row_count
                           ? std::numeric_limits<std::size_t>::max()
                           : linked_row_count + count;
  }
  LinkedRowVisitBudget linked_visit_budget(linked_row_count, geometry_budget.limit());
  StringDecodeCache string_cache;
  NumericDecodeCache double_cache;
  NumericDecodeCache integer_cache;
  // Expanded group bars are not individually persisted. Keep their count and
  // retained point storage proportional to the source payload even when a
  // count field is corrupt or adversarial.
  const auto maximum_expanded_curve_count =
      std::max<std::size_t>(1U, storage->payload.bytes().size() / 128U);
  std::size_t expanded_curve_count = 0U;
  std::unordered_map<std::uint32_t, RebarAttribute> rebar_attributes;
  if (const auto* table =
          populated_table(*storage, schema, std::array<std::string_view, 1>{"rebar_attr"})) {
    const auto* id = find_field(schema, *table, "id");
    const auto* profile = find_field(schema, *table, "profile_id");
    const auto* bar_type_info = find_field(schema, *table, "bar_type_info_id");
    if (id != nullptr && profile != nullptr) {
      const auto& layout = storage->layout.tables[table->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, table->tuple_size);
        rebar_attributes.insert_or_assign(
            read_u32(tuple, id->offset),
            RebarAttribute{read_u32(tuple, profile->offset),
                           bar_type_info == nullptr ? 0U : read_u32(tuple, bar_type_info->offset)});
      }
    }
  }
  std::unordered_map<std::uint32_t, MeshAttribute> mesh_attributes;
  if (const auto* table =
          populated_table(*storage, schema, std::array<std::string_view, 1>{"mesh_attr"})) {
    const auto* id = find_field(schema, *table, "id");
    const auto* diameters_id = find_field(schema, *table, "diameters_id");
    const auto* spacings_id = find_field(schema, *table, "spacings_id");
    const auto* width = find_field(schema, *table, "width");
    const auto* height = find_field(schema, *table, "height");
    const auto* longitudinal_overhang_left = find_field(schema, *table, "longit_overhang_left");
    const auto* cross_overhang_left = find_field(schema, *table, "cross_overhang_left");
    const auto* flags = find_field(schema, *table, "mesh_flags");
    if (id != nullptr && diameters_id != nullptr && spacings_id != nullptr && width != nullptr &&
        height != nullptr && longitudinal_overhang_left != nullptr &&
        cross_overhang_left != nullptr && flags != nullptr) {
      const auto& layout = storage->layout.tables[table->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, table->tuple_size);
        mesh_attributes.insert_or_assign(
            read_u32(tuple, id->offset),
            MeshAttribute{read_u32(tuple, diameters_id->offset),
                          read_u32(tuple, spacings_id->offset), scalar(tuple, *width),
                          scalar(tuple, *height), scalar(tuple, *longitudinal_overhang_left),
                          scalar(tuple, *cross_overhang_left), read_u32(tuple, flags->offset)});
      }
    }
  }
  if (const auto* rebars =
          populated_table(*storage, schema, std::array<std::string_view, 1>{"rebar"})) {
    const auto* id = find_field(schema, *rebars, "id");
    const auto* attr_id = find_field(schema, *rebars, "rebar_attr_id");
    const auto* mesh_attr_id = find_field(schema, *rebars, "mesh_attr_id");
    const auto* polygon_id = find_field(schema, *rebars, "polygon_id");
    const auto* bending_id = find_field(schema, *rebars, "bending_id");
    const auto* spacing_id = find_field(schema, *rebars, "spacing_id");
    const auto* geometry_id = find_field(schema, *rebars, "offset_id");
    const auto* bar_count = find_field(schema, *rebars, "n_bars");
    if (id != nullptr && attr_id != nullptr && mesh_attr_id != nullptr && polygon_id != nullptr &&
        bending_id != nullptr && spacing_id != nullptr && geometry_id != nullptr &&
        bar_count != nullptr) {
      const auto& layout = storage->layout.tables[rebars->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, rebars->tuple_size);
        const auto rebar_id = read_u32(tuple, id->offset);
        if (rebar_id < request.geometry_object_id_min || rebar_id > request.geometry_object_id_max)
          continue;
        const auto object = object_types.find(rebar_id);
        if (object == object_types.end() || object->second.type != 47U) continue;
        if (!geometry_budget.can_consume_curve(2U)) {
          diagnostics.push_back(
              {ErrorCode::resource_limit, rebar_id,
               "Classic reinforcement exceeds the aggregate geometry memory budget."});
          continue;
        }
        const auto attribute = rebar_attributes.find(read_u32(tuple, attr_id->offset));
        const auto system = systems.find(rebar_id);
        if (attribute == rebar_attributes.end() || system == systems.end() ||
            !axes.contains(system->second.axes_id)) {
          diagnostics.push_back({ErrorCode::decoder_unavailable, rebar_id,
                                 "Classic reinforcement frame or attribute is missing."});
          continue;
        }
        const auto& polygon_values =
            resolve_array(doubles, read_u32(tuple, polygon_id->offset), double_cache,
                          linked_visit_budget, geometry_budget);
        if (polygon_values.status == LinkedDecodeStatus::resource_limit) {
          diagnostics.push_back(
              {ErrorCode::resource_limit, rebar_id,
               "Classic reinforcement linked data exceeds the aggregate geometry memory budget."});
          continue;
        }
        if (polygon_values.status != LinkedDecodeStatus::success ||
            polygon_values.value.size() < 6U || polygon_values.value.size() % 3U != 0U) {
          diagnostics.push_back({ErrorCode::invalid_geometry, rebar_id,
                                 "Classic reinforcement polygon array is invalid."});
          continue;
        }
        std::vector<Vector3d> polygon;
        polygon.reserve(polygon_values.value.size() / 3U);
        for (std::size_t index = 0; index < polygon_values.value.size(); index += 3U)
          polygon.push_back({polygon_values.value[index], polygon_values.value[index + 1U],
                             polygon_values.value[index + 2U]});
        std::vector<std::pair<std::vector<Vector3d>, double>> evaluated_curves;
        if (object->second.subtype == 0U || object->second.subtype == 1U) {
          const auto& profile = resolve_string(strings, string_next, attribute->second.profile_id,
                                               string_cache, linked_visit_budget, geometry_budget);
          if (profile.status == LinkedDecodeStatus::resource_limit) {
            diagnostics.push_back({ErrorCode::resource_limit, rebar_id,
                                   "Classic reinforcement linked data exceeds the aggregate "
                                   "geometry memory budget."});
            continue;
          }
          const auto radius = profile.status == LinkedDecodeStatus::success
                                  ? rebar_radius(profile.value)
                                  : std::nullopt;
          if (!radius) {
            diagnostics.push_back(
                {ErrorCode::invalid_geometry, rebar_id, "Classic reinforcement size is invalid."});
            continue;
          }
          std::vector<std::vector<Vector3d>> centerlines;
          if (object->second.subtype == 0U) {
            if (!geometry_budget.consume_curve(polygon.size())) {
              geometry_budget.exhaust();
              diagnostics.push_back(
                  {ErrorCode::resource_limit, rebar_id,
                   "Classic reinforcement exceeds the aggregate geometry memory budget."});
              continue;
            }
            centerlines.push_back(std::move(polygon));
          } else {
            const auto& actual_type_info =
                resolve_array(integers, attribute->second.bar_type_info_id, integer_cache,
                              linked_visit_budget, geometry_budget);
            const auto& spacings =
                resolve_array(doubles, read_u32(tuple, spacing_id->offset), double_cache,
                              linked_visit_budget, geometry_budget);
            const auto& geometry =
                resolve_array(doubles, read_u32(tuple, geometry_id->offset), double_cache,
                              linked_visit_budget, geometry_budget);
            if (actual_type_info.status == LinkedDecodeStatus::resource_limit ||
                spacings.status == LinkedDecodeStatus::resource_limit ||
                geometry.status == LinkedDecodeStatus::resource_limit) {
              diagnostics.push_back({ErrorCode::resource_limit, rebar_id,
                                     "Classic rebar-group linked data exceeds the aggregate "
                                     "geometry memory budget."});
              continue;
            }
            if (actual_type_info.status != LinkedDecodeStatus::success ||
                actual_type_info.value.size() < 3U ||
                spacings.status != LinkedDecodeStatus::success ||
                geometry.status != LinkedDecodeStatus::success) {
              diagnostics.push_back({ErrorCode::invalid_geometry, rebar_id,
                                     "Classic rebar-group arrays are incomplete or cyclic."});
              continue;
            }
            const auto spacing_type = static_cast<std::uint32_t>(actual_type_info.value[0]);
            std::array<double, 1> persisted_bar_count{};
            std::span<const double> spacing_values = spacings.value;
            if (spacing_type == 2U && read_u32(tuple, bar_count->offset) > 0U) {
              persisted_bar_count[0] = static_cast<double>(read_u32(tuple, bar_count->offset));
              spacing_values = persisted_bar_count;
            }
            const auto remaining_by_count =
                expanded_curve_count >= maximum_expanded_curve_count
                    ? 0U
                    : maximum_expanded_curve_count - expanded_curve_count;
            const auto remaining_count =
                std::min(remaining_by_count, geometry_budget.maximum_curve_count(polygon.size()));
            if (remaining_count == 0U) {
              geometry_budget.exhaust();
              diagnostics.push_back(
                  {ErrorCode::resource_limit, rebar_id,
                   "Classic rebar-group expansion exceeds the aggregate geometry memory budget."});
              continue;
            }
            const auto exclude_type = static_cast<std::uint32_t>(actual_type_info.value[2]);
            const auto distribution_start =
                geometry.value.size() >= 6U
                    ? Vector3d{geometry.value[0], geometry.value[1], geometry.value[2]}
                    : Vector3d{};
            const auto distribution_end =
                geometry.value.size() >= 6U
                    ? Vector3d{geometry.value[3], geometry.value[4], geometry.value[5]}
                    : Vector3d{};
            const auto stored_leg_count =
                geometry.value.size() >= 9U ? (geometry.value.size() - 9U) / 3U : 0U;
            const bool tapered_straight =
                polygon.size() == 4U && stored_leg_count == 1U &&
                actual_type_info.value[1] == 3.0 && geometry.value.size() >= 9U &&
                std::all_of(geometry.value.begin() + 6, geometry.value.end(),
                            [](double value) { return std::abs(value) <= 1.0e-7; });
            const bool nonplanar =
                std::any_of(polygon.begin(), polygon.end(), [&](const Vector3d point) {
                  return std::abs(point.z - polygon.front().z) > 0.002;
                });
            Result<std::vector<std::vector<Vector3d>>> evaluated =
                tapered_straight ? evaluate_tapered_straight_group_centerlines(
                                       std::array<Vector3d, 2>{polygon[0], polygon[1]},
                                       std::array<Vector3d, 2>{polygon[2], polygon[3]},
                                       distribution_start, distribution_end, 0.0, 0.0, spacing_type,
                                       spacing_values, exclude_type, remaining_count)
                : nonplanar && geometry.value.size() >= 12U
                    ? evaluate_nonplanar_group_reference_centerlines(
                          polygon, distribution_start, distribution_end,
                          (geometry.value[6] + geometry.value[7]) / 2.0, geometry.value[8],
                          geometry.value.back(), geometry.value[geometry.value.size() - 3U],
                          geometry.value[geometry.value.size() - 2U], spacing_type, spacing_values,
                          exclude_type, actual_type_info.value[1] != 1.0, remaining_count)
                    : group_centerlines(polygon, geometry.value, spacing_type, spacing_values,
                                        exclude_type, remaining_count);
            if (!evaluated) {
              diagnostics.push_back(
                  {evaluated.error().code, rebar_id, std::move(evaluated.error().message)});
              continue;
            }
            centerlines = std::move(evaluated.value());
            bool consumed = true;
            for (const auto& centerline : centerlines)
              consumed = consumed && geometry_budget.consume_curve(centerline.size());
            if (!consumed) {
              diagnostics.push_back(
                  {ErrorCode::resource_limit, rebar_id,
                   "Classic rebar-group expansion exceeds the aggregate geometry memory budget."});
              continue;
            }
            expanded_curve_count += centerlines.size();
          }
          evaluated_curves.reserve(centerlines.size());
          for (auto& centerline : centerlines)
            evaluated_curves.emplace_back(std::move(centerline), *radius);
        } else if (object->second.subtype == 6U || object->second.subtype == 8U) {
          const auto mesh_attribute = mesh_attributes.find(read_u32(tuple, mesh_attr_id->offset));
          if (mesh_attribute == mesh_attributes.end()) {
            diagnostics.push_back({ErrorCode::decoder_unavailable, rebar_id,
                                   "Classic rebar-mesh attribute is missing."});
            continue;
          }
          const auto& sizes =
              resolve_string(strings, string_next, mesh_attribute->second.diameters_id,
                             string_cache, linked_visit_budget, geometry_budget);
          const auto& spacings =
              resolve_string(strings, string_next, mesh_attribute->second.spacings_id, string_cache,
                             linked_visit_budget, geometry_budget);
          const auto& geometry = resolve_array(doubles, read_u32(tuple, geometry_id->offset),
                                               double_cache, linked_visit_budget, geometry_budget);
          const LinkedDecodeResult<std::vector<double>>* bending = nullptr;
          if (object->second.subtype == 8U) {
            bending = &resolve_array(doubles, read_u32(tuple, bending_id->offset), double_cache,
                                     linked_visit_budget, geometry_budget);
          }
          if (sizes.status == LinkedDecodeStatus::resource_limit ||
              spacings.status == LinkedDecodeStatus::resource_limit ||
              geometry.status == LinkedDecodeStatus::resource_limit ||
              (bending != nullptr && bending->status == LinkedDecodeStatus::resource_limit)) {
            diagnostics.push_back(
                {ErrorCode::resource_limit, rebar_id,
                 "Classic rebar-mesh linked data exceeds the aggregate geometry memory budget."});
            continue;
          }
          const auto size_pair = sizes.status == LinkedDecodeStatus::success
                                     ? mesh_string_pair(sizes.value)
                                     : std::nullopt;
          const auto spacing_pair = spacings.status == LinkedDecodeStatus::success
                                        ? mesh_string_pair(spacings.value)
                                        : std::nullopt;
          const auto longitudinal_radius = size_pair ? rebar_radius((*size_pair)[0]) : std::nullopt;
          const auto cross_radius = size_pair ? rebar_radius((*size_pair)[1]) : std::nullopt;
          const auto longitudinal_spacing =
              spacing_pair ? positive_number((*spacing_pair)[0]) : std::nullopt;
          const auto cross_spacing =
              spacing_pair ? positive_number((*spacing_pair)[1]) : std::nullopt;
          if (!longitudinal_radius || !cross_radius || !longitudinal_spacing || !cross_spacing ||
              geometry.status != LinkedDecodeStatus::success ||
              (bending != nullptr &&
               (bending->status != LinkedDecodeStatus::success || bending->value.empty()))) {
            diagnostics.push_back({ErrorCode::invalid_geometry, rebar_id,
                                   "Classic rebar-mesh attributes or arrays are invalid."});
            continue;
          }
          const auto remaining_by_count = expanded_curve_count >= maximum_expanded_curve_count
                                              ? 0U
                                              : maximum_expanded_curve_count - expanded_curve_count;
          auto remaining_count =
              std::min(remaining_by_count, geometry_budget.maximum_curve_count(2U));
          if (object->second.subtype == 8U) {
            if (geometry.value.size() < 6U) {
              diagnostics.push_back({ErrorCode::invalid_geometry, rebar_id,
                                     "Classic bent rebar-mesh arrays are inconsistent."});
              continue;
            }
            const Vector3d distribution_start{geometry.value[0U], geometry.value[1U],
                                              geometry.value[2U]};
            const Vector3d distribution_end{geometry.value[3U], geometry.value[4U],
                                            geometry.value[5U]};
            const double distribution_length =
                vector_length(subtract(distribution_end, distribution_start));
            auto cross_count =
                regular_distance_count(mesh_attribute->second.longitudinal_overhang_left,
                                       distribution_length, *cross_spacing);
            auto longitudinal_count =
                regular_distance_count(mesh_attribute->second.cross_overhang_left,
                                       mesh_attribute->second.width, *longitudinal_spacing);
            if (!cross_count || !longitudinal_count) {
              const auto& error = !cross_count ? cross_count.error() : longitudinal_count.error();
              diagnostics.push_back({error.code, rebar_id, error.message});
              continue;
            }
            if (longitudinal_count.value() >
                std::numeric_limits<std::size_t>::max() - cross_count.value()) {
              diagnostics.push_back(
                  {ErrorCode::resource_limit, rebar_id,
                   "Classic rebar-mesh expansion exceeds the aggregate geometry memory budget."});
              continue;
            }
            const auto required_count = cross_count.value() + longitudinal_count.value();
            const std::array<std::pair<std::size_t, std::size_t>, 2> retained_sets{
                {{cross_count.value(), polygon.size()}, {longitudinal_count.value(), 2U}}};
            if (required_count > remaining_by_count ||
                !geometry_budget.can_consume_curve_sets(retained_sets)) {
              diagnostics.push_back(
                  {ErrorCode::resource_limit, rebar_id,
                   "Classic rebar-mesh expansion exceeds the aggregate geometry memory budget."});
              continue;
            }
            remaining_count = required_count;
          } else if (remaining_count == 0U) {
            geometry_budget.exhaust();
            diagnostics.push_back(
                {ErrorCode::resource_limit, rebar_id,
                 "Classic rebar-mesh expansion exceeds the aggregate geometry memory budget."});
            continue;
          }
          Result<MeshCenterlines> evaluated =
              object->second.subtype == 6U
                  ? polygon_mesh_centerlines(
                        polygon, geometry.value, *longitudinal_spacing, *cross_spacing,
                        mesh_attribute->second.longitudinal_overhang_left,
                        mesh_attribute->second.cross_overhang_left, *longitudinal_radius,
                        *cross_radius, mesh_attribute->second.flags, remaining_count)
                  : bent_mesh_centerlines(polygon, geometry.value, bending->value.front(),
                                          mesh_attribute->second.width, *longitudinal_spacing,
                                          *cross_spacing,
                                          mesh_attribute->second.longitudinal_overhang_left,
                                          mesh_attribute->second.cross_overhang_left,
                                          *longitudinal_radius, *cross_radius, remaining_count);
          if (!evaluated) {
            diagnostics.push_back(
                {evaluated.error().code, rebar_id, std::move(evaluated.error().message)});
            continue;
          }
          bool consumed = true;
          for (const auto& centerline : evaluated.value().cross)
            consumed = consumed && geometry_budget.consume_curve(centerline.size());
          for (const auto& centerline : evaluated.value().longitudinal)
            consumed = consumed && geometry_budget.consume_curve(centerline.size());
          if (!consumed) {
            diagnostics.push_back(
                {ErrorCode::resource_limit, rebar_id,
                 "Classic rebar-mesh expansion exceeds the aggregate geometry memory budget."});
            continue;
          }
          const auto mesh_curve_count =
              evaluated.value().cross.size() + evaluated.value().longitudinal.size();
          expanded_curve_count += mesh_curve_count;
          evaluated_curves.reserve(mesh_curve_count);
          if (object->second.subtype == 6U) {
            for (auto& centerline : evaluated.value().longitudinal)
              evaluated_curves.emplace_back(std::move(centerline), *longitudinal_radius);
            for (auto& centerline : evaluated.value().cross)
              evaluated_curves.emplace_back(std::move(centerline), *cross_radius);
          } else {
            for (auto& centerline : evaluated.value().cross)
              evaluated_curves.emplace_back(std::move(centerline), *cross_radius);
            for (auto& centerline : evaluated.value().longitudinal)
              evaluated_curves.emplace_back(std::move(centerline), *longitudinal_radius);
          }
        } else {
          diagnostics.push_back({ErrorCode::decoder_unavailable, rebar_id,
                                 "Classic reinforcement subtype is not yet emitted as curves."});
          continue;
        }
        const auto& frame_axes = axes.at(system->second.axes_id);
        for (auto& [centerline, radius] : evaluated_curves) {
          for (auto& point : centerline) point = transform(point, system->second, frame_axes);
          curves.push_back({rebar_id, CurveGeometryKind::polyline, std::move(centerline), radius});
        }
      }
    }
  }

  // Bolt groups: emit independently persisted shanks plus the supported
  // historical 8.95 headed-stud family. Other heads, nuts and special anchors
  // remain explicit future adapter/evaluator work.
  if (const auto* bolts =
          populated_table(*storage, schema, std::array<std::string_view, 1>{"bolt"})) {
    const auto* id = find_field(schema, *bolts, "id");
    const auto* attr_id = find_field(schema, *bolts, "bolt_attr_id");
    if (attr_id == nullptr) attr_id = find_field(schema, *bolts, "attr_id");
    const auto* polygon_id = find_field(schema, *bolts, "polygon_id");
    const auto* attrs =
        populated_table(*storage, schema,
                        std::array<std::string_view, 4>{"bolt_attr", "old_bolt_attr_935",
                                                        "old_bolt_attr_897", "old_bolt_attr_807"});
    if (id != nullptr && attr_id != nullptr && polygon_id != nullptr && attrs != nullptr) {
      const auto* attr_key = find_field(schema, *attrs, "id");
      const auto* diameter_field = find_field(schema, *attrs, "BoltDiameter");
      const auto* standard_field = find_field(schema, *attrs, "mat");
      const auto* length_field = find_field(schema, *attrs, "BoltLength");
      const auto* structure_field = find_field(schema, *attrs, "BoltStructure");
      std::unordered_map<std::uint32_t, BoltDisplayAttribute> attributes;
      if (attr_key != nullptr && diameter_field != nullptr) {
        const auto& layout = storage->layout.tables[attrs->ordinal];
        for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
          const auto record = layout.record(storage->payload.bytes(), row);
          if (hidden(record)) continue;
          const auto tuple = record.subspan(1U, attrs->tuple_size);
          attributes.insert_or_assign(
              read_u32(tuple, attr_key->offset),
              BoltDisplayAttribute{
                  .standard = standard_field == nullptr
                                  ? std::string{}
                                  : std::string(read_text(tuple, standard_field->offset,
                                                          standard_field->size)),
                  .diameter = scalar(tuple, *diameter_field),
                  .length = length_field == nullptr ? 0.0 : scalar(tuple, *length_field),
                  .structure =
                      structure_field == nullptr ? 0U : read_u32(tuple, structure_field->offset),
                  .legacy = attrs->name != "bolt_attr"});
        }
      }
      std::unordered_map<std::uint32_t, std::vector<PolygonChunk>> chunks;
      const auto* polygons = populated_table(
          *storage, schema, std::array<std::string_view, 2>{"partpolygon", "old_partpolygon_898"});
      if (polygons != nullptr) {
        const auto* key = find_field(schema, *polygons, "id");
        const auto* number = find_field(schema, *polygons, "no");
        if (key != nullptr) {
          const auto& layout = storage->layout.tables[polygons->ordinal];
          for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
            const auto record = layout.record(storage->payload.bytes(), row);
            if (hidden(record)) continue;
            const auto tuple = record.subspan(1U, polygons->tuple_size);
            PolygonChunk chunk{.number = number == nullptr ? 0U : read_u32(tuple, number->offset)};
            for (std::size_t index = 1U; index <= 10U; ++index) {
              const auto suffix = std::to_string(index);
              const auto* x = find_field(schema, *polygons, "x" + suffix);
              const auto* y = find_field(schema, *polygons, "y" + suffix);
              const auto* type = find_field(schema, *polygons, "types" + suffix);
              if (x == nullptr || y == nullptr || type == nullptr ||
                  read_u32(tuple, type->offset) == 2'147'483'647U)
                break;
              chunk.points.push_back({scalar(tuple, *x), scalar(tuple, *y)});
            }
            if (!chunk.points.empty())
              chunks[read_u32(tuple, key->offset)].push_back(std::move(chunk));
          }
        }
      }
      for (auto& [key, polygon_chunks] : chunks) {
        (void)key;
        std::sort(polygon_chunks.begin(), polygon_chunks.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.number < rhs.number; });
      }
      std::unordered_map<std::uint32_t, BoltAxisLimits> axis_limits;
      if (const auto* hole_points = populated_table(
              *storage, schema, std::array<std::string_view, 1>{"bolt_hole_points"})) {
        const auto* key = find_field(schema, *hole_points, "bolt_id");
        const auto* start = find_field(schema, *hole_points, "int_point_1_z");
        const auto* end = find_field(schema, *hole_points, "int_point_2_z");
        if (key != nullptr && start != nullptr && end != nullptr) {
          const auto& hole_layout = storage->layout.tables[hole_points->ordinal];
          for (std::uint64_t row = 0; row < hole_layout.info.row_count; ++row) {
            const auto record = hole_layout.record(storage->payload.bytes(), row);
            if (hidden(record)) continue;
            const auto tuple = record.subspan(1U, hole_points->tuple_size);
            double first = scalar(tuple, *start);
            double second = scalar(tuple, *end);
            if (second < first) std::swap(first, second);
            axis_limits.insert_or_assign(read_u32(tuple, key->offset),
                                         BoltAxisLimits{first, second});
          }
        }
      }
      const auto& layout = storage->layout.tables[bolts->ordinal];
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (hidden(record)) continue;
        const auto tuple = record.subspan(1U, bolts->tuple_size);
        const auto bolt_id = read_u32(tuple, id->offset);
        if (bolt_id < request.geometry_object_id_min || bolt_id > request.geometry_object_id_max)
          continue;
        const auto object = object_types.find(bolt_id);
        if (object == object_types.end() || object->second.type != 10U) continue;
        constexpr std::size_t minimum_cylinder_position_count = (2U * 12U + 2U) * 3U;
        constexpr std::size_t minimum_cylinder_index_count = 12U * 12U;
        if (!geometry_budget.can_consume_mesh(minimum_cylinder_position_count,
                                              minimum_cylinder_index_count)) {
          diagnostics.push_back(
              {ErrorCode::resource_limit, bolt_id,
               "Bolt shank expansion exceeds the aggregate geometry memory budget."});
          continue;
        }
        const auto attribute = attributes.find(read_u32(tuple, attr_id->offset));
        const auto system = systems.find(bolt_id);
        if (attribute == attributes.end() || system == systems.end() ||
            !axes.contains(system->second.axes_id) || attribute->second.diameter <= 0.0 ||
            system->second.length <= 0.0) {
          diagnostics.push_back({ErrorCode::decoder_unavailable, bolt_id,
                                 "Bolt shank frame or diameter is unavailable."});
          continue;
        }
        const auto found = chunks.find(read_u32(tuple, polygon_id->offset));
        if (found == chunks.end()) {
          diagnostics.push_back(
              {ErrorCode::decoder_unavailable, bolt_id, "Bolt pattern polygon is unavailable."});
          continue;
        }
        std::size_t pattern_count = 0U;
        bool pattern_count_valid = true;
        for (const auto& chunk : found->second) {
          if (chunk.points.size() > std::numeric_limits<std::size_t>::max() - pattern_count) {
            pattern_count_valid = false;
            break;
          }
          pattern_count += chunk.points.size();
        }
        const bool legacy_undefined_stud =
            attribute->second.legacy && attribute->second.standard == "UNDEFINED_STUD" &&
            std::abs(attribute->second.diameter - 20.0) <= 1.0e-6 &&
            attribute->second.structure == 110U && attribute->second.length > 10.0;
        if (attribute->second.legacy && !legacy_undefined_stud) {
          diagnostics.push_back({ErrorCode::decoder_unavailable, bolt_id,
                                 "Legacy bolt fastener display dimensions are unavailable."});
          continue;
        }
        const auto legacy_limits = axis_limits.find(bolt_id);
        const auto fastener =
            known_fastener_dimensions(attribute->second.standard, attribute->second.diameter);
        const bool headed_fastener =
            !attribute->second.legacy && fastener.has_value() &&
            legacy_limits != axis_limits.end() &&
            legacy_limits->second.end - legacy_limits->second.start > 1.0e-9;
        if (legacy_undefined_stud &&
            (legacy_limits == axis_limits.end() ||
             legacy_limits->second.end - legacy_limits->second.start <= 1.0e-9)) {
          diagnostics.push_back(
              {ErrorCode::invalid_geometry, bolt_id,
               "Legacy headed-stud evaluation requires a non-zero persisted shank."});
          continue;
        }
        constexpr std::size_t cylinder_position_count = (2U * 12U + 2U) * 3U;
        constexpr std::size_t cylinder_index_count = 12U * 12U;
        constexpr std::size_t headed_stud_position_count = 2U * (2U * 8U + 2U) * 3U;
        constexpr std::size_t headed_stud_index_count = 2U * 8U * 12U;
        constexpr std::size_t headed_fastener_position_count =
            cylinder_position_count + 2U * (2U * 6U + 2U) * 3U;
        constexpr std::size_t headed_fastener_index_count = cylinder_index_count + 2U * 6U * 12U;
        const auto position_count = legacy_undefined_stud ? headed_stud_position_count
                                    : headed_fastener     ? headed_fastener_position_count
                                                          : cylinder_position_count;
        const auto index_count = legacy_undefined_stud ? headed_stud_index_count
                                 : headed_fastener     ? headed_fastener_index_count
                                                       : cylinder_index_count;
        if (!pattern_count_valid ||
            pattern_count > std::numeric_limits<std::size_t>::max() / position_count ||
            pattern_count > std::numeric_limits<std::size_t>::max() / index_count ||
            !geometry_budget.can_consume_mesh(pattern_count * position_count,
                                              pattern_count * index_count)) {
          geometry_budget.exhaust();
          diagnostics.push_back(
              {ErrorCode::resource_limit, bolt_id,
               "Bolt shank expansion exceeds the aggregate geometry memory budget."});
          continue;
        }
        std::vector<std::array<double, 2>> pattern;
        pattern.reserve(pattern_count);
        for (const auto& chunk : found->second)
          pattern.insert(pattern.end(), chunk.points.begin(), chunk.points.end());
        if (pattern.empty()) {
          diagnostics.push_back(
              {ErrorCode::decoder_unavailable, bolt_id, "Bolt pattern polygon is unavailable."});
          continue;
        }
        MeshData mesh{.object_id = bolt_id};
        const auto& frame = axes.at(system->second.axes_id);
        const auto z = cross(frame.x, frame.y);
        for (const auto point : pattern) {
          const auto pattern_origin =
              add(system->second.origin, add(scale(frame.x, point[0]), scale(frame.y, point[1])));
          if (legacy_undefined_stud) {
            constexpr double head_height = 10.0;
            constexpr double head_radius = 16.5;
            const double shank_end =
                legacy_limits->second.end + attribute->second.length - head_height;
            append_prism(mesh, add(pattern_origin, scale(z, legacy_limits->second.end)),
                         add(pattern_origin, scale(z, shank_end)), frame.x, frame.y,
                         attribute->second.diameter / 2.0, 8U);
            append_prism(mesh, add(pattern_origin, scale(z, shank_end)),
                         add(pattern_origin, scale(z, shank_end + head_height)), frame.x, frame.y,
                         head_radius, 8U);
            continue;
          }
          if (const auto limits = axis_limits.find(bolt_id); limits != axis_limits.end()) {
            append_cylinder(mesh, add(pattern_origin, scale(z, limits->second.start)),
                            add(pattern_origin, scale(z, limits->second.end)),
                            attribute->second.diameter / 2.0);
            if (headed_fastener) {
              constexpr double rotation = std::numbers::pi / 6.0;
              const auto first_axis =
                  add(scale(frame.x, std::cos(rotation)), scale(frame.y, std::sin(rotation)));
              const auto second_axis =
                  add(scale(frame.x, -std::sin(rotation)), scale(frame.y, std::cos(rotation)));
              const double radius =
                  fastener->across_flats / (2.0 * std::cos(std::numbers::pi / 6.0));
              append_prism(mesh,
                           add(pattern_origin, scale(z, limits->second.start - fastener->height)),
                           add(pattern_origin, scale(z, limits->second.start)), first_axis,
                           second_axis, radius, 6U);
              append_prism(mesh, add(pattern_origin, scale(z, limits->second.end)),
                           add(pattern_origin, scale(z, limits->second.end + fastener->height)),
                           first_axis, second_axis, radius, 6U);
            }
          } else {
            append_cylinder(mesh, add(pattern_origin, scale(z, -system->second.length)),
                            pattern_origin, attribute->second.diameter / 2.0);
          }
        }
        if (!mesh.positions.empty()) {
          if (!geometry_budget.consume_mesh(mesh.positions.size(), mesh.indices.size())) {
            geometry_budget.exhaust();
            diagnostics.push_back(
                {ErrorCode::resource_limit, bolt_id,
                 "Bolt shank expansion exceeds the aggregate geometry memory budget."});
            continue;
          }
          meshes.push_back(std::move(mesh));
        }
      }
    }
  }

  return Result<ProcessStream>::success(std::make_unique<NonPartReader>(
      std::move(curves), std::move(meshes), std::move(diagnostics), batch_size));
}

}  // namespace tekla::db1::detail
