#include "identity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tekla::db1::detail {
namespace {

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1])) << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3])) << 24U);
}

[[nodiscard]] const FieldSchema* find_field(const Schema& schema, const TableSchema& table,
                                            std::string_view name) noexcept {
  for (const auto& field : schema.table_fields(table)) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

[[nodiscard]] Result<std::uint32_t> u32_offset(const Schema& schema, const TableSchema& table,
                                               std::string_view name) {
  const auto* field = find_field(schema, table, name);
  if (field == nullptr || field->type != FieldType::u32 || field->size != 4 ||
      field->offset + field->size > table.tuple_size) {
    return Result<std::uint32_t>::failure(
        {ErrorCode::schema_mismatch,
         "The generated identity field '" + std::string(name) + "' is unavailable."});
  }
  return Result<std::uint32_t>::success(field->offset);
}

struct ObjectOffsets {
  std::uint32_t id = 0;
  std::uint32_t parent = 0;
  std::uint32_t assembly = 0;
  std::uint32_t guid = 0;
  std::uint32_t type = 0;
  std::uint32_t subtype = 0;
  std::uint32_t flags = 0;
  std::uint32_t attribute_id = 0;
};

struct AttributeOffsets {
  std::uint32_t id = 0;
  std::uint32_t type = 0;
  std::uint32_t subtype = 0;
  std::uint32_t flags = 0;
};

struct LegacyAttribute {
  std::uint32_t type = 0;
  std::uint32_t subtype = 0;
  std::uint32_t flags = 0;
};

[[nodiscard]] char hexadecimal(std::uint8_t value) noexcept {
  constexpr std::string_view digits = "0123456789abcdef";
  return digits[value & 0x0fU];
}

void format_guid(std::span<const std::byte, 16> bytes, std::array<char, 36>& output) noexcept {
  constexpr std::array<std::size_t, 4> breaks{4, 6, 8, 10};
  std::size_t target = 0;
  for (std::size_t source = 0; source < bytes.size(); ++source) {
    if (std::find(breaks.begin(), breaks.end(), source) != breaks.end()) {
      output[target++] = '-';
    }
    const auto value = std::to_integer<std::uint8_t>(bytes[source]);
    output[target++] = hexadecimal(static_cast<std::uint8_t>(value >> 4U));
    output[target++] = hexadecimal(value);
  }
}

[[nodiscard]] bool normalize_legacy_guid(std::span<const std::byte> bytes,
                                         std::array<char, 36>& output,
                                         std::size_t& length) noexcept {
  std::size_t end = 0;
  while (end < bytes.size() && bytes[end] != std::byte{0}) {
    ++end;
  }
  std::size_t begin = 0;
  while (begin < end && std::isspace(static_cast<unsigned char>(bytes[begin])) != 0) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(bytes[end - 1])) != 0) {
    --end;
  }
  if (begin == end) {
    length = 0;
    return true;
  }
  if (bytes[begin] == std::byte{'{'} && bytes[end - 1] == std::byte{'}'}) {
    ++begin;
    --end;
  }
  if (end - begin != output.size()) {
    return false;
  }
  constexpr std::array<std::size_t, 4> hyphens{8, 13, 18, 23};
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto character = static_cast<unsigned char>(bytes[begin + index]);
    const bool expects_hyphen = std::find(hyphens.begin(), hyphens.end(), index) != hyphens.end();
    if (expects_hyphen) {
      if (character != '-') {
        return false;
      }
      output[index] = '-';
    } else if (std::isxdigit(character) != 0) {
      output[index] = static_cast<char>(std::tolower(character));
    } else {
      return false;
    }
  }
  length = output.size();
  return true;
}

[[nodiscard]] ObjectKind object_kind(std::uint32_t type, std::uint32_t subtype) noexcept {
  if (type == 2U) {
    switch (subtype) {
      case 0U:
        return ObjectKind::beam;
      case 1U:
        return ObjectKind::contour_plate;
      case 2U:
        return ObjectKind::poly_beam;
      case 5U:
        return ObjectKind::brep;
      case 8U:
        return ObjectKind::lofted_plate;
      default:
        return ObjectKind::unknown;
    }
  }
  if (type == 3U) return ObjectKind::connection;
  if (type == 4U) return ObjectKind::component;
  if (type == 7U) return ObjectKind::grid;
  if (type == 10U && subtype == 1U) return ObjectKind::bolt_array;
  if (type == 11U) return ObjectKind::boolean_part;
  if (type == 12U) return ObjectKind::cut_plane;
  if (type == 13U) return ObjectKind::weld;
  if (type == 30U && subtype == 3U) return ObjectKind::grid_plane;
  if (type == 47U) {
    if (subtype == 0U) return ObjectKind::single_rebar;
    if (subtype == 1U) return ObjectKind::rebar_group;
    if (subtype == 6U || subtype == 8U) return ObjectKind::rebar_mesh;
  }
  return ObjectKind::unknown;
}

