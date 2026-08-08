#include "weld_semantics.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "record.hpp"

namespace tekla::db1::detail {
namespace {

[[nodiscard]] Result<const FieldSchema*> required_u32(const Schema& schema,
                                                      const TableSchema& table,
                                                      std::string_view name) {
  const auto* field = find_field(schema, table, name);
  if (field == nullptr || field->type != FieldType::u32 || field->size != 4U ||
      field->offset + field->size > table.tuple_size) {
    return Result<const FieldSchema*>::failure(
        {ErrorCode::schema_mismatch,
         "The generated weld field '" + std::string(name) + "' is unavailable."});
  }
  return Result<const FieldSchema*>::success(field);
}

struct CommonRow {
  WeldCommonSemantics semantics;
};

struct WeldLinkOffsets {
  const FieldSchema* above = nullptr;
  const FieldSchema* below = nullptr;
};

}  // namespace

Result<std::vector<WeldSemantics>> load_weld_semantics(const ModelStorage& storage,
                                                       const Schema& schema) {
  std::vector<WeldSemantics> result;
  const auto* welding = schema.find_table("welding");
  const auto* common = schema.find_table("welding_common_attr");
  if (welding == nullptr || common == nullptr || welding->ordinal >= storage.layout.tables.size() ||
      common->ordinal >= storage.layout.tables.size()) {
    return Result<std::vector<WeldSemantics>>::success(std::move(result));
  }
  const auto& welding_layout = storage.layout.tables[welding->ordinal];
  const auto& common_layout = storage.layout.tables[common->ordinal];
  if (welding_layout.info.row_count == 0U || common_layout.info.row_count == 0U) {
    return Result<std::vector<WeldSemantics>>::success(std::move(result));
  }

  auto common_id = required_u32(schema, *common, "id");
  auto workshop = required_u32(schema, *common, "workshop_weld");
  auto around = required_u32(schema, *common, "around_weld");
  auto compound = required_u32(schema, *common, "compound_weld");
  auto logical = required_u32(schema, *common, "logical_weld");
  auto weld_id = required_u32(schema, *welding, "id");
  auto common_reference = required_u32(schema, *welding, "weld_common_attr_id");
  if (!common_id || !workshop || !around || !compound || !logical || !weld_id ||
      !common_reference) {
    const auto& error = !common_id  ? common_id.error()
                        : !workshop ? workshop.error()
                        : !around   ? around.error()
                        : !compound ? compound.error()
                        : !logical  ? logical.error()
                        : !weld_id  ? weld_id.error()
                                    : common_reference.error();
    return Result<std::vector<WeldSemantics>>::failure(error);
  }
  const auto* intermittent_type = find_field(schema, *common, "intermittent_type");
  if (intermittent_type != nullptr &&
      (intermittent_type->type != FieldType::u32 || intermittent_type->size != 4U ||
       intermittent_type->offset + intermittent_type->size > common->tuple_size)) {
    return Result<std::vector<WeldSemantics>>::failure(
        {ErrorCode::schema_mismatch,
         "The generated weld field 'intermittent_type' is unavailable."});
  }

  std::unordered_map<std::uint32_t, WeldSeamSemantics> seam_rows;
  WeldLinkOffsets seam_links;
  if (const auto* seams = schema.find_table("welding_attr");
      seams != nullptr && seams->ordinal < storage.layout.tables.size() &&
      storage.layout.tables[seams->ordinal].info.row_count != 0U) {
    auto seam_id = required_u32(schema, *seams, "id");
    auto seam_type = required_u32(schema, *seams, "type");
    auto seam_intermittent = required_u32(schema, *seams, "intermittent");
    const auto* seam_size = find_field(schema, *seams, "size");
    auto above = required_u32(schema, *welding, "weld_seam1_id");
    auto below = required_u32(schema, *welding, "weld_seam2_id");
    if (!seam_id || !seam_type || !seam_intermittent || seam_size == nullptr ||
        seam_size->type != FieldType::f32 || seam_size->size != 4U ||
        seam_size->offset + seam_size->size > seams->tuple_size || !above || !below) {
      if (!seam_id) return Result<std::vector<WeldSemantics>>::failure(seam_id.error());
      if (!seam_type) return Result<std::vector<WeldSemantics>>::failure(seam_type.error());
      if (!seam_intermittent)
        return Result<std::vector<WeldSemantics>>::failure(seam_intermittent.error());
      if (!above) return Result<std::vector<WeldSemantics>>::failure(above.error());
      if (!below) return Result<std::vector<WeldSemantics>>::failure(below.error());
      return Result<std::vector<WeldSemantics>>::failure(
          {ErrorCode::schema_mismatch, "The generated weld field 'size' is unavailable."});
    }
    seam_links.above = above.value();
    seam_links.below = below.value();
    const auto& seam_layout = storage.layout.tables[seams->ordinal];
    seam_rows.reserve(static_cast<std::size_t>(seam_layout.info.row_count));
    for (std::uint64_t row = 0; row < seam_layout.info.row_count; ++row) {
      const auto record = seam_layout.record(storage.payload.bytes(), row);
      if (record.empty()) {
        return Result<std::vector<WeldSemantics>>::failure(
            {ErrorCode::invalid_container, "A weld-seam record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, seams->tuple_size);
      seam_rows.insert_or_assign(
          read_u32(tuple, seam_id.value()->offset),
          WeldSeamSemantics{.size = static_cast<double>(read_f32(tuple, seam_size->offset)),
                            .type = read_u32(tuple, seam_type.value()->offset),
                            .intermittent = read_u32(tuple, seam_intermittent.value()->offset)});
    }
  }

  std::unordered_map<std::uint32_t, CommonRow> common_rows;
  common_rows.reserve(static_cast<std::size_t>(common_layout.info.row_count));
  const auto payload = storage.payload.bytes();
  for (std::uint64_t row = 0; row < common_layout.info.row_count; ++row) {
    const auto record = common_layout.record(payload, row);
    if (record.empty()) {
      return Result<std::vector<WeldSemantics>>::failure(
          {ErrorCode::invalid_container, "A weld-common record lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
    const auto tuple = record.subspan(1U, common->tuple_size);
    const auto workshop_value = read_u32(tuple, workshop.value()->offset);
    CommonRow decoded;
    decoded.semantics.location = workshop_value == 1U ? WeldLocation::workshop : WeldLocation::site;
    decoded.semantics.workshop = workshop_value;
    decoded.semantics.around = read_u32(tuple, around.value()->offset);
    decoded.semantics.compound = read_u32(tuple, compound.value()->offset);
    decoded.semantics.logical = read_u32(tuple, logical.value()->offset);
    if (intermittent_type != nullptr) {
      decoded.semantics.intermittent_type = read_u32(tuple, intermittent_type->offset);
    }
    common_rows.insert_or_assign(read_u32(tuple, common_id.value()->offset), decoded);
  }

  result.reserve(static_cast<std::size_t>(welding_layout.info.row_count));
  for (std::uint64_t row = 0; row < welding_layout.info.row_count; ++row) {
    const auto record = welding_layout.record(payload, row);
    if (record.empty()) {
      return Result<std::vector<WeldSemantics>>::failure(
          {ErrorCode::invalid_container, "A welding record lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
    const auto tuple = record.subspan(1U, welding->tuple_size);
    const auto found = common_rows.find(read_u32(tuple, common_reference.value()->offset));
    if (found == common_rows.end()) continue;
    WeldSemantics decoded{.object_id = read_u32(tuple, weld_id.value()->offset),
                          .common = found->second.semantics};
    if (seam_links.above != nullptr) {
      if (const auto seam = seam_rows.find(read_u32(tuple, seam_links.above->offset));
          seam != seam_rows.end()) {
        decoded.above = seam->second;
      }
      if (const auto seam = seam_rows.find(read_u32(tuple, seam_links.below->offset));
          seam != seam_rows.end()) {
        decoded.below = seam->second;
      }
    }
    result.push_back(decoded);
  }
  return Result<std::vector<WeldSemantics>>::success(std::move(result));
}

}  // namespace tekla::db1::detail
