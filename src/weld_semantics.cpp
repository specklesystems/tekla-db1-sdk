#include "weld_semantics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "record.hpp"

namespace tekla::db1::detail {
namespace {

struct WeldCommonSemantics {
  WeldLocation location = WeldLocation::unknown;
  std::uint32_t workshop = 0U;
  std::uint32_t around = 0U;
  std::uint32_t compound = 0U;
  std::uint32_t logical = 0U;
  std::optional<std::uint32_t> intermittent_type;
};

struct WeldSeamSemantics {
  double size = 0.0;
  std::uint32_t type = 0U;
  std::uint32_t intermittent = 0U;
};

struct WeldSemantics {
  std::uint32_t object_id = 0U;
  WeldCommonSemantics common;
  std::optional<WeldSeamSemantics> above;
  std::optional<WeldSeamSemantics> below;
};

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

class WeldDecoder {
 public:
  [[nodiscard]] std::optional<WeldSemantics> decode(
      std::span<const std::byte> tuple) const noexcept {
    const auto common = common_rows_.find(read_u32(tuple, common_reference_offset_));
    if (common == common_rows_.end()) return std::nullopt;
    WeldSemantics result{.object_id = read_u32(tuple, object_id_offset_), .common = common->second};
    if (above_reference_offset_.has_value()) {
      if (const auto seam = seam_rows_.find(read_u32(tuple, *above_reference_offset_));
          seam != seam_rows_.end()) {
        result.above = seam->second;
      }
      if (const auto seam = seam_rows_.find(read_u32(tuple, *below_reference_offset_));
          seam != seam_rows_.end()) {
        result.below = seam->second;
      }
    }
    return result;
  }