class IdentityReader final : public BatchReader {
 public:
  IdentityReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& objects,
                 const TableSchema& object_schema, ObjectOffsets object_offsets,
                 std::unordered_map<std::uint32_t, LegacyAttribute> attributes,
                 std::unordered_map<std::uint32_t, WeldLocation> weld_locations,
                 std::size_t batch_size, bool legacy)
      : storage_(std::move(storage)),
        objects_(&objects),
        object_schema_(&object_schema),
        object_offsets_(object_offsets),
        attributes_(std::move(attributes)),
        weld_locations_(std::move(weld_locations)),
        batch_size_(batch_size),
        legacy_(legacy) {
    batch_.reserve(batch_size_);
    guids_.reserve(batch_size_);
  }

  Result<BatchView> next() override {
    if (row_ >= objects_->info.row_count) {
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    }
    batch_.clear();
    guids_.clear();
    const auto payload = storage_->payload.bytes();
    while (row_ < objects_->info.row_count && batch_.size() < batch_size_) {
      const auto record = objects_->record(payload, row_++);
      if (record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "An object record lies outside the payload."});
      }
      const auto tuple = record.subspan(1, object_schema_->tuple_size);
      ObjectView object;
      object.visible = (std::to_integer<std::uint8_t>(record[0]) & 0x08U) == 0;
      object.internal_id = read_u32(tuple, object_offsets_.id);
      object.parent_id = read_u32(tuple, object_offsets_.parent);
      object.assembly_id = read_u32(tuple, object_offsets_.assembly);
      object.row_id = read_u32(record, 1 + object_schema_->tuple_size);
      object.event_id = read_u32(record, 1 + object_schema_->tuple_size + 4);

      guids_.emplace_back();
      std::size_t guid_length = 36;
      if (legacy_) {
        const auto attribute_id = read_u32(tuple, object_offsets_.attribute_id);
        const auto attribute = attributes_.find(attribute_id);
        if (attribute == attributes_.end()) {
          return Result<BatchView>::failure(
              {ErrorCode::schema_mismatch,
               "A legacy object references a missing attribute record."});
        }
        object.type = attribute->second.type;
        object.subtype = attribute->second.subtype;
        object.object_flags = attribute->second.flags;
        if (!normalize_legacy_guid(tuple.subspan(object_offsets_.guid, 39), guids_.back(),
                                   guid_length)) {
          return Result<BatchView>::failure(
              {ErrorCode::schema_mismatch, "A legacy object GUID is malformed."});
        }
      } else {
        object.type = read_u32(tuple, object_offsets_.type);
        object.subtype = read_u32(tuple, object_offsets_.subtype);
        object.object_flags = read_u32(tuple, object_offsets_.flags);
        format_guid(std::span<const std::byte, 16>(tuple.subspan(object_offsets_.guid, 16)),
                    guids_.back());
      }
      object.kind = object_kind(object.type, object.subtype);
      if (object.kind == ObjectKind::weld) {
        const auto location = weld_locations_.find(static_cast<std::uint32_t>(object.internal_id));
        object.weld_location =
            location == weld_locations_.end() ? WeldLocation::unknown : location->second;
      }
      object.application_id = std::string_view(guids_.back().data(), guid_length);
      batch_.push_back(object);
    }
    return Result<BatchView>::success(BatchView{.kind = BatchKind::objects, .objects = batch_});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* objects_ = nullptr;
  const TableSchema* object_schema_ = nullptr;
  ObjectOffsets object_offsets_;
  std::unordered_map<std::uint32_t, LegacyAttribute> attributes_;
  std::unordered_map<std::uint32_t, WeldLocation> weld_locations_;
  std::size_t batch_size_ = 0;
  std::uint64_t row_ = 0;
  bool legacy_ = false;
  std::vector<ObjectView> batch_;
  std::vector<std::array<char, 36>> guids_;
};

[[nodiscard]] Result<ObjectOffsets> modern_offsets(const Schema& schema, const TableSchema& table) {
  ObjectOffsets result;
  auto id = u32_offset(schema, table, "id");
  auto parent = u32_offset(schema, table, "kuuluu");
  auto assembly = u32_offset(schema, table, "assembly");
  auto type = u32_offset(schema, table, "type");
  auto subtype = u32_offset(schema, table, "subtype");
  auto flags = u32_offset(schema, table, "obj_flag");
  const auto* guid = find_field(schema, table, "guid");
  if (!id || !parent || !assembly || !type || !subtype || !flags || guid == nullptr ||
      guid->type != FieldType::bytes || guid->size != 16 ||
      guid->offset + guid->size > table.tuple_size) {
    return Result<ObjectOffsets>::failure(
        {ErrorCode::schema_mismatch, "The generated modern object layout is incomplete."});
  }
  result.id = id.value();
  result.parent = parent.value();
  result.assembly = assembly.value();
  result.guid = guid->offset;
  result.type = type.value();
  result.subtype = subtype.value();
  result.flags = flags.value();
  return Result<ObjectOffsets>::success(result);
}

[[nodiscard]] Result<ObjectOffsets> legacy_offsets(const Schema& schema, const TableSchema& table) {
  ObjectOffsets result;
  auto id = u32_offset(schema, table, "id");
  auto parent = u32_offset(schema, table, "kuuluu");
  auto assembly = u32_offset(schema, table, "assembly");
  auto attribute_id = u32_offset(schema, table, "object_attr_id");
  const auto* guid = find_field(schema, table, "guid");
  if (!id || !parent || !assembly || !attribute_id || guid == nullptr ||
      guid->type != FieldType::text || guid->size != 39 ||
      guid->offset + guid->size > table.tuple_size) {
    return Result<ObjectOffsets>::failure(
        {ErrorCode::schema_mismatch, "The generated legacy object layout is incomplete."});
  }
  result.id = id.value();
  result.parent = parent.value();
  result.assembly = assembly.value();
  result.guid = guid->offset;
  result.attribute_id = attribute_id.value();
  return Result<ObjectOffsets>::success(result);
}

[[nodiscard]] Result<std::unordered_map<std::uint32_t, WeldLocation>> weld_locations(
    const ModelStorage& storage, const Schema& schema) {
  std::unordered_map<std::uint32_t, WeldLocation> result;
  const auto* welding_schema = schema.find_table("welding");
  const auto* common_schema = schema.find_table("welding_common_attr");
  if (welding_schema == nullptr || common_schema == nullptr ||
      welding_schema->ordinal >= storage.layout.tables.size() ||
      common_schema->ordinal >= storage.layout.tables.size()) {
    return Result<std::unordered_map<std::uint32_t, WeldLocation>>::success(std::move(result));
  }
  const auto& welding_table = storage.layout.tables[welding_schema->ordinal];
  const auto& common_table = storage.layout.tables[common_schema->ordinal];
  if (welding_table.info.row_count == 0 || common_table.info.row_count == 0) {
    return Result<std::unordered_map<std::uint32_t, WeldLocation>>::success(std::move(result));
  }

  auto common_id = u32_offset(schema, *common_schema, "id");
  auto workshop = u32_offset(schema, *common_schema, "workshop_weld");
  auto weld_id = u32_offset(schema, *welding_schema, "id");
  auto common_reference = u32_offset(schema, *welding_schema, "weld_common_attr_id");
  if (!common_id || !workshop || !weld_id || !common_reference) {
    return Result<std::unordered_map<std::uint32_t, WeldLocation>>::failure(
        {ErrorCode::schema_mismatch, "The generated weld semantic layout is incomplete."});
  }

  std::unordered_map<std::uint32_t, WeldLocation> common_locations;
  common_locations.reserve(static_cast<std::size_t>(common_table.info.row_count));
  const auto payload = storage.payload.bytes();
  for (std::uint64_t row = 0; row < common_table.info.row_count; ++row) {
    const auto record = common_table.record(payload, row);
    if (record.empty()) {
      return Result<std::unordered_map<std::uint32_t, WeldLocation>>::failure(
          {ErrorCode::invalid_container, "A weld-common record lies outside the payload."});
    }
    const auto tuple = record.subspan(1, common_schema->tuple_size);
    common_locations.insert_or_assign(
        read_u32(tuple, common_id.value()),
        read_u32(tuple, workshop.value()) == 1U ? WeldLocation::workshop : WeldLocation::site);
  }

  result.reserve(static_cast<std::size_t>(welding_table.info.row_count));
  for (std::uint64_t row = 0; row < welding_table.info.row_count; ++row) {
    const auto record = welding_table.record(payload, row);
    if (record.empty()) {
      return Result<std::unordered_map<std::uint32_t, WeldLocation>>::failure(
          {ErrorCode::invalid_container, "A welding record lies outside the payload."});
    }
    const auto tuple = record.subspan(1, welding_schema->tuple_size);
    const auto location = common_locations.find(read_u32(tuple, common_reference.value()));
    if (location != common_locations.end()) {
      result.insert_or_assign(read_u32(tuple, weld_id.value()), location->second);
    }
  }
  return Result<std::unordered_map<std::uint32_t, WeldLocation>>::success(std::move(result));
}

}  // namespace