  const TableSchema* welding_schema_ = nullptr;
  const TableLayout* welding_layout_ = nullptr;
  std::uint32_t object_id_offset_ = 0U;
  std::uint32_t common_reference_offset_ = 0U;
  std::optional<std::uint32_t> above_reference_offset_;
  std::optional<std::uint32_t> below_reference_offset_;
  std::unordered_map<std::uint32_t, WeldCommonSemantics> common_rows_;
  std::unordered_map<std::uint32_t, WeldSeamSemantics> seam_rows_;
};

[[nodiscard]] Result<std::optional<WeldDecoder>> load_weld_decoder(const ModelStorage& storage,
                                                                   const Schema& schema,
                                                                   bool include_seams) {
  const auto* welding = schema.find_table("welding");
  const auto* common = schema.find_table("welding_common_attr");
  if (welding == nullptr || common == nullptr || welding->ordinal >= storage.layout.tables.size() ||
      common->ordinal >= storage.layout.tables.size()) {
    return Result<std::optional<WeldDecoder>>::success(std::nullopt);
  }
  const auto& welding_layout = storage.layout.tables[welding->ordinal];
  const auto& common_layout = storage.layout.tables[common->ordinal];
  if (welding_layout.info.row_count == 0U || common_layout.info.row_count == 0U) {
    return Result<std::optional<WeldDecoder>>::success(std::nullopt);
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
    return Result<std::optional<WeldDecoder>>::failure(error);
  }
  const auto* intermittent_type = find_field(schema, *common, "intermittent_type");
  if (intermittent_type != nullptr &&
      (intermittent_type->type != FieldType::u32 || intermittent_type->size != 4U ||
       intermittent_type->offset + intermittent_type->size > common->tuple_size)) {
    return Result<std::optional<WeldDecoder>>::failure(
        {ErrorCode::schema_mismatch,
         "The generated weld field 'intermittent_type' is unavailable."});
  }

  WeldDecoder decoder;
  decoder.welding_schema_ = welding;
  decoder.welding_layout_ = &welding_layout;
  decoder.object_id_offset_ = weld_id.value()->offset;
  decoder.common_reference_offset_ = common_reference.value()->offset;
  decoder.common_rows_.reserve(static_cast<std::size_t>(common_layout.info.row_count));
  const auto payload = storage.payload.bytes();
  for (std::uint64_t row = 0; row < common_layout.info.row_count; ++row) {
    const auto record = common_layout.record(payload, row);
    if (record.empty()) {
      return Result<std::optional<WeldDecoder>>::failure(
          {ErrorCode::invalid_container, "A weld-common record lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
    const auto tuple = record.subspan(1U, common->tuple_size);
    const auto workshop_value = read_u32(tuple, workshop.value()->offset);
    WeldCommonSemantics decoded;
    decoded.location = workshop_value == 1U ? WeldLocation::workshop : WeldLocation::site;
    decoded.workshop = workshop_value;
    decoded.around = read_u32(tuple, around.value()->offset);
    decoded.compound = read_u32(tuple, compound.value()->offset);
    decoded.logical = read_u32(tuple, logical.value()->offset);
    if (intermittent_type != nullptr) {
      decoded.intermittent_type = read_u32(tuple, intermittent_type->offset);
    }
    decoder.common_rows_.insert_or_assign(read_u32(tuple, common_id.value()->offset), decoded);
  }

  if (!include_seams) {
    return Result<std::optional<WeldDecoder>>::success(std::move(decoder));
  }
  const auto* seams = schema.find_table("welding_attr");
  if (seams == nullptr || seams->ordinal >= storage.layout.tables.size() ||
      storage.layout.tables[seams->ordinal].info.row_count == 0U) {
    return Result<std::optional<WeldDecoder>>::success(std::move(decoder));
  }
  auto seam_id = required_u32(schema, *seams, "id");
  auto seam_type = required_u32(schema, *seams, "type");
  auto seam_intermittent = required_u32(schema, *seams, "intermittent");
  const auto* seam_size = find_field(schema, *seams, "size");
  auto above = required_u32(schema, *welding, "weld_seam1_id");
  auto below = required_u32(schema, *welding, "weld_seam2_id");
  if (!seam_id || !seam_type || !seam_intermittent || seam_size == nullptr ||
      seam_size->type != FieldType::f32 || seam_size->size != 4U ||
      seam_size->offset + seam_size->size > seams->tuple_size || !above || !below) {
    if (!seam_id) return Result<std::optional<WeldDecoder>>::failure(seam_id.error());
    if (!seam_type) return Result<std::optional<WeldDecoder>>::failure(seam_type.error());
    if (!seam_intermittent)
      return Result<std::optional<WeldDecoder>>::failure(seam_intermittent.error());
    if (!above) return Result<std::optional<WeldDecoder>>::failure(above.error());
    if (!below) return Result<std::optional<WeldDecoder>>::failure(below.error());
    return Result<std::optional<WeldDecoder>>::failure(
        {ErrorCode::schema_mismatch, "The generated weld field 'size' is unavailable."});
  }
  decoder.above_reference_offset_ = above.value()->offset;
  decoder.below_reference_offset_ = below.value()->offset;
  const auto& seam_layout = storage.layout.tables[seams->ordinal];
  decoder.seam_rows_.reserve(static_cast<std::size_t>(seam_layout.info.row_count));
  for (std::uint64_t row = 0; row < seam_layout.info.row_count; ++row) {
    const auto record = seam_layout.record(payload, row);
    if (record.empty()) {
      return Result<std::optional<WeldDecoder>>::failure(
          {ErrorCode::invalid_container, "A weld-seam record lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
    const auto tuple = record.subspan(1U, seams->tuple_size);
    decoder.seam_rows_.insert_or_assign(
        read_u32(tuple, seam_id.value()->offset),
        WeldSeamSemantics{.size = static_cast<double>(read_f32(tuple, seam_size->offset)),
                          .type = read_u32(tuple, seam_type.value()->offset),
                          .intermittent = read_u32(tuple, seam_intermittent.value()->offset)});
  }
  return Result<std::optional<WeldDecoder>>::success(std::move(decoder));
}

class WeldSemanticReader final : public BatchReader {
 public:
  WeldSemanticReader(std::shared_ptr<const ModelStorage> storage, WeldDecoder decoder,
                     std::size_t batch_size)
      : storage_(std::move(storage)), decoder_(std::move(decoder)), batch_size_(batch_size) {
    properties_.reserve(batch_size_ * 11U);
  }

  Result<BatchView> next() override {
    properties_.clear();
    std::size_t weld_count = 0U;
    const auto payload = storage_->payload.bytes();
    while (row_ < decoder_.welding_layout_->info.row_count && weld_count < batch_size_) {
      const auto record = decoder_.welding_layout_->record(payload, row_++);
      if (record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A welding record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto decoded =
          decoder_.decode(record.subspan(1U, decoder_.welding_schema_->tuple_size));
      if (!decoded.has_value()) continue;
      append(*decoded);
      ++weld_count;
    }
    if (properties_.empty()) return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    return Result<BatchView>::success(
        BatchView{.kind = BatchKind::properties, .properties = properties_});
  }

 private:
  void append(const WeldSemantics& weld) {
    const auto add_integer = [&](std::string_view name, std::int64_t value) {
      properties_.push_back(PropertyView{.object_id = weld.object_id,
                                         .group = "Tekla",
                                         .name = name,
                                         .kind = PropertyValueKind::integer,
                                         .integer_value = value});
    };
    const auto add_floating = [&](std::string_view name, double value) {
      if (!std::isfinite(value)) return;
      properties_.push_back(PropertyView{.object_id = weld.object_id,
                                         .group = "Tekla",
                                         .name = name,
                                         .kind = PropertyValueKind::floating,
                                         .floating_value = value});
    };
    add_integer("weldShop", weld.common.workshop);
    add_integer("weldAround", weld.common.around);
    add_integer("weldCompound", weld.common.compound);
    add_integer("weldLogical", weld.common.logical);
    if (weld.common.intermittent_type.has_value()) {
      add_integer("weldIntermittentType", *weld.common.intermittent_type);
    }
    if (weld.above.has_value()) {
      add_floating("weldSizeAbove", weld.above->size);
      add_integer("weldTypeAbove", weld.above->type);
      add_integer("weldIntermittentAbove", weld.above->intermittent);
    }
    if (weld.below.has_value()) {
      add_floating("weldSizeBelow", weld.below->size);
      add_integer("weldTypeBelow", weld.below->type);
      add_integer("weldIntermittentBelow", weld.below->intermittent);
    }
  }

  std::shared_ptr<const ModelStorage> storage_;
  WeldDecoder decoder_;
  std::size_t batch_size_ = 0U;
  std::uint64_t row_ = 0U;
  std::vector<PropertyView> properties_;
};

}  // namespace

Result<std::unordered_map<std::uint32_t, WeldLocation>> load_weld_locations(
    const ModelStorage& storage, const Schema& schema) {
  std::unordered_map<std::uint32_t, WeldLocation> result;
  auto loaded = load_weld_decoder(storage, schema, false);
  if (!loaded) {
    return Result<std::unordered_map<std::uint32_t, WeldLocation>>::failure(loaded.error());
  }
  if (!loaded.value().has_value()) {
    return Result<std::unordered_map<std::uint32_t, WeldLocation>>::success(std::move(result));
  }
  auto& decoder = *loaded.value();
  result.reserve(static_cast<std::size_t>(decoder.welding_layout_->info.row_count));
  const auto payload = storage.payload.bytes();
  for (std::uint64_t row = 0; row < decoder.welding_layout_->info.row_count; ++row) {
    const auto record = decoder.welding_layout_->record(payload, row);
    if (record.empty()) {
      return Result<std::unordered_map<std::uint32_t, WeldLocation>>::failure(
          {ErrorCode::invalid_container, "A welding record lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
    const auto decoded = decoder.decode(record.subspan(1U, decoder.welding_schema_->tuple_size));
    if (decoded.has_value()) {
      result.insert_or_assign(decoded->object_id, decoded->common.location);
    }
  }
  return Result<std::unordered_map<std::uint32_t, WeldLocation>>::success(std::move(result));
}

Result<ProcessStream> make_weld_semantic_stream(std::shared_ptr<const ModelStorage> storage,
                                                const Schema& schema,
                                                const ProcessRequest& request) {
  auto loaded = load_weld_decoder(*storage, schema, true);
  if (!loaded) return Result<ProcessStream>::failure(loaded.error());
  if (!loaded.value().has_value()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The weld semantic tables are unavailable."});
  }
  constexpr std::uint64_t properties_per_weld = 11U;
  const auto estimate = properties_per_weld * sizeof(PropertyView);
  const auto batch_size = request.batch_memory_budget_bytes == 0U
                              ? 4096U
                              : static_cast<std::size_t>(std::clamp<std::uint64_t>(
                                    request.batch_memory_budget_bytes / estimate, 1U, 65536U));
  return Result<ProcessStream>::success(std::make_unique<WeldSemanticReader>(
      std::move(storage), std::move(*loaded.value()), batch_size));
}

}  // namespace tekla::db1::detail