Result<ProcessStream> make_identity_stream(std::shared_ptr<const ModelStorage> storage,
                                           const Schema& schema, const ProcessRequest& request) {
  const TableSchema* object_schema = schema.find_table("object");
  bool legacy = object_schema == nullptr ||
                storage->layout.tables[object_schema->ordinal].info.row_count == 0;
  if (legacy) {
    object_schema = schema.find_table("old_object_948");
  }
  if (object_schema == nullptr || object_schema->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The schema has no supported object identity table."});
  }

  Result<ObjectOffsets> offsets =
      legacy ? legacy_offsets(schema, *object_schema) : modern_offsets(schema, *object_schema);
  if (!offsets) {
    return Result<ProcessStream>::failure(offsets.error());
  }

  std::unordered_map<std::uint32_t, LegacyAttribute> attributes;
  if (legacy) {
    constexpr std::array<std::string_view, 4> names{"old_object_attr_951", "old_object_attr_915",
                                                    "old_object_attr_900", "old_object_attr_879"};
    const TableSchema* attribute_schema = nullptr;
    for (const auto name : names) {
      if (const auto* candidate = schema.find_table(name);
          candidate != nullptr && storage->layout.tables[candidate->ordinal].info.row_count != 0) {
        attribute_schema = candidate;
        break;
      }
    }
    if (attribute_schema == nullptr) {
      return Result<ProcessStream>::failure(
          {ErrorCode::schema_mismatch, "The legacy object attribute table is unavailable."});
    }
    auto id = u32_offset(schema, *attribute_schema, "id");
    auto type = u32_offset(schema, *attribute_schema, "type");
    auto subtype = u32_offset(schema, *attribute_schema, "subtype");
    auto flags = u32_offset(schema, *attribute_schema, "obj_flag");
    if (!id || !type || !subtype || !flags) {
      return Result<ProcessStream>::failure(
          {ErrorCode::schema_mismatch, "The legacy object attribute layout is incomplete."});
    }
    const auto& table = storage->layout.tables[attribute_schema->ordinal];
    attributes.reserve(static_cast<std::size_t>(table.info.row_count));
    for (std::uint64_t row = 0; row < table.info.row_count; ++row) {
      const auto record = table.record(storage->payload.bytes(), row);
      const auto tuple = record.subspan(1, attribute_schema->tuple_size);
      attributes.emplace(
          read_u32(tuple, id.value()),
          LegacyAttribute{read_u32(tuple, type.value()), read_u32(tuple, subtype.value()),
                          read_u32(tuple, flags.value())});
    }
  }

  auto locations = weld_locations(*storage, schema);
  if (!locations) {
    return Result<ProcessStream>::failure(locations.error());
  }

  std::size_t batch_size = 4096;
  if (request.batch_memory_budget_bytes != 0) {
    constexpr std::uint64_t estimated_object_bytes = 128;
    batch_size = static_cast<std::size_t>(std::clamp<std::uint64_t>(
        request.batch_memory_budget_bytes / estimated_object_bytes, 1, 65536));
  }
  const auto* object_table = &storage->layout.tables[object_schema->ordinal];
  return Result<ProcessStream>::success(
      std::make_unique<IdentityReader>(std::move(storage), *object_table, *object_schema,
                                       offsets.value(), std::move(attributes),
                                       std::move(locations.value()), batch_size, legacy));
}

}  // namespace tekla::db1::detail
