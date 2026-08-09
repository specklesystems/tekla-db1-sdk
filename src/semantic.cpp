#include "semantic.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "identity.hpp"
#include "record.hpp"
#include "role_schema.hpp"
#include "weld_semantics.hpp"

namespace tekla::db1::detail {
namespace {

struct Definition {
  PropertyValueKind kind = PropertyValueKind::text;
  std::string_view name;
  std::int64_t integer_value = 0;
  double floating_value = 0.0;
  std::string_view text_value;
  std::uint64_t reference_id = 0;
};

struct LinkSource {
  const TableSchema* schema = nullptr;
  const TableLayout* layout = nullptr;
  PropertyValueKind kind = PropertyValueKind::text;
  std::uint32_t definition_id_offset = 0;
  std::uint32_t father_id_offset = 0;
};

struct PropertyKey {
  std::uint64_t object_id = 0;
  std::string_view name;

  friend bool operator==(const PropertyKey&, const PropertyKey&) = default;
};

struct PropertyKeyHash {
  [[nodiscard]] std::size_t operator()(const PropertyKey& key) const noexcept {
    const auto object_hash = std::hash<std::uint64_t>{}(key.object_id);
    const auto name_hash = std::hash<std::string_view>{}(key.name);
    return object_hash ^ (name_hash + 0x9e3779b9U + (object_hash << 6U) + (object_hash >> 2U));
  }
};

struct PropertyWinner {
  std::size_t source = 0;
  std::uint64_t row = 0;
  std::uint8_t precedence = 0;
  std::uint32_t occurrences = 0;
};

using PropertyWinners = std::unordered_map<PropertyKey, PropertyWinner, PropertyKeyHash>;

struct TransparentStringHash {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
};

struct ComponentDefinitions {
  bool checked = false;
  using Names = std::unordered_set<std::string, TransparentStringHash, std::equal_to<>>;

  [[nodiscard]] bool contains(InstanceKind kind, std::string_view name) const {
    const auto& definitions = kind == InstanceKind::joint ? joint_names : macro_names;
    return definitions.contains(name);
  }

  Names joint_names;
  Names macro_names;
};

[[nodiscard]] constexpr bool contains(Stage stages, Stage stage) noexcept {
  return (static_cast<std::uint32_t>(stages) & static_cast<std::uint32_t>(stage)) != 0;
}

[[nodiscard]] Result<const FieldSchema*> required_field(const Schema& schema,
                                                        const TableSchema& table,
                                                        std::string_view name, FieldType type) {
  const auto* field = find_field(schema, table, name);
  if (field == nullptr || field->type != type || field->offset + field->size > table.tuple_size) {
    return Result<const FieldSchema*>::failure(
        {ErrorCode::schema_mismatch,
         "The generated semantic field '" + std::string(name) + "' is unavailable."});
  }
  return Result<const FieldSchema*>::success(field);
}

[[nodiscard]] std::size_t batch_size_for(const ProcessRequest& request,
                                         std::uint64_t estimate) noexcept {
  if (request.batch_memory_budget_bytes == 0) {
    return 4096;
  }
  return static_cast<std::size_t>(
      std::clamp<std::uint64_t>(request.batch_memory_budget_bytes / estimate, 1, 65536));
}

[[nodiscard]] Result<std::unordered_map<std::uint32_t, Definition>> load_definitions(
    const ModelStorage& storage, const Schema& schema,
    std::span<const std::string_view> table_names, PropertyValueKind kind) {
  std::unordered_map<std::uint32_t, Definition> result;
  for (const auto table_name : table_names) {
    const auto* table_schema = schema.find_table(table_name);
    if (table_schema == nullptr || table_schema->ordinal >= storage.layout.tables.size()) {
      continue;
    }
    const auto& table = storage.layout.tables[table_schema->ordinal];
    if (table.info.row_count == 0) {
      continue;
    }
    auto id = required_field(schema, *table_schema, "id", FieldType::u32);
    auto name = required_field(schema, *table_schema, "name", FieldType::text);
    const auto* value = find_field(schema, *table_schema, "value");
    const auto* type = find_field(schema, *table_schema, "type");
    if (!id || !name || value == nullptr ||
        value->offset + value->size > table_schema->tuple_size) {
      return Result<std::unordered_map<std::uint32_t, Definition>>::failure(
          {ErrorCode::schema_mismatch,
           "An attribute definition table has an incomplete generated layout."});
    }
    for (std::uint64_t row = 0; row < table.info.row_count; ++row) {
      const auto record = table.record(storage.payload.bytes(), row);
      if (record.empty()) {
        return Result<std::unordered_map<std::uint32_t, Definition>>::failure(
            {ErrorCode::invalid_container,
             "An attribute definition record lies outside the payload."});
      }
      const auto tuple = record.subspan(1, table_schema->tuple_size);
      Definition definition;
      definition.kind = kind;
      definition.name = read_text(tuple, name.value()->offset, name.value()->size);
      if (kind == PropertyValueKind::text) {
        if (value->type != FieldType::text) {
          return Result<std::unordered_map<std::uint32_t, Definition>>::failure(
              {ErrorCode::schema_mismatch, "A string attribute value is not text."});
        }
        definition.text_value = read_text(tuple, value->offset, value->size);
      } else if (kind == PropertyValueKind::reference) {
        if (value->type != FieldType::u32 || value->size != 4) {
          return Result<std::unordered_map<std::uint32_t, Definition>>::failure(
              {ErrorCode::schema_mismatch, "A reference attribute value is not an ID."});
        }
        definition.reference_id = read_u32(tuple, value->offset);
      } else {
        if (value->type == FieldType::f64 && value->size == 8) {
          definition.floating_value = read_f64(tuple, value->offset);
        } else if (value->type == FieldType::f32 && value->size == 4) {
          definition.floating_value = read_f32(tuple, value->offset);
        } else {
          return Result<std::unordered_map<std::uint32_t, Definition>>::failure(
              {ErrorCode::schema_mismatch, "A numeric attribute value is not numeric."});
        }
        if (type != nullptr && type->type == FieldType::u32 && read_u32(tuple, type->offset) == 0 &&
            std::isfinite(definition.floating_value) &&
            definition.floating_value >=
                static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            definition.floating_value <=
                static_cast<double>(std::numeric_limits<std::int64_t>::max()) &&
            std::trunc(definition.floating_value) == definition.floating_value) {
          definition.kind = PropertyValueKind::integer;
          definition.integer_value = static_cast<std::int64_t>(definition.floating_value);
        }
      }
      result.insert_or_assign(read_u32(tuple, id.value()->offset), definition);
    }
  }
  return Result<std::unordered_map<std::uint32_t, Definition>>::success(std::move(result));
}

[[nodiscard]] constexpr std::uint8_t property_precedence(PropertyValueKind kind) noexcept {
  switch (kind) {
    case PropertyValueKind::reference:
      return 3;
    case PropertyValueKind::integer:
    case PropertyValueKind::floating:
      return 2;
    case PropertyValueKind::text:
      return 1;
  }
  return 0;
}

[[nodiscard]] const std::unordered_map<std::uint32_t, Definition>& definitions_for(
    const LinkSource& source, const std::unordered_map<std::uint32_t, Definition>& numeric,
    const std::unordered_map<std::uint32_t, Definition>& strings,
    const std::unordered_map<std::uint32_t, Definition>& references) {
  if (source.kind == PropertyValueKind::floating) return numeric;
  if (source.kind == PropertyValueKind::reference) return references;
  return strings;
}

[[nodiscard]] Result<PropertyWinners> find_property_winners(
    const ModelStorage& storage, std::span<const LinkSource> sources,
    const std::unordered_map<std::uint32_t, Definition>& numeric,
    const std::unordered_map<std::uint32_t, Definition>& strings,
    const std::unordered_map<std::uint32_t, Definition>& references) {
  std::unordered_map<std::string_view, std::uint32_t> definition_counts;
  definition_counts.reserve(numeric.size() + strings.size() + references.size());
  const auto count_definitions = [&](const auto& definitions) {
    for (const auto& [id, definition] : definitions) {
      static_cast<void>(id);
      ++definition_counts[definition.name];
    }
  };
  count_definitions(references);
  count_definitions(numeric);
  count_definitions(strings);

  std::unordered_set<std::string_view> ambiguous_names;
  ambiguous_names.reserve(definition_counts.size());
  for (const auto& [name, count] : definition_counts) {
    if (count > 1U) ambiguous_names.insert(name);
  }
  if (ambiguous_names.empty()) return Result<PropertyWinners>::success({});

  PropertyWinners winners;
  const auto payload = storage.payload.bytes();
  for (std::size_t source_index = 0; source_index < sources.size(); ++source_index) {
    const auto& source = sources[source_index];
    const auto& definitions = definitions_for(source, numeric, strings, references);
    for (std::uint64_t row = 0; row < source.layout->info.row_count; ++row) {
      const auto record = source.layout->record(payload, row);
      if (record.empty()) {
        return Result<PropertyWinners>::failure(
            {ErrorCode::invalid_container, "An attribute link lies outside the payload."});
      }
      const auto tuple = record.subspan(1, source.schema->tuple_size);
      const auto definition = definitions.find(read_u32(tuple, source.definition_id_offset));
      if (definition == definitions.end() || !ambiguous_names.contains(definition->second.name)) {
        continue;
      }
      const PropertyKey key{read_u32(tuple, source.father_id_offset), definition->second.name};
      const auto precedence = property_precedence(definition->second.kind);
      const auto [winner, inserted] =
          winners.try_emplace(key, PropertyWinner{source_index, row, precedence, 1U});
      if (inserted) continue;
      ++winner->second.occurrences;
      if (precedence > winner->second.precedence) {
        winner->second.source = source_index;
        winner->second.row = row;
        winner->second.precedence = precedence;
      }
    }
  }
  std::erase_if(winners, [](const auto& winner) { return winner.second.occurrences < 2U; });
  return Result<PropertyWinners>::success(std::move(winners));
}

class PropertyReader final : public BatchReader {
 public:
  PropertyReader(std::shared_ptr<const ModelStorage> storage, std::vector<LinkSource> sources,
                 std::unordered_map<std::uint32_t, Definition> numeric,
                 std::unordered_map<std::uint32_t, Definition> strings,
                 std::unordered_map<std::uint32_t, Definition> references, PropertyWinners winners,
                 std::size_t batch_size)
      : storage_(std::move(storage)),
        sources_(std::move(sources)),
        numeric_(std::move(numeric)),
        strings_(std::move(strings)),
        references_(std::move(references)),
        winners_(std::move(winners)),
        batch_size_(batch_size) {
    batch_.reserve(batch_size_);
  }

  Result<BatchView> next() override {
    batch_.clear();
    const auto payload = storage_->payload.bytes();
    while (source_ < sources_.size() && batch_.size() < batch_size_) {
      const auto& source = sources_[source_];
      const auto& definitions = definitions_for(source, numeric_, strings_, references_);
      while (row_ < source.layout->info.row_count && batch_.size() < batch_size_) {
        const auto current_row = row_++;
        const auto record = source.layout->record(payload, current_row);
        if (record.empty()) {
          return Result<BatchView>::failure(
              {ErrorCode::invalid_container, "An attribute link lies outside the payload."});
        }
        const auto tuple = record.subspan(1, source.schema->tuple_size);
        const auto definition_id = read_u32(tuple, source.definition_id_offset);
        const auto definition = definitions.find(definition_id);
        if (definition == definitions.end()) {
          continue;
        }
        const auto& value = definition->second;
        const auto object_id = read_u32(tuple, source.father_id_offset);
        if (const auto winner = winners_.find(PropertyKey{object_id, value.name});
            winner != winners_.end() &&
            (winner->second.source != source_ || winner->second.row != current_row)) {
          continue;
        }
        batch_.push_back(PropertyView{
            .object_id = object_id,
            .group = "User Defined Attributes",
            .name = value.name,
            .kind = value.kind,
            .integer_value = value.integer_value,
            .floating_value = value.floating_value,
            .text_value = value.text_value,
            .reference_id = value.reference_id,
        });
      }
      if (row_ >= source.layout->info.row_count) {
        ++source_;
        row_ = 0;
      }
    }
    if (batch_.empty() && source_ >= sources_.size()) {
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    }
    return Result<BatchView>::success(
        BatchView{.kind = BatchKind::properties, .properties = batch_});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  std::vector<LinkSource> sources_;
  std::unordered_map<std::uint32_t, Definition> numeric_;
  std::unordered_map<std::uint32_t, Definition> strings_;
  std::unordered_map<std::uint32_t, Definition> references_;
  PropertyWinners winners_;
  std::size_t batch_size_ = 0;
  std::size_t source_ = 0;
  std::uint64_t row_ = 0;
  std::vector<PropertyView> batch_;
};

class RelationReader final : public BatchReader {
 public:
  RelationReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& layout,
                 const TableSchema& schema, std::array<std::uint32_t, 5> offsets,
                 std::size_t batch_size)
      : storage_(std::move(storage)),
        layout_(&layout),
        schema_(&schema),
        offsets_(offsets),
        batch_size_(batch_size) {
    batch_.reserve(batch_size_);
  }

  Result<BatchView> next() override {
    if (row_ >= layout_->info.row_count) {
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    }
    batch_.clear();
    const auto payload = storage_->payload.bytes();
    while (row_ < layout_->info.row_count && batch_.size() < batch_size_) {
      const auto record = layout_->record(payload, row_++);
      if (record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A relation record lies outside the payload."});
      }
      const auto tuple = record.subspan(1, schema_->tuple_size);
      batch_.push_back(RelationView{
          .relation_id = read_u32(tuple, offsets_[0]),
          .type = read_u32(tuple, offsets_[1]),
          .source_id = read_u32(tuple, offsets_[2]),
          .target_id = read_u32(tuple, offsets_[3]),
          .flags = read_u32(tuple, offsets_[4]),
          .row_id = read_u32(record, 1 + schema_->tuple_size),
          .event_id = read_u32(record, 1 + schema_->tuple_size + 4),
          .visible = (std::to_integer<std::uint8_t>(record[0]) & 0x08U) == 0,
      });
    }
    return Result<BatchView>::success(BatchView{.kind = BatchKind::relations, .relations = batch_});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* layout_ = nullptr;
  const TableSchema* schema_ = nullptr;
  std::array<std::uint32_t, 5> offsets_{};
  std::size_t batch_size_ = 0;
  std::uint64_t row_ = 0;
  std::vector<RelationView> batch_;
};

struct SemanticObject {
  std::uint32_t id = 0;
  std::uint32_t parent_id = 0;
  ObjectKind kind = ObjectKind::unknown;
};

struct SemanticEdgeKey {
  std::uint32_t source_id = 0;
  std::uint32_t target_id = 0;

  friend bool operator==(const SemanticEdgeKey&, const SemanticEdgeKey&) = default;
};

struct SemanticEdgeKeyHash {
  [[nodiscard]] std::size_t operator()(const SemanticEdgeKey& key) const noexcept {
    auto hash = std::hash<std::uint32_t>{}(key.source_id);
    hash ^= std::hash<std::uint32_t>{}(key.target_id) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
    return hash;
  }
};

using AssemblyMembers = std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>;

struct PourMembership {
  std::uint32_t pour_object_id = 0U;
  std::uint32_t pour_unit_id = 0U;
};

struct RebarSpliceConnection {
  std::uint32_t splice_id = 0U;
  std::uint32_t first_reinforcement_id = 0U;
  std::uint32_t second_reinforcement_id = 0U;
};

class SemanticRelationReader final : public BatchReader {
 public:
  SemanticRelationReader(
      std::shared_ptr<const ModelStorage> storage, std::vector<SemanticObject> objects,
      std::unordered_set<std::uint32_t> endpoints, const TableLayout* relation_layout,
      const TableSchema* relation_schema, std::array<std::uint32_t, 4> relation_offsets,
      const TableLayout* joint_layout, const TableSchema* joint_schema,
      std::array<std::uint32_t, 3> joint_offsets, std::vector<std::uint32_t> assembly_order,
      std::unordered_map<std::uint32_t, std::uint32_t> main_members,
      AssemblyMembers assembly_members, std::vector<PourMembership> pour_memberships,
      std::vector<RebarSpliceConnection> rebar_splices, std::size_t batch_size)
      : storage_(std::move(storage)),
        objects_(std::move(objects)),
        endpoints_(std::move(endpoints)),
        relation_layout_(relation_layout),
        relation_schema_(relation_schema),
        relation_offsets_(relation_offsets),
        joint_layout_(joint_layout),
        joint_schema_(joint_schema),
        joint_offsets_(joint_offsets),
        assembly_order_(std::move(assembly_order)),
        main_members_(std::move(main_members)),
        assembly_members_(std::move(assembly_members)),
        pour_memberships_(std::move(pour_memberships)),
        rebar_splices_(std::move(rebar_splices)),
        batch_size_(batch_size) {
    batch_.reserve(batch_size_);
    subelements_.reserve(objects_.size());
    kinds_.reserve(objects_.size());
    for (const auto& object : objects_) kinds_.emplace(object.id, object.kind);
  }

  Result<BatchView> next() override {
    batch_.clear();
    while (batch_.size() < batch_size_ && phase_ != Phase::done) {
      if (phase_ == Phase::parents) {
        emit_parents();
      } else if (phase_ == Phase::stored_relations) {
        auto emitted = emit_stored_relations();
        if (!emitted) return Result<BatchView>::failure(emitted.error());
      } else if (phase_ == Phase::assemblies) {
        emit_assembly_memberships();
      } else if (phase_ == Phase::pours) {
        emit_pour_memberships();
      } else if (phase_ == Phase::rebar_splices) {
        emit_rebar_splice_connections();
      } else {
        auto emitted = emit_component_connections();
        if (!emitted) return Result<BatchView>::failure(emitted.error());
      }
    }
    if (batch_.empty() && phase_ == Phase::done) {
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    }
    return Result<BatchView>::success(
        BatchView{.kind = BatchKind::semantic_relations, .semantic_relations = batch_});
  }

 private:
  enum class Phase {
    parents,
    stored_relations,
    assemblies,
    pours,
    rebar_splices,
    component_connections,
    done,
  };

  [[nodiscard]] bool valid_edge(std::uint32_t source, std::uint32_t target) const {
    if (source == 0U || target == 0U || source == target || !endpoints_.contains(source) ||
        !endpoints_.contains(target)) {
      return false;
    }
    const auto source_kind = kinds_.find(source);
    const auto target_kind = kinds_.find(target);
    return source_kind != kinds_.end() && target_kind != kinds_.end() &&
           is_model_element(source_kind->second) && is_model_element(target_kind->second);
  }

  bool append_subelement(std::uint32_t parent, std::uint32_t child,
                         SemanticRelationOrigin origin, std::uint32_t source_relation_id = 0U) {
    if (!valid_edge(parent, child) || !subelements_.insert({parent, child}).second) return false;
    batch_.push_back(SemanticRelationView{
        .kind = SemanticRelationKind::subelement,
        .source_id = parent,
        .target_id = child,
        .ordinal = child_ordinals_[parent]++,
        .origin = origin,
        .source_relation_id = source_relation_id,
    });
    return true;
  }

  [[nodiscard]] static bool is_reinforcement(ObjectKind kind) noexcept {
    return kind == ObjectKind::single_rebar || kind == ObjectKind::rebar_group ||
           kind == ObjectKind::rebar_mesh;
  }

  [[nodiscard]] static bool is_host(ObjectKind kind) noexcept {
    return kind == ObjectKind::beam || kind == ObjectKind::contour_plate ||
           kind == ObjectKind::poly_beam || kind == ObjectKind::brep ||
           kind == ObjectKind::lofted_plate || kind == ObjectKind::connection ||
           kind == ObjectKind::component;
  }

  bool append_hosted_on(std::uint32_t reinforcement, std::uint32_t host,
                        std::uint32_t source_relation_id) {
    if (!valid_edge(reinforcement, host) || !hosted_.insert({reinforcement, host}).second) {
      return false;
    }
    const auto reinforcement_kind = kinds_.find(reinforcement);
    const auto host_kind = kinds_.find(host);
    if (reinforcement_kind == kinds_.end() || host_kind == kinds_.end() ||
        !is_reinforcement(reinforcement_kind->second) || !is_host(host_kind->second)) {
      hosted_.erase({reinforcement, host});
      return false;
    }
    batch_.push_back(SemanticRelationView{
        .kind = SemanticRelationKind::hosted_on,
        .source_id = reinforcement,
        .target_id = host,
        .ordinal = 0U,
        .origin = SemanticRelationOrigin::rebar_host,
        .source_relation_id = source_relation_id,
    });
    return true;
  }

  bool append_connects_to(
      std::uint32_t primary, std::uint32_t secondary, std::uint32_t source_relation_id,
      SemanticRelationOrigin origin = SemanticRelationOrigin::component_connection,
      std::uint32_t ordinal = 0U) {
    if (!valid_edge(primary, secondary) || !connections_.insert({primary, secondary}).second) {
      return false;
    }
    batch_.push_back(SemanticRelationView{
        .kind = SemanticRelationKind::connects_to,
        .source_id = primary,
        .target_id = secondary,
        .ordinal = ordinal,
        .origin = origin,
        .source_relation_id = source_relation_id,
    });
    return true;
  }

  void emit_parents() {
    while (object_offset_ < objects_.size() && batch_.size() < batch_size_) {
      const auto& object = objects_[object_offset_++];
      append_subelement(object.parent_id, object.id, SemanticRelationOrigin::object_parent);
    }
    if (object_offset_ == objects_.size()) phase_ = Phase::stored_relations;
  }

  Result<bool> emit_stored_relations() {
    if (relation_layout_ == nullptr || relation_schema_ == nullptr) {
      phase_ = Phase::assemblies;
      return Result<bool>::success(false);
    }
    const auto payload = storage_->payload.bytes();
    while (relation_row_ < relation_layout_->info.row_count && batch_.size() < batch_size_) {
      const auto record = relation_layout_->record(payload, relation_row_++);
      if (record.empty()) {
        return Result<bool>::failure(
            {ErrorCode::invalid_container, "A relation record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0) continue;
      const auto tuple = record.subspan(1, relation_schema_->tuple_size);
      const auto relation_type = read_u32(tuple, relation_offsets_[1]);
      const auto source = read_u32(tuple, relation_offsets_[2]);
      const auto target = read_u32(tuple, relation_offsets_[3]);
      const auto relation_id = read_u32(tuple, relation_offsets_[0]);
      if (relation_type == 7U || relation_type == 11U || relation_type == 12U ||
          relation_type == 73U) {
        append_subelement(source, target, SemanticRelationOrigin::stored_relation, relation_id);
      } else if (relation_type == 47U) {
        append_hosted_on(target, source, relation_id);
      }
    }
    if (relation_row_ == relation_layout_->info.row_count) phase_ = Phase::assemblies;
    return Result<bool>::success(true);
  }

  void emit_assembly_memberships() {
    while (assembly_offset_ < assembly_order_.size() && batch_.size() < batch_size_) {
      const auto assembly = assembly_order_[assembly_offset_];
      const auto main = main_members_.at(assembly);
      if (!assembly_initialized_) {
        if (valid_edge(main, assembly)) {
          batch_.push_back(SemanticRelationView{
              .kind = SemanticRelationKind::in_assembly,
              .source_id = main,
              .target_id = assembly,
              .ordinal = 0U,
              .origin = SemanticRelationOrigin::assembly_membership,
          });
        }
        assembly_initialized_ = true;
        next_member_ordinal_ = 1U;
        if (batch_.size() == batch_size_) return;
      }
      const auto members = assembly_members_.find(assembly);
      if (members != assembly_members_.end()) {
        while (member_offset_ < members->second.size() && batch_.size() < batch_size_) {
          const auto member = members->second[member_offset_++];
          if (member == main || member == assembly || !valid_edge(member, assembly)) continue;
          batch_.push_back(SemanticRelationView{
              .kind = SemanticRelationKind::in_assembly,
              .source_id = member,
              .target_id = assembly,
              .ordinal = next_member_ordinal_++,
              .origin = SemanticRelationOrigin::assembly_membership,
          });
        }
        if (member_offset_ < members->second.size()) return;
      }
      ++assembly_offset_;
      member_offset_ = 0U;
      next_member_ordinal_ = 1U;
      assembly_initialized_ = false;
    }
    if (assembly_offset_ == assembly_order_.size()) phase_ = Phase::pours;
  }

  void emit_pour_memberships() {
    while (pour_membership_offset_ < pour_memberships_.size() && batch_.size() < batch_size_) {
      const auto& membership = pour_memberships_[pour_membership_offset_++];
      if (!valid_edge(membership.pour_object_id, membership.pour_unit_id)) continue;
      batch_.push_back(SemanticRelationView{
          .kind = SemanticRelationKind::in_assembly,
          .source_id = membership.pour_object_id,
          .target_id = membership.pour_unit_id,
          .ordinal = 0U,
          .origin = SemanticRelationOrigin::pour_membership,
      });
    }
    if (pour_membership_offset_ == pour_memberships_.size()) {
      phase_ = Phase::rebar_splices;
    }
  }

  void emit_rebar_splice_connections() {
    while (rebar_splice_offset_ < rebar_splices_.size() && batch_.size() < batch_size_) {
      const auto& splice = rebar_splices_[rebar_splice_offset_];
      if (next_splice_endpoint_ == 0U) {
        append_connects_to(splice.splice_id, splice.first_reinforcement_id, 0U,
                           SemanticRelationOrigin::rebar_splice, 0U);
        next_splice_endpoint_ = 1U;
        if (batch_.size() == batch_size_) return;
      }
      append_connects_to(splice.splice_id, splice.second_reinforcement_id, 0U,
                         SemanticRelationOrigin::rebar_splice, 1U);
      next_splice_endpoint_ = 0U;
      ++rebar_splice_offset_;
    }
    if (rebar_splice_offset_ == rebar_splices_.size()) {
      phase_ = Phase::component_connections;
    }
  }

  Result<bool> emit_component_connections() {
    if (joint_layout_ == nullptr || joint_schema_ == nullptr) {
      phase_ = Phase::done;
      return Result<bool>::success(false);
    }
    const auto payload = storage_->payload.bytes();
    while (joint_row_ < joint_layout_->info.row_count && batch_.size() < batch_size_) {
      const auto record = joint_layout_->record(payload, joint_row_++);
      if (record.empty()) {
        return Result<bool>::failure(
            {ErrorCode::invalid_container, "A joint record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, joint_schema_->tuple_size);
      append_connects_to(read_u32(tuple, joint_offsets_[1]), read_u32(tuple, joint_offsets_[2]),
                         read_u32(tuple, joint_offsets_[0]));
    }
    if (joint_row_ == joint_layout_->info.row_count) phase_ = Phase::done;
    return Result<bool>::success(true);
  }

  std::shared_ptr<const ModelStorage> storage_;
  std::vector<SemanticObject> objects_;
  std::unordered_set<std::uint32_t> endpoints_;
  const TableLayout* relation_layout_ = nullptr;
  const TableSchema* relation_schema_ = nullptr;
  std::array<std::uint32_t, 4> relation_offsets_{};
  const TableLayout* joint_layout_ = nullptr;
  const TableSchema* joint_schema_ = nullptr;
  std::array<std::uint32_t, 3> joint_offsets_{};
  std::vector<std::uint32_t> assembly_order_;
  std::unordered_map<std::uint32_t, std::uint32_t> main_members_;
  AssemblyMembers assembly_members_;
  std::vector<PourMembership> pour_memberships_;
  std::vector<RebarSpliceConnection> rebar_splices_;
  std::unordered_set<SemanticEdgeKey, SemanticEdgeKeyHash> subelements_;
  std::unordered_set<SemanticEdgeKey, SemanticEdgeKeyHash> hosted_;
  std::unordered_set<SemanticEdgeKey, SemanticEdgeKeyHash> connections_;
  std::unordered_map<std::uint32_t, ObjectKind> kinds_;
  std::unordered_map<std::uint32_t, std::uint32_t> child_ordinals_;
  std::size_t batch_size_ = 0;
  Phase phase_ = Phase::parents;
  std::size_t object_offset_ = 0U;
  std::uint64_t relation_row_ = 0U;
  std::uint64_t joint_row_ = 0U;
  std::size_t assembly_offset_ = 0U;
  std::size_t pour_membership_offset_ = 0U;
  std::size_t rebar_splice_offset_ = 0U;
  std::size_t member_offset_ = 0U;
  std::uint32_t next_splice_endpoint_ = 0U;
  std::uint32_t next_member_ordinal_ = 1U;
  bool assembly_initialized_ = false;
  std::vector<SemanticRelationView> batch_;
};

class InstanceReader final : public BatchReader {
 public:
  InstanceReader(std::shared_ptr<const ModelStorage> storage, std::vector<InstanceView> instances,
                 std::size_t batch_size)
      : storage_(std::move(storage)), instances_(std::move(instances)), batch_size_(batch_size) {}

  Result<BatchView> next() override {
    if (offset_ >= instances_.size()) {
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    }
    const auto count = std::min(batch_size_, instances_.size() - offset_);
    const auto batch = std::span<const InstanceView>(instances_).subspan(offset_, count);
    offset_ += count;
    return Result<BatchView>::success(BatchView{.kind = BatchKind::instances, .instances = batch});
  }

 private:
  // Instance strings are non-owning views into the immutable model payload.
  std::shared_ptr<const ModelStorage> storage_;
  std::vector<InstanceView> instances_;
  std::size_t batch_size_ = 0;
  std::size_t offset_ = 0;
};

[[nodiscard]] std::unordered_map<std::uint32_t, std::string_view> load_strings(
    const ModelStorage& storage, const Schema& schema) {
  std::unordered_map<std::uint32_t, std::string_view> strings;
  const auto* table_schema = schema.find_table("string");
  if (table_schema == nullptr || table_schema->ordinal >= storage.layout.tables.size()) {
    return strings;
  }
  const auto* id = find_field(schema, *table_schema, "id");
  const auto* value = find_field(schema, *table_schema, "string");
  if (id == nullptr || value == nullptr || id->type != FieldType::u32 ||
      value->type != FieldType::text) {
    return strings;
  }
  const auto& layout = storage.layout.tables[table_schema->ordinal];
  strings.reserve(static_cast<std::size_t>(layout.info.row_count));
  for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
    const auto record = layout.record(storage.payload.bytes(), row);
    if (record.empty()) continue;
    const auto tuple = record.subspan(1, table_schema->tuple_size);
    strings.insert_or_assign(read_u32(tuple, id->offset),
                             read_text(tuple, value->offset, value->size));
  }
  return strings;
}

[[nodiscard]] Result<ComponentDefinitions> load_component_definitions(
    const ModelStorage& storage, const ProcessRequest& request) {
  ComponentDefinitions definitions;
  if (!contains(request.stages, Stage::component_definitions)) {
    return Result<ComponentDefinitions>::success(std::move(definitions));
  }
  definitions.checked = true;
  const auto* asset = storage.package.find_first(AssetRole::component_catalog);
  if (asset == nullptr || asset->source() == nullptr) {
    return Result<ComponentDefinitions>::success(std::move(definitions));
  }

  constexpr std::uint64_t default_catalog_budget = 1ULL * 1024ULL * 1024ULL * 1024ULL;
  const auto budget = request.component_catalog_memory_budget_bytes == 0
                          ? default_catalog_budget
                          : request.component_catalog_memory_budget_bytes;
  auto payload = decode_payload(asset->source(), budget);
  if (!payload) return Result<ComponentDefinitions>::failure(payload.error());
  auto layout = inspect_payload(payload.value(), true);
  if (!layout) return Result<ComponentDefinitions>::failure(layout.error());
  if (layout.value().info.internal_format != storage.layout.info.internal_format ||
      layout.value().info.kind_marker != storage.layout.info.kind_marker ||
      layout.value().info.database_uuid != storage.layout.info.database_uuid) {
    return Result<ComponentDefinitions>::failure(
        {ErrorCode::schema_mismatch,
         "The component catalog does not belong to this model database format and UUID."});
  }
  auto projected = project_keyed_role_schema(layout.value());
  if (!projected) return Result<ComponentDefinitions>::failure(projected.error());

  ModelStorage catalog(ModelPackage{}, std::move(payload.value()), std::move(layout.value()));
  const auto schema = projected.value().view();
  std::unordered_map<std::uint32_t, std::uint8_t> name_kinds;
  for (const auto [table_name, kind_bit] :
       {std::pair{std::string_view{"joint"}, std::uint8_t{1U}},
        std::pair{std::string_view{"macro"}, std::uint8_t{2U}}}) {
    const auto* table_schema = schema.find_table(table_name);
    if (table_schema == nullptr || table_schema->ordinal >= catalog.layout.tables.size()) {
      return Result<ComponentDefinitions>::failure(
          {ErrorCode::schema_mismatch,
           "The component catalog has no compatible " + std::string(table_name) + " table."});
    }
    const auto* name = find_field(schema, *table_schema, "name");
    if (name == nullptr || name->type != FieldType::u32) {
      return Result<ComponentDefinitions>::failure(
          {ErrorCode::schema_mismatch,
           "A component definition table has no generated name reference."});
    }
    const auto& table = catalog.layout.tables[table_schema->ordinal];
    for (std::uint64_t row = 0; row < table.info.row_count; ++row) {
      const auto record = table.record(catalog.payload.bytes(), row);
      if (record.empty()) {
        return Result<ComponentDefinitions>::failure(
            {ErrorCode::invalid_container,
             "A component definition record lies outside the catalog payload."});
      }
      const auto tuple = record.subspan(1, table_schema->tuple_size);
      const auto name_id = read_u32(tuple, name->offset);
      if (name_id != 0U) name_kinds[name_id] |= kind_bit;
    }
  }

  const auto* string_schema = schema.find_table("string");
  if (string_schema == nullptr || string_schema->ordinal >= catalog.layout.tables.size()) {
    return Result<ComponentDefinitions>::failure(
        {ErrorCode::schema_mismatch, "The component catalog has no compatible string table."});
  }
  const auto* string_id = find_field(schema, *string_schema, "id");
  const auto* string_value = find_field(schema, *string_schema, "string");
  if (string_id == nullptr || string_id->type != FieldType::u32 || string_value == nullptr ||
      string_value->type != FieldType::text) {
    return Result<ComponentDefinitions>::failure(
        {ErrorCode::schema_mismatch,
         "The component catalog string table has no generated ID and text fields."});
  }
  const auto& string_table = catalog.layout.tables[string_schema->ordinal];
  for (std::uint64_t row = 0; row < string_table.info.row_count && !name_kinds.empty(); ++row) {
    const auto record = string_table.record(catalog.payload.bytes(), row);
    if (record.empty()) {
      return Result<ComponentDefinitions>::failure(
          {ErrorCode::invalid_container,
           "A component catalog string record lies outside the payload."});
    }
    const auto tuple = record.subspan(1, string_schema->tuple_size);
    const auto reference = name_kinds.find(read_u32(tuple, string_id->offset));
    if (reference == name_kinds.end()) continue;
    const auto value = read_text(tuple, string_value->offset, string_value->size);
    if (!value.empty()) {
      if ((reference->second & 1U) != 0U) definitions.joint_names.emplace(value);
      if ((reference->second & 2U) != 0U) definitions.macro_names.emplace(value);
    }
    name_kinds.erase(reference);
  }
  if (!name_kinds.empty()) {
    return Result<ComponentDefinitions>::failure(
        {ErrorCode::schema_mismatch,
         "A component definition references a missing catalog string."});
  }
  return Result<ComponentDefinitions>::success(std::move(definitions));
}

struct PartAttributeOffsets {
  std::uint32_t id = 0;
  const FieldSchema* name = nullptr;
  const FieldSchema* profile = nullptr;
  const FieldSchema* parameter_string_id = nullptr;
  const FieldSchema* material = nullptr;
  const FieldSchema* finish = nullptr;
  const FieldSchema* object_class = nullptr;
  const FieldSchema* form_type = nullptr;
};

struct ObjectReportData {
  std::uint32_t assembly_id = 0;
  std::uint32_t mask1 = 0;
};

struct AssemblyReportData {
  std::uint32_t main_part_id = 0;
  std::uint32_t flags = 0;
};

struct NumberingReportData {
  std::uint32_t start_number = 0;
  std::uint32_t position = 0;
  std::string_view prefix;
};

struct PartReportLookups {
  std::unordered_map<std::uint32_t, ObjectReportData> objects;
  std::unordered_map<std::uint32_t, AssemblyReportData> assemblies;
  std::unordered_map<std::uint32_t, std::uint32_t> phases;
  std::unordered_map<std::uint32_t, std::uint32_t> numbering_groups;
  std::unordered_map<std::uint32_t, NumberingReportData> part_numbering;
  std::unordered_map<std::uint32_t, NumberingReportData> assembly_numbering;
};

struct MaterialCatalogEntry {
  std::string_view report_type;
  double density_kg_m3 = 0.0;
};

[[nodiscard]] std::optional<std::vector<std::byte>> inflate_gzip(
    std::span<const std::byte> compressed) {
  constexpr std::size_t maximum_size = 64U * 1024U * 1024U;
  if (compressed.size() < 18U || compressed.size() > std::numeric_limits<uInt>::max()) {
    return std::nullopt;
  }
  const auto tail = compressed.size() - 4U;
  std::uint32_t inflated_size = 0U;
  for (unsigned index = 0; index != 4U; ++index) {
    inflated_size |= static_cast<std::uint32_t>(
                         std::to_integer<std::uint8_t>(compressed[tail + index]))
                     << (index * 8U);
  }
  if (inflated_size == 0U || inflated_size > maximum_size) return std::nullopt;
  std::vector<std::byte> output(inflated_size);
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return std::nullopt;
  const int status = inflate(&stream, Z_FINISH);
  const bool valid = status == Z_STREAM_END && stream.total_out == output.size();
  inflateEnd(&stream);
  if (!valid) return std::nullopt;
  return output;
}

[[nodiscard]] std::unordered_map<std::string, MaterialCatalogEntry> load_material_catalog(
    const ModelPackage& package) {
  std::unordered_map<std::string, MaterialCatalogEntry> result;
  const auto* asset = package.find_first(AssetRole::material_catalog);
  if (asset == nullptr) return result;
  const auto inflated = inflate_gzip(asset->source()->bytes());
  if (!inflated || inflated->size() < 12U) return result;
  const auto bytes = std::span<const std::byte>(*inflated);
  const auto version = read_u32(bytes, 0U);
  const auto count = read_u32(bytes, 4U);
  const auto payload_size = read_u32(bytes, 8U);
  constexpr std::size_t record_size = 181U;
  if (version != 3U || payload_size + 1U != record_size ||
      count > (bytes.size() - 12U) / record_size) {
    return result;
  }
  result.reserve(count);
  for (std::uint32_t index = 0; index != count; ++index) {
    const auto offset = 12U + static_cast<std::size_t>(index) * record_size;
    if (std::to_integer<std::uint8_t>(bytes[offset]) != 4U) return {};
    const auto type = read_u32(bytes, offset + 1U);
    const char* raw_name = reinterpret_cast<const char*>(bytes.data() + offset + 5U);
    std::size_t name_size = 0U;
    while (name_size != 32U && raw_name[name_size] != '\0') ++name_size;
    if (name_size == 0U) continue;
    std::string_view report_type;
    if (type == 1U) report_type = "STEEL";
    if (type == 2U) report_type = "CONCRETE";
    if (type == 3U) report_type = "TIMBER";
    if (type == 6U) report_type = "MISCELLANEOUS";
    result.insert_or_assign(
        std::string(raw_name, name_size),
        MaterialCatalogEntry{.report_type = report_type,
                             .density_kg_m3 = read_f64(bytes, offset + 137U)});
  }
  return result;
}

[[nodiscard]] std::string_view fallback_material_report_type(
    std::string_view material) noexcept {
  if (material == "DUMMY") return "MISCELLANEOUS";
  if (material == "8.8" || material.starts_with("Steel") ||
      (material.size() > 1U && material[0] == 'S' &&
       std::isdigit(static_cast<unsigned char>(material[1])) != 0)) {
    return "STEEL";
  }
  if (material.size() > 1U && material[0] == 'C' &&
      std::isdigit(static_cast<unsigned char>(material[1])) != 0) {
    return "CONCRETE";
  }
  return {};
}

[[nodiscard]] std::string_view profile_report_type(std::string_view family,
                                                   std::uint32_t form_type) noexcept {
  if (family.starts_with("BL") || family.starts_with("HWR") || form_type == 70U) return "B";
  if (family.starts_with("HEA") || family.starts_with("IPE")) return "I";
  if (family.starts_with("THYSSEN-T") || family.starts_with("PL_V")) return "Z";
  if (family.starts_with("RO")) return "RO";
  if (family.starts_with("GS-M") || family.starts_with("D")) return "RU";
  if (family.starts_with("L")) return "L";
  if (family.starts_with("U")) return "U";
  return {};
}

class PartSemanticReader final : public BatchReader {
 public:
  PartSemanticReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& parts,
                     const TableSchema& part_schema, std::uint32_t part_id_offset,
                     std::uint32_t attribute_id_offset, const TableLayout& attributes,
                     const TableSchema& attribute_schema, PartAttributeOffsets offsets,
                     std::unordered_map<std::uint32_t, std::uint64_t> attribute_rows,
                     std::unordered_map<std::uint32_t, std::string> strings,
                     PartReportLookups report,
                     std::unordered_map<std::string, MaterialCatalogEntry> material_catalog,
                     std::size_t part_batch_size)
      : storage_(std::move(storage)),
        parts_(&parts),
        part_schema_(&part_schema),
        part_id_offset_(part_id_offset),
        attribute_id_offset_(attribute_id_offset),
        attributes_(&attributes),
        attribute_schema_(&attribute_schema),
        offsets_(offsets),
        attribute_rows_(std::move(attribute_rows)),
        strings_(std::move(strings)),
        report_(std::move(report)),
        material_catalog_(std::move(material_catalog)),
        part_batch_size_(part_batch_size) {
    properties_.reserve(part_batch_size_ * 12U);
    materials_.reserve(part_batch_size_);
    profile_displays_.reserve(part_batch_size_);
    assembly_positions_.reserve(part_batch_size_);
  }

  Result<BatchView> next() override {
    if (emit_materials_) {
      emit_materials_ = false;
      return Result<BatchView>::success(
          BatchView{.kind = BatchKind::materials, .materials = materials_});
    }
    if (row_ >= parts_->info.row_count) {
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    }
    properties_.clear();
    materials_.clear();
    profile_displays_.clear();
    assembly_positions_.clear();
    const auto payload = storage_->payload.bytes();
    std::size_t part_count = 0;
    while (row_ < parts_->info.row_count && part_count < part_batch_size_) {
      const auto part_record = parts_->record(payload, row_++);
      if (part_record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A part record lies outside the payload."});
      }
      const auto part_tuple = part_record.subspan(1, part_schema_->tuple_size);
      const auto object_id = read_u32(part_tuple, part_id_offset_);
      const auto attribute_id = read_u32(part_tuple, attribute_id_offset_);
      const auto found = attribute_rows_.find(attribute_id);
      if (found == attribute_rows_.end()) {
        continue;
      }
      const auto attribute_record = attributes_->record(payload, found->second);
      if (attribute_record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A part attribute lies outside the payload."});
      }
      const auto tuple = attribute_record.subspan(1, attribute_schema_->tuple_size);
      const auto text = [&](const FieldSchema* field) -> std::string_view {
        return field == nullptr ? std::string_view{} : read_text(tuple, field->offset, field->size);
      };
      const auto add_text = [&](std::string_view group, std::string_view name,
                                std::string_view value) {
        if (!value.empty()) {
          properties_.push_back(PropertyView{.object_id = object_id,
                                             .group = group,
                                             .name = name,
                                             .kind = PropertyValueKind::text,
                                             .text_value = value});
        }
      };
      const auto name = text(offsets_.name);
      const auto profile = text(offsets_.profile);
      const auto material = text(offsets_.material);
      const auto finish = text(offsets_.finish);
      const auto object_class = text(offsets_.object_class);
      add_text("Tekla", "name", name);
      if (offsets_.parameter_string_id != nullptr) {
        const auto parameter_id = read_u32(tuple, offsets_.parameter_string_id->offset);
        if (const auto parameter = strings_.find(parameter_id); parameter != strings_.end()) {
          add_text("Tekla", "profileParameters", parameter->second);
          profile_displays_.emplace_back(profile);
          if (!profile.ends_with(parameter->second)) profile_displays_.back() += parameter->second;
          add_text("Tekla", "profile", profile_displays_.back());
        } else {
          add_text("Tekla", "profile", profile);
        }
      } else {
        add_text("Tekla", "profile", profile);
      }
      add_text("Tekla", "material", material);
      add_text("Tekla", "finish", finish);
      add_text("Tekla", "class", object_class);
      const auto form_type =
          offsets_.form_type == nullptr ? 0U : read_u32(tuple, offsets_.form_type->offset);
      if (offsets_.form_type != nullptr) {
        properties_.push_back(PropertyView{.object_id = object_id,
                                           .group = "Tekla",
                                           .name = "formType",
                                           .kind = PropertyValueKind::integer,
                                           .integer_value = form_type});
      }
      if (const auto phase = report_.phases.find(object_id); phase != report_.phases.end()) {
        properties_.push_back(PropertyView{.object_id = object_id,
                                           .group = "Report",
                                           .name = "PHASE",
                                           .kind = PropertyValueKind::integer,
                                           .integer_value = phase->second});
      }
      const auto object = report_.objects.find(object_id);
      const auto part_numbering_link = report_.numbering_groups.find(object_id);
      if (part_numbering_link != report_.numbering_groups.end()) {
        const auto numbering = report_.part_numbering.find(part_numbering_link->second);
        if (numbering != report_.part_numbering.end()) {
          add_text("Report", "PREFIX", numbering->second.prefix);
        }
      }
      if (object != report_.objects.end()) {
        const auto assembly_link = report_.numbering_groups.find(object->second.assembly_id);
        if (assembly_link != report_.numbering_groups.end()) {
          const auto numbering = report_.assembly_numbering.find(assembly_link->second);
          if (numbering != report_.assembly_numbering.end()) {
            bool has_position = false;
            if (!numbering->second.prefix.empty()) {
              assembly_positions_.emplace_back(numbering->second.prefix);
              assembly_positions_.back() += "/" + std::to_string(numbering->second.position);
              has_position = true;
            } else if (numbering->second.start_number != 0U && numbering->second.position != 0U) {
              assembly_positions_.push_back(
                  std::to_string(numbering->second.start_number + numbering->second.position - 1U));
              has_position = true;
              const auto assembly = report_.assemblies.find(object->second.assembly_id);
              if (assembly != report_.assemblies.end() && assembly->second.flags == 1U &&
                  material.starts_with("C")) {
                const auto main_part = report_.objects.find(assembly->second.main_part_id);
                if (main_part != report_.objects.end() && main_part->second.mask1 == 256U) {
                  assembly_positions_.back() += "(?)";
                }
              }
            }
            if (has_position) {
              add_text("Report", "ASSEMBLY_POS", assembly_positions_.back());
            }
          }
        }
      }
      add_text("Report", "PROFILE_TYPE", profile_report_type(profile, form_type));
      const auto catalog = material_catalog_.find(std::string(material));
      const auto report_type = catalog == material_catalog_.end()
                                   ? fallback_material_report_type(material)
                                   : catalog->second.report_type;
      const auto density =
          catalog == material_catalog_.end() ? 0.0 : catalog->second.density_kg_m3;
      materials_.push_back(MaterialView{.object_id = object_id,
                                        .name = material,
                                        .finish = finish,
                                        .object_class = object_class,
                                        .report_type = report_type,
                                        .density_kg_m3 = density});
      ++part_count;
    }
    emit_materials_ = !materials_.empty();
    if (properties_.empty()) {
      if (emit_materials_) {
        emit_materials_ = false;
        return Result<BatchView>::success(
            BatchView{.kind = BatchKind::materials, .materials = materials_});
      }
      return next();
    }
    return Result<BatchView>::success(
        BatchView{.kind = BatchKind::properties, .properties = properties_});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* parts_ = nullptr;
  const TableSchema* part_schema_ = nullptr;
  std::uint32_t part_id_offset_ = 0;
  std::uint32_t attribute_id_offset_ = 0;
  const TableLayout* attributes_ = nullptr;
  const TableSchema* attribute_schema_ = nullptr;
  PartAttributeOffsets offsets_;
  std::unordered_map<std::uint32_t, std::uint64_t> attribute_rows_;
  std::unordered_map<std::uint32_t, std::string> strings_;
  PartReportLookups report_;
  std::unordered_map<std::string, MaterialCatalogEntry> material_catalog_;
  std::size_t part_batch_size_ = 0;
  std::uint64_t row_ = 0;
  bool emit_materials_ = false;
  std::vector<PropertyView> properties_;
  std::vector<MaterialView> materials_;
  std::vector<std::string> profile_displays_;
  std::vector<std::string> assembly_positions_;
};

struct BoltAttributeData {
  std::string_view standard;
  double diameter = 0.0;
  double length = 0.0;
  std::uint32_t fallback_count = 0;
};

[[nodiscard]] double numeric_field(std::span<const std::byte> tuple,
                                   const FieldSchema& field) noexcept {
  if (field.type == FieldType::f32 && field.size == 4U)
    return static_cast<double>(read_f32(tuple, field.offset));
  if (field.type == FieldType::f64 && field.size == 8U)
    return read_f64(tuple, field.offset);
  return 0.0;
}

[[nodiscard]] std::string_view bolt_report_standard(std::string_view standard) noexcept {
  if (standard == "7990") return "7990";
  if (standard == "EN-14399-4") return "EN-14399-4";
  if (standard == "7990-434-7989-5.6") return "7990-434";
  if (standard == "A325N") return "A325N";
  if (standard == "FISCHER FAZ") return "FAZ";
  return {};
}

class BoltSemanticReader final : public BatchReader {
 public:
  BoltSemanticReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& bolts,
                     const TableSchema& bolt_schema, std::uint32_t id_offset,
                     std::uint32_t attribute_id_offset, std::uint32_t polygon_id_offset,
                     std::unordered_map<std::uint32_t, BoltAttributeData> attributes,
                     std::unordered_map<std::uint32_t, std::uint32_t> polygon_counts,
                     std::size_t bolt_batch_size)
      : storage_(std::move(storage)),
        bolts_(&bolts),
        bolt_schema_(&bolt_schema),
        id_offset_(id_offset),
        attribute_id_offset_(attribute_id_offset),
        polygon_id_offset_(polygon_id_offset),
        attributes_(std::move(attributes)),
        polygon_counts_(std::move(polygon_counts)),
        bolt_batch_size_(bolt_batch_size) {
    properties_.reserve(bolt_batch_size_ * 6U);
  }

  Result<BatchView> next() override {
    if (row_ >= bolts_->info.row_count)
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    properties_.clear();
    const auto payload = storage_->payload.bytes();
    std::size_t bolt_count = 0U;
    while (row_ < bolts_->info.row_count && bolt_count < bolt_batch_size_) {
      const auto record = bolts_->record(payload, row_++);
      if (record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A bolt record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, bolt_schema_->tuple_size);
      const auto attribute = attributes_.find(read_u32(tuple, attribute_id_offset_));
      if (attribute == attributes_.end()) continue;
      const auto object_id = read_u32(tuple, id_offset_);
      const auto count = polygon_counts_.find(read_u32(tuple, polygon_id_offset_));
      const auto point_count =
          count == polygon_counts_.end() ? attribute->second.fallback_count : count->second;
      const auto add_text = [&](std::string_view group, std::string_view name,
                                std::string_view value) {
        if (!value.empty()) {
          properties_.push_back(PropertyView{.object_id = object_id,
                                             .group = group,
                                             .name = name,
                                             .kind = PropertyValueKind::text,
                                             .text_value = value});
        }
      };
      const auto add_number = [&](std::string_view group, std::string_view name, double value) {
        if (std::isfinite(value)) {
          properties_.push_back(PropertyView{.object_id = object_id,
                                             .group = group,
                                             .name = name,
                                             .kind = PropertyValueKind::floating,
                                             .floating_value = value});
        }
      };
      add_number("Tekla", "boltSize", attribute->second.diameter);
      properties_.push_back(PropertyView{.object_id = object_id,
                                         .group = "Tekla",
                                         .name = "boltCount",
                                         .kind = PropertyValueKind::integer,
                                         .integer_value = point_count});
      add_text("Tekla", "boltStandard", attribute->second.standard);
      add_text("Report", "BOLT_TYPE", attribute->second.standard);
      add_number("Report", "LENGTH", attribute->second.length);
      add_text("Report", "BOLT_STANDARD", bolt_report_standard(attribute->second.standard));
      ++bolt_count;
    }
    if (properties_.empty()) return next();
    return Result<BatchView>::success(
        BatchView{.kind = BatchKind::properties, .properties = properties_});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* bolts_ = nullptr;
  const TableSchema* bolt_schema_ = nullptr;
  std::uint32_t id_offset_ = 0;
  std::uint32_t attribute_id_offset_ = 0;
  std::uint32_t polygon_id_offset_ = 0;
  std::unordered_map<std::uint32_t, BoltAttributeData> attributes_;
  std::unordered_map<std::uint32_t, std::uint32_t> polygon_counts_;
  std::size_t bolt_batch_size_ = 0;
  std::uint64_t row_ = 0;
  std::vector<PropertyView> properties_;
};

struct SurfaceTreatmentAttributeData {
  std::string_view name;
  std::string_view material;
  std::string_view finish;
  std::string_view object_class;
  double thickness = 0.0;
  std::uint32_t type = 0U;
  std::uint32_t father_cuts = 0U;
};

class SurfaceTreatmentSemanticReader final : public BatchReader {
 public:
  SurfaceTreatmentSemanticReader(
      std::shared_ptr<const ModelStorage> storage, const TableLayout& surfaces,
      const TableSchema& surface_schema, std::uint32_t id_offset, std::uint32_t attribute_id_offset,
      std::unordered_map<std::uint32_t, SurfaceTreatmentAttributeData> attributes,
      std::size_t surface_batch_size)
      : storage_(std::move(storage)),
        surfaces_(&surfaces),
        surface_schema_(&surface_schema),
        id_offset_(id_offset),
        attribute_id_offset_(attribute_id_offset),
        attributes_(std::move(attributes)),
        surface_batch_size_(surface_batch_size) {
    properties_.reserve(surface_batch_size_ * 7U);
    materials_.reserve(surface_batch_size_);
  }

  Result<BatchView> next() override {
    if (emit_materials_) {
      emit_materials_ = false;
      return Result<BatchView>::success(
          BatchView{.kind = BatchKind::materials, .materials = materials_});
    }
    if (row_ >= surfaces_->info.row_count)
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    properties_.clear();
    materials_.clear();
    const auto payload = storage_->payload.bytes();
    std::size_t surface_count = 0U;
    while (row_ < surfaces_->info.row_count && surface_count < surface_batch_size_) {
      const auto record = surfaces_->record(payload, row_++);
      if (record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A surface-treatment record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, surface_schema_->tuple_size);
      const auto attribute = attributes_.find(read_u32(tuple, attribute_id_offset_));
      if (attribute == attributes_.end()) continue;
      const auto object_id = read_u32(tuple, id_offset_);
      const auto add_text = [&](std::string_view name, std::string_view value) {
        if (!value.empty()) {
          properties_.push_back(PropertyView{.object_id = object_id,
                                             .group = "Tekla",
                                             .name = name,
                                             .kind = PropertyValueKind::text,
                                             .text_value = value});
        }
      };
      add_text("name", attribute->second.name);
      add_text("material", attribute->second.material);
      add_text("finish", attribute->second.finish);
      add_text("class", attribute->second.object_class);
      properties_.push_back(PropertyView{.object_id = object_id,
                                         .group = "Tekla",
                                         .name = "surfaceType",
                                         .kind = PropertyValueKind::integer,
                                         .integer_value = attribute->second.type});
      if (std::isfinite(attribute->second.thickness) && attribute->second.thickness > 0.0) {
        properties_.push_back(PropertyView{.object_id = object_id,
                                           .group = "Tekla",
                                           .name = "thickness",
                                           .kind = PropertyValueKind::floating,
                                           .floating_value = attribute->second.thickness});
      }
      properties_.push_back(PropertyView{.object_id = object_id,
                                         .group = "Tekla",
                                         .name = "cutByFatherBooleans",
                                         .kind = PropertyValueKind::integer,
                                         .integer_value = attribute->second.father_cuts});
      materials_.push_back(MaterialView{.object_id = object_id,
                                        .name = attribute->second.material,
                                        .finish = attribute->second.finish,
                                        .object_class = attribute->second.object_class});
      ++surface_count;
    }
    emit_materials_ = !materials_.empty();
    if (properties_.empty()) {
      if (emit_materials_) {
        emit_materials_ = false;
        return Result<BatchView>::success(
            BatchView{.kind = BatchKind::materials, .materials = materials_});
      }
      return next();
    }
    return Result<BatchView>::success(
        BatchView{.kind = BatchKind::properties, .properties = properties_});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* surfaces_ = nullptr;
  const TableSchema* surface_schema_ = nullptr;
  std::uint32_t id_offset_ = 0U;
  std::uint32_t attribute_id_offset_ = 0U;
  std::unordered_map<std::uint32_t, SurfaceTreatmentAttributeData> attributes_;
  std::size_t surface_batch_size_ = 0U;
  std::uint64_t row_ = 0U;
  bool emit_materials_ = false;
  std::vector<PropertyView> properties_;
  std::vector<MaterialView> materials_;
};

struct PourObjectOffsets {
  std::uint32_t id = 0U;
  std::uint32_t object_class = 0U;
  std::uint32_t pour_phase = 0U;
  const FieldSchema* pour_number = nullptr;
  const FieldSchema* pour_type = nullptr;
  const FieldSchema* concrete_mixture = nullptr;
};

class PourObjectSemanticReader final : public BatchReader {
 public:
  PourObjectSemanticReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& objects,
                           const TableSchema& schema, PourObjectOffsets offsets,
                           std::size_t object_batch_size)
      : storage_(std::move(storage)),
        objects_(&objects),
        schema_(&schema),
        offsets_(offsets),
        object_batch_size_(object_batch_size) {
    properties_.reserve(object_batch_size_ * 5U);
  }

  Result<BatchView> next() override {
    const auto payload = storage_->payload.bytes();
    while (row_ < objects_->info.row_count) {
      properties_.clear();
      std::size_t object_count = 0U;
      while (row_ < objects_->info.row_count && object_count < object_batch_size_) {
        const auto record = objects_->record(payload, row_++);
        if (record.empty()) {
          return Result<BatchView>::failure(
              {ErrorCode::invalid_container, "A pour-object record lies outside the payload."});
        }
        if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
        const auto tuple = record.subspan(1U, schema_->tuple_size);
        const auto object_id = read_u32(tuple, offsets_.id);
        const auto add_integer = [&](std::string_view name, std::uint32_t value) {
          properties_.push_back(PropertyView{.object_id = object_id,
                                             .group = "Tekla",
                                             .name = name,
                                             .kind = PropertyValueKind::integer,
                                             .integer_value = value});
        };
        const auto add_text = [&](std::string_view name, const FieldSchema& field) {
          const auto value = read_text(tuple, field.offset, field.size);
          if (!value.empty()) {
            properties_.push_back(PropertyView{.object_id = object_id,
                                               .group = "Tekla",
                                               .name = name,
                                               .kind = PropertyValueKind::text,
                                               .text_value = value});
          }
        };
        add_integer("class", read_u32(tuple, offsets_.object_class));
        add_integer("pourPhase", read_u32(tuple, offsets_.pour_phase));
        add_text("pourNumber", *offsets_.pour_number);
        add_text("pourType", *offsets_.pour_type);
        add_text("concreteMixture", *offsets_.concrete_mixture);
        ++object_count;
      }
      if (!properties_.empty()) {
        return Result<BatchView>::success(
            BatchView{.kind = BatchKind::properties, .properties = properties_});
      }
    }
    return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* objects_ = nullptr;
  const TableSchema* schema_ = nullptr;
  PourObjectOffsets offsets_;
  std::size_t object_batch_size_ = 0U;
  std::uint64_t row_ = 0U;
  std::vector<PropertyView> properties_;
};

class PourUnitSemanticReader final : public BatchReader {
 public:
  PourUnitSemanticReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& units,
                         const TableSchema& schema, std::uint32_t id_offset,
                         const FieldSchema& name_field, std::size_t unit_batch_size)
      : storage_(std::move(storage)),
        units_(&units),
        schema_(&schema),
        id_offset_(id_offset),
        name_field_(&name_field),
        unit_batch_size_(unit_batch_size) {
    properties_.reserve(unit_batch_size_);
  }

  Result<BatchView> next() override {
    const auto payload = storage_->payload.bytes();
    while (row_ < units_->info.row_count) {
      properties_.clear();
      std::size_t unit_count = 0U;
      while (row_ < units_->info.row_count && unit_count < unit_batch_size_) {
        const auto record = units_->record(payload, row_++);
        if (record.empty()) {
          return Result<BatchView>::failure(
              {ErrorCode::invalid_container, "A pour-unit record lies outside the payload."});
        }
        if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
        const auto tuple = record.subspan(1U, schema_->tuple_size);
        const auto name = read_text(tuple, name_field_->offset, name_field_->size);
        if (!name.empty()) {
          properties_.push_back(PropertyView{.object_id = read_u32(tuple, id_offset_),
                                             .group = "Tekla",
                                             .name = "name",
                                             .kind = PropertyValueKind::text,
                                             .text_value = name});
        }
        ++unit_count;
      }
      if (!properties_.empty()) {
        return Result<BatchView>::success(
            BatchView{.kind = BatchKind::properties, .properties = properties_});
      }
    }
    return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* units_ = nullptr;
  const TableSchema* schema_ = nullptr;
  std::uint32_t id_offset_ = 0U;
  const FieldSchema* name_field_ = nullptr;
  std::size_t unit_batch_size_ = 0U;
  std::uint64_t row_ = 0U;
  std::vector<PropertyView> properties_;
};

struct RebarSpliceOffsets {
  std::uint32_t id = 0U;
  std::uint32_t end1 = 0U;
  std::uint32_t end2 = 0U;
  std::uint32_t type = 0U;
  std::uint32_t lap_length = 0U;
  std::uint32_t offset = 0U;
  std::uint32_t clearance = 0U;
  std::uint32_t position = 0U;
};

class RebarSpliceSemanticReader final : public BatchReader {
 public:
  RebarSpliceSemanticReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& splices,
                            const TableSchema& schema, RebarSpliceOffsets offsets,
                            std::size_t splice_batch_size)
      : storage_(std::move(storage)),
        splices_(&splices),
        schema_(&schema),
        offsets_(offsets),
        splice_batch_size_(splice_batch_size) {
    properties_.reserve(splice_batch_size_ * 7U);
  }

  Result<BatchView> next() override {
    if (row_ >= splices_->info.row_count) {
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    }
    properties_.clear();
    const auto payload = storage_->payload.bytes();
    std::size_t splice_count = 0U;
    while (row_ < splices_->info.row_count && splice_count < splice_batch_size_) {
      const auto record = splices_->record(payload, row_++);
      if (record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A rebar-splice record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, schema_->tuple_size);
      const auto object_id = read_u32(tuple, offsets_.id);
      const auto add_integer = [&](std::string_view name, std::uint32_t value) {
        properties_.push_back(PropertyView{.object_id = object_id,
                                           .group = "Tekla",
                                           .name = name,
                                           .kind = PropertyValueKind::integer,
                                           .integer_value = value});
      };
      const auto add_floating = [&](std::string_view name, std::uint32_t field_offset) {
        properties_.push_back(PropertyView{.object_id = object_id,
                                           .group = "Tekla",
                                           .name = name,
                                           .kind = PropertyValueKind::floating,
                                           .floating_value = read_f64(tuple, field_offset)});
      };
      add_integer("spliceType", read_u32(tuple, offsets_.type));
      add_floating("lapLength", offsets_.lap_length);
      add_floating("offset", offsets_.offset);
      add_floating("clearance", offsets_.clearance);
      add_integer("barPositions", read_u32(tuple, offsets_.position));
      add_integer("firstEnd", read_u32(tuple, offsets_.end1));
      add_integer("secondEnd", read_u32(tuple, offsets_.end2));
      ++splice_count;
    }
    if (properties_.empty()) return next();
    return Result<BatchView>::success(
        BatchView{.kind = BatchKind::properties, .properties = properties_});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* splices_ = nullptr;
  const TableSchema* schema_ = nullptr;
  RebarSpliceOffsets offsets_;
  std::size_t splice_batch_size_ = 0U;
  std::uint64_t row_ = 0U;
  std::vector<PropertyView> properties_;
};

class RebarSemanticReader final : public BatchReader {
 public:
  RebarSemanticReader(std::shared_ptr<const ModelStorage> storage, const TableLayout& rebars,
                      const TableSchema& rebar_schema, std::uint32_t object_id_offset,
                      std::uint32_t attribute_id_offset,
                      std::unordered_map<std::uint32_t, std::uint32_t> classes,
                      std::size_t rebar_batch_size)
      : storage_(std::move(storage)),
        rebars_(&rebars),
        rebar_schema_(&rebar_schema),
        object_id_offset_(object_id_offset),
        attribute_id_offset_(attribute_id_offset),
        classes_(std::move(classes)),
        rebar_batch_size_(rebar_batch_size) {
    properties_.reserve(rebar_batch_size_);
  }

  Result<BatchView> next() override {
    if (row_ >= rebars_->info.row_count)
      return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
    properties_.clear();
    const auto payload = storage_->payload.bytes();
    std::size_t rebar_count = 0U;
    while (row_ < rebars_->info.row_count && rebar_count < rebar_batch_size_) {
      const auto record = rebars_->record(payload, row_++);
      if (record.empty()) {
        return Result<BatchView>::failure(
            {ErrorCode::invalid_container, "A rebar record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, rebar_schema_->tuple_size);
      const auto attribute = classes_.find(read_u32(tuple, attribute_id_offset_));
      if (attribute == classes_.end()) continue;
      properties_.push_back(PropertyView{
          .object_id = read_u32(tuple, object_id_offset_),
          .group = "Tekla",
          .name = "class",
          .kind = PropertyValueKind::integer,
          .integer_value = attribute->second,
      });
      ++rebar_count;
    }
    if (properties_.empty()) return next();
    return Result<BatchView>::success(
        BatchView{.kind = BatchKind::properties, .properties = properties_});
  }

 private:
  std::shared_ptr<const ModelStorage> storage_;
  const TableLayout* rebars_ = nullptr;
  const TableSchema* rebar_schema_ = nullptr;
  std::uint32_t object_id_offset_ = 0U;
  std::uint32_t attribute_id_offset_ = 0U;
  std::unordered_map<std::uint32_t, std::uint32_t> classes_;
  std::size_t rebar_batch_size_ = 0U;
  std::uint64_t row_ = 0U;
  std::vector<PropertyView> properties_;
};

[[nodiscard]] Result<ProcessStream> make_rebar_semantic_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request) {
  const auto* rebars = schema.find_table("rebar");
  const auto* attributes = schema.find_table("rebar_attr");
  if (rebars == nullptr || attributes == nullptr ||
      rebars->ordinal >= storage->layout.tables.size() ||
      attributes->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The rebar semantic tables are unavailable."});
  }
  const auto* object_id = find_field(schema, *rebars, "id");
  const auto* attribute_id = find_field(schema, *rebars, "rebar_attr_id");
  const auto* attribute_key = find_field(schema, *attributes, "id");
  const auto* bar_class = find_field(schema, *attributes, "bar_class");
  if (object_id == nullptr || attribute_id == nullptr || attribute_key == nullptr ||
      bar_class == nullptr) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The rebar class links are unavailable."});
  }
  std::unordered_map<std::uint32_t, std::uint32_t> classes;
  const auto& attribute_layout = storage->layout.tables[attributes->ordinal];
  classes.reserve(static_cast<std::size_t>(attribute_layout.info.row_count));
  for (std::uint64_t row = 0; row < attribute_layout.info.row_count; ++row) {
    const auto record = attribute_layout.record(storage->payload.bytes(), row);
    if (record.empty()) {
      return Result<ProcessStream>::failure(
          {ErrorCode::invalid_container, "A rebar attribute lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
    const auto tuple = record.subspan(1U, attributes->tuple_size);
    classes.insert_or_assign(read_u32(tuple, attribute_key->offset),
                             read_u32(tuple, bar_class->offset));
  }
  const auto* rebar_layout = &storage->layout.tables[rebars->ordinal];
  return Result<ProcessStream>::success(std::make_unique<RebarSemanticReader>(
      std::move(storage), *rebar_layout, *rebars, object_id->offset, attribute_id->offset,
      std::move(classes), batch_size_for(request, 1024U)));
}

[[nodiscard]] Result<ProcessStream> make_bolt_semantic_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request) {
  const auto* bolts = schema.find_table("bolt");
  if (bolts == nullptr || bolts->ordinal >= storage->layout.tables.size() ||
      storage->layout.tables[bolts->ordinal].info.row_count == 0U) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The populated bolt table is unavailable."});
  }
  const auto* id = find_field(schema, *bolts, "id");
  const auto* attribute_id = find_field(schema, *bolts, "bolt_attr_id");
  if (attribute_id == nullptr) attribute_id = find_field(schema, *bolts, "attr_id");
  const auto* polygon_id = find_field(schema, *bolts, "polygon_id");
  if (id == nullptr || attribute_id == nullptr || polygon_id == nullptr) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The bolt semantic links are unavailable."});
  }

  constexpr std::array<std::string_view, 4> attribute_names{
      "bolt_attr", "old_bolt_attr_935", "old_bolt_attr_897", "old_bolt_attr_807"};
  const TableSchema* attribute_schema = nullptr;
  for (const auto name : attribute_names) {
    const auto* candidate = schema.find_table(name);
    if (candidate != nullptr && candidate->ordinal < storage->layout.tables.size() &&
        storage->layout.tables[candidate->ordinal].info.row_count != 0U) {
      attribute_schema = candidate;
      break;
    }
  }
  if (attribute_schema == nullptr) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "No populated bolt attribute table is available."});
  }
  const auto* attribute_key = find_field(schema, *attribute_schema, "id");
  const auto* standard = find_field(schema, *attribute_schema, "mat");
  const auto* diameter = find_field(schema, *attribute_schema, "BoltDiameter");
  const auto* length = find_field(schema, *attribute_schema, "BoltLength");
  const auto* fallback_count = find_field(schema, *attribute_schema, "npoints");
  if (attribute_key == nullptr || standard == nullptr || diameter == nullptr || length == nullptr) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The bolt semantic values are unavailable."});
  }
  std::unordered_map<std::uint32_t, BoltAttributeData> attributes;
  const auto& attribute_layout = storage->layout.tables[attribute_schema->ordinal];
  attributes.reserve(static_cast<std::size_t>(attribute_layout.info.row_count));
  for (std::uint64_t row = 0; row < attribute_layout.info.row_count; ++row) {
    const auto record = attribute_layout.record(storage->payload.bytes(), row);
    if (record.empty()) {
      return Result<ProcessStream>::failure(
          {ErrorCode::invalid_container, "A bolt attribute lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
    const auto tuple = record.subspan(1U, attribute_schema->tuple_size);
    attributes.insert_or_assign(
        read_u32(tuple, attribute_key->offset),
        BoltAttributeData{.standard = read_text(tuple, standard->offset, standard->size),
                          .diameter = numeric_field(tuple, *diameter),
                          .length = numeric_field(tuple, *length),
                          .fallback_count = fallback_count == nullptr
                                                ? 0U
                                                : read_u32(tuple, fallback_count->offset)});
  }

  std::unordered_map<std::uint32_t, std::uint32_t> polygon_counts;
  constexpr std::array<std::string_view, 2> polygon_names{"partpolygon",
                                                          "old_partpolygon_898"};
  for (const auto name : polygon_names) {
    const auto* polygons = schema.find_table(name);
    if (polygons == nullptr || polygons->ordinal >= storage->layout.tables.size() ||
        storage->layout.tables[polygons->ordinal].info.row_count == 0U)
      continue;
    const auto* polygon_key = find_field(schema, *polygons, "id");
    if (polygon_key == nullptr) continue;
    const auto& polygon_layout = storage->layout.tables[polygons->ordinal];
    for (std::uint64_t row = 0; row < polygon_layout.info.row_count; ++row) {
      const auto record = polygon_layout.record(storage->payload.bytes(), row);
      if (record.empty()) {
        return Result<ProcessStream>::failure(
            {ErrorCode::invalid_container, "A bolt polygon lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, polygons->tuple_size);
      std::uint32_t count = 0U;
      for (std::size_t index = 1U; index <= 10U; ++index) {
        const auto* type = find_field(schema, *polygons, "types" + std::to_string(index));
        if (type == nullptr || read_u32(tuple, type->offset) == 2'147'483'647U) break;
        ++count;
      }
      polygon_counts[read_u32(tuple, polygon_key->offset)] += count;
    }
    break;
  }
  return Result<ProcessStream>::success(std::make_unique<BoltSemanticReader>(
      storage, storage->layout.tables[bolts->ordinal], *bolts, id->offset,
      attribute_id->offset, polygon_id->offset, std::move(attributes), std::move(polygon_counts),
      batch_size_for(request, 384)));
}

[[nodiscard]] Result<ProcessStream> make_surface_treatment_semantic_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request) {
  const auto* surfaces = schema.find_table("surfacing");
  const auto* attributes = schema.find_table("surfacing_attr");
  if (surfaces == nullptr || attributes == nullptr ||
      surfaces->ordinal >= storage->layout.tables.size() ||
      attributes->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The surface-treatment tables are unavailable."});
  }
  const auto* id = find_field(schema, *surfaces, "id");
  const auto* attribute_id = find_field(schema, *surfaces, "attr_id");
  const auto* attribute_key = find_field(schema, *attributes, "id");
  const auto* name = find_field(schema, *attributes, "ben");
  const auto* material = find_field(schema, *attributes, "mat");
  const auto* finish = find_field(schema, *attributes, "finish");
  const auto* object_class = find_field(schema, *attributes, "ryhma");
  const auto* geometry = find_field(schema, *attributes, "Geometry");
  const auto* type = find_field(schema, *attributes, "surfacing_type");
  const auto* father_cuts = find_field(schema, *attributes, "father_cuts");
  if (id == nullptr || attribute_id == nullptr || attribute_key == nullptr || name == nullptr ||
      material == nullptr || finish == nullptr || object_class == nullptr || geometry == nullptr ||
      type == nullptr || father_cuts == nullptr) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The surface-treatment semantic layout is incomplete."});
  }
  std::unordered_map<std::uint32_t, SurfaceTreatmentAttributeData> decoded;
  const auto& layout = storage->layout.tables[attributes->ordinal];
  decoded.reserve(static_cast<std::size_t>(layout.info.row_count));
  for (std::uint64_t row = 0U; row < layout.info.row_count; ++row) {
    const auto record = layout.record(storage->payload.bytes(), row);
    if (record.empty()) {
      return Result<ProcessStream>::failure(
          {ErrorCode::invalid_container,
           "A surface-treatment attribute lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
    const auto tuple = record.subspan(1U, attributes->tuple_size);
    const auto thickness_text = read_text(tuple, geometry->offset, geometry->size);
    double thickness = 0.0;
    const auto parsed = std::from_chars(thickness_text.data(),
                                        thickness_text.data() + thickness_text.size(), thickness);
    if (parsed.ec != std::errc{} || parsed.ptr != thickness_text.data() + thickness_text.size()) {
      thickness = 0.0;
    }
    decoded.insert_or_assign(
        read_u32(tuple, attribute_key->offset),
        SurfaceTreatmentAttributeData{
            .name = read_text(tuple, name->offset, name->size),
            .material = read_text(tuple, material->offset, material->size),
            .finish = read_text(tuple, finish->offset, finish->size),
            .object_class = read_text(tuple, object_class->offset, object_class->size),
            .thickness = thickness,
            .type = read_u32(tuple, type->offset),
            .father_cuts = read_u32(tuple, father_cuts->offset),
        });
  }
  return Result<ProcessStream>::success(std::make_unique<SurfaceTreatmentSemanticReader>(
      storage, storage->layout.tables[surfaces->ordinal], *surfaces, id->offset,
      attribute_id->offset, std::move(decoded), batch_size_for(request, 512U)));
}

[[nodiscard]] Result<ProcessStream> make_pour_object_semantic_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request) {
  const auto* table = schema.find_table("pour_object");
  if (table == nullptr || table->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The pour-object table is unavailable."});
  }
  const auto* id = find_field(schema, *table, "id");
  const auto* object_class = find_field(schema, *table, "obj_class");
  const auto* pour_phase = find_field(schema, *table, "pour_phase");
  const auto* pour_number = find_field(schema, *table, "pour_number");
  const auto* pour_type = find_field(schema, *table, "pour_type");
  const auto* concrete_mixture = find_field(schema, *table, "concrete_mixture");
  if (id == nullptr || object_class == nullptr || pour_phase == nullptr || pour_number == nullptr ||
      pour_type == nullptr || concrete_mixture == nullptr || id->type != FieldType::u32 ||
      object_class->type != FieldType::u32 || pour_phase->type != FieldType::u32 ||
      pour_number->type != FieldType::text || pour_type->type != FieldType::text ||
      concrete_mixture->type != FieldType::text) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The pour-object semantic layout is incomplete."});
  }
  return Result<ProcessStream>::success(std::make_unique<PourObjectSemanticReader>(
      storage, storage->layout.tables[table->ordinal], *table,
      PourObjectOffsets{.id = id->offset,
                        .object_class = object_class->offset,
                        .pour_phase = pour_phase->offset,
                        .pour_number = pour_number,
                        .pour_type = pour_type,
                        .concrete_mixture = concrete_mixture},
      batch_size_for(request, 160U)));
}

[[nodiscard]] Result<ProcessStream> make_pour_unit_semantic_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request) {
  const auto* table = schema.find_table("pour_unit");
  if (table == nullptr || table->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The pour-unit table is unavailable."});
  }
  const auto* id = find_field(schema, *table, "id");
  const auto* name = find_field(schema, *table, "name");
  if (id == nullptr || name == nullptr || id->type != FieldType::u32 ||
      name->type != FieldType::text) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The pour-unit semantic layout is incomplete."});
  }
  return Result<ProcessStream>::success(std::make_unique<PourUnitSemanticReader>(
      storage, storage->layout.tables[table->ordinal], *table, id->offset, *name,
      batch_size_for(request, 80U)));
}

[[nodiscard]] Result<ProcessStream> make_rebar_splice_semantic_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request) {
  const auto* table = schema.find_table("rebar_splice");
  if (table == nullptr || table->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The rebar-splice table is unavailable."});
  }
  const auto* id = find_field(schema, *table, "id");
  const auto* end1 = find_field(schema, *table, "end1");
  const auto* end2 = find_field(schema, *table, "end2");
  const auto* type = find_field(schema, *table, "type");
  const auto* lap_length = find_field(schema, *table, "laplength");
  const auto* offset = find_field(schema, *table, "offset");
  const auto* clearance = find_field(schema, *table, "clearance");
  const auto* position = find_field(schema, *table, "position");
  if (id == nullptr || end1 == nullptr || end2 == nullptr || type == nullptr ||
      lap_length == nullptr || offset == nullptr || clearance == nullptr || position == nullptr ||
      id->type != FieldType::u32 || end1->type != FieldType::u32 || end2->type != FieldType::u32 ||
      type->type != FieldType::u32 || lap_length->type != FieldType::f64 ||
      offset->type != FieldType::f64 || clearance->type != FieldType::f64 ||
      position->type != FieldType::u32) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The rebar-splice semantic layout is incomplete."});
  }
  return Result<ProcessStream>::success(std::make_unique<RebarSpliceSemanticReader>(
      storage, storage->layout.tables[table->ordinal], *table,
      RebarSpliceOffsets{.id = id->offset,
                         .end1 = end1->offset,
                         .end2 = end2->offset,
                         .type = type->offset,
                         .lap_length = lap_length->offset,
                         .offset = offset->offset,
                         .clearance = clearance->offset,
                         .position = position->offset},
      batch_size_for(request, 512U)));
}

class ChainedReader final : public BatchReader {
 public:
  explicit ChainedReader(std::vector<ProcessStream> streams) : streams_(std::move(streams)) {}

  Result<BatchView> next() override {
    while (current_ < streams_.size()) {
      auto batch = streams_[current_]->next();
      if (!batch || batch.value().kind != BatchKind::end) return batch;
      ++current_;
    }
    return Result<BatchView>::success(BatchView{.kind = BatchKind::end});
  }

 private:
  std::vector<ProcessStream> streams_;
  std::size_t current_ = 0;
};

[[nodiscard]] Result<ProcessStream> make_part_semantic_stream(
    std::shared_ptr<const ModelStorage> storage, const Schema& schema,
    const ProcessRequest& request) {
  const auto* part_schema = schema.find_table("part");
  if (part_schema == nullptr || part_schema->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The part table is unavailable."});
  }
  constexpr std::array<std::string_view, 10> attribute_names{
      "part_attr",         "old_part_attr_911", "old_part_attr_866", "old_part_attr_801",
      "old_part_attr_787", "old_part_attr_784", "old_part_attr_742", "old_part_attr_650",
      "old_part_attr_644", "old_part_attr_622"};
  const TableSchema* attribute_schema = nullptr;
  for (const auto name : attribute_names) {
    const auto* candidate = schema.find_table(name);
    if (candidate != nullptr && candidate->ordinal < storage->layout.tables.size() &&
        storage->layout.tables[candidate->ordinal].info.row_count != 0) {
      attribute_schema = candidate;
      break;
    }
  }
  if (attribute_schema == nullptr) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "No populated part attribute table is available."});
  }
  auto part_id = required_field(schema, *part_schema, "id", FieldType::u32);
  auto part_attribute_id = required_field(schema, *part_schema, "part_attr_id", FieldType::u32);
  auto attribute_id = required_field(schema, *attribute_schema, "id", FieldType::u32);
  if (!part_id || !part_attribute_id || !attribute_id) {
    return Result<ProcessStream>::failure(!part_id             ? part_id.error()
                                          : !part_attribute_id ? part_attribute_id.error()
                                                               : attribute_id.error());
  }
  PartAttributeOffsets offsets;
  offsets.id = attribute_id.value()->offset;
  offsets.name = find_field(schema, *attribute_schema, "ben");
  offsets.profile = find_field(schema, *attribute_schema, "prof");
  if (offsets.profile == nullptr)
    offsets.profile = find_field(schema, *attribute_schema, "Geometry");
  offsets.parameter_string_id = find_field(schema, *attribute_schema, "ParameterStringId");
  offsets.material = find_field(schema, *attribute_schema, "mat");
  offsets.finish = find_field(schema, *attribute_schema, "finish");
  offsets.object_class = find_field(schema, *attribute_schema, "ryhma");
  offsets.form_type = find_field(schema, *attribute_schema, "form_type");

  const auto& attribute_layout = storage->layout.tables[attribute_schema->ordinal];
  std::unordered_map<std::uint32_t, std::uint64_t> attribute_rows;
  attribute_rows.reserve(static_cast<std::size_t>(attribute_layout.info.row_count));
  for (std::uint64_t row = 0; row < attribute_layout.info.row_count; ++row) {
    const auto record = attribute_layout.record(storage->payload.bytes(), row);
    if (record.empty()) {
      return Result<ProcessStream>::failure(
          {ErrorCode::invalid_container, "A part attribute lies outside the payload."});
    }
    const auto tuple = record.subspan(1, attribute_schema->tuple_size);
    attribute_rows.insert_or_assign(read_u32(tuple, offsets.id), row);
  }

  std::unordered_map<std::uint32_t, std::string_view> string_segments;
  std::unordered_map<std::uint32_t, std::uint32_t> string_next;
  if (const auto* string_schema = schema.find_table("string");
      string_schema != nullptr && string_schema->ordinal < storage->layout.tables.size()) {
    const auto* string_id = find_field(schema, *string_schema, "id");
    const auto* string_next_id = find_field(schema, *string_schema, "next_id");
    const auto* string_value = find_field(schema, *string_schema, "string");
    const auto& string_layout = storage->layout.tables[string_schema->ordinal];
    if (string_id != nullptr && string_next_id != nullptr && string_value != nullptr) {
      string_segments.reserve(static_cast<std::size_t>(string_layout.info.row_count));
      string_next.reserve(static_cast<std::size_t>(string_layout.info.row_count));
      for (std::uint64_t row = 0; row < string_layout.info.row_count; ++row) {
        const auto record = string_layout.record(storage->payload.bytes(), row);
        if (record.empty()) continue;
        const auto tuple = record.subspan(1, string_schema->tuple_size);
        const auto id = read_u32(tuple, string_id->offset);
        string_segments.insert_or_assign(
            id, read_text(tuple, string_value->offset, string_value->size));
        string_next.insert_or_assign(id, read_u32(tuple, string_next_id->offset));
      }
    }
  }
  std::unordered_map<std::uint32_t, std::string> strings;
  if (offsets.parameter_string_id != nullptr) {
    strings.reserve(attribute_rows.size());
    for (const auto& [attribute_id_value, attribute_row] : attribute_rows) {
      static_cast<void>(attribute_id_value);
      const auto record = attribute_layout.record(storage->payload.bytes(), attribute_row);
      if (record.empty()) continue;
      const auto tuple = record.subspan(1, attribute_schema->tuple_size);
      const auto root = read_u32(tuple, offsets.parameter_string_id->offset);
      if (root == 0U || strings.contains(root)) continue;
      std::string resolved;
      std::unordered_set<std::uint32_t> seen;
      auto current = root;
      while (current != 0U && seen.size() <= string_segments.size() &&
             seen.insert(current).second) {
        const auto segment = string_segments.find(current);
        const auto next = string_next.find(current);
        if (segment == string_segments.end() || next == string_next.end()) {
          resolved.clear();
          break;
        }
        resolved += segment->second;
        current = next->second;
      }
      if (current == 0U && !resolved.empty()) strings.emplace(root, std::move(resolved));
    }
  }

  PartReportLookups report;
  if (const auto* table = schema.find_table("object");
      table != nullptr && table->ordinal < storage->layout.tables.size()) {
    const auto* id = find_field(schema, *table, "id");
    const auto* assembly = find_field(schema, *table, "assembly");
    const auto* mask1 = find_field(schema, *table, "mask1");
    const auto& layout = storage->layout.tables[table->ordinal];
    if (id != nullptr && assembly != nullptr && mask1 != nullptr) {
      report.objects.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) {
          return Result<ProcessStream>::failure(
              {ErrorCode::invalid_container, "An object report record lies outside the payload."});
        }
        const auto tuple = record.subspan(1, table->tuple_size);
        report.objects.insert_or_assign(
            read_u32(tuple, id->offset),
            ObjectReportData{.assembly_id = read_u32(tuple, assembly->offset),
                             .mask1 = read_u32(tuple, mask1->offset)});
      }
    }
  }
  if (const auto* table = schema.find_table("assembly");
      table != nullptr && table->ordinal < storage->layout.tables.size()) {
    const auto* id = find_field(schema, *table, "id");
    const auto* main_part = find_field(schema, *table, "dum");
    const auto* flags = find_field(schema, *table, "flags");
    const auto& layout = storage->layout.tables[table->ordinal];
    if (id != nullptr && main_part != nullptr && flags != nullptr) {
      report.assemblies.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) {
          return Result<ProcessStream>::failure(
              {ErrorCode::invalid_container,
               "An assembly report record lies outside the payload."});
        }
        const auto tuple = record.subspan(1, table->tuple_size);
        report.assemblies.insert_or_assign(
            read_u32(tuple, id->offset),
            AssemblyReportData{.main_part_id = read_u32(tuple, main_part->offset),
                               .flags = read_u32(tuple, flags->offset)});
      }
    }
  }
  if (const auto* table = schema.find_table("object_phase");
      table != nullptr && table->ordinal < storage->layout.tables.size()) {
    const auto* object_id = find_field(schema, *table, "object_id");
    const auto* phase = find_field(schema, *table, "phase_number");
    const auto& layout = storage->layout.tables[table->ordinal];
    if (object_id != nullptr && phase != nullptr) {
      report.phases.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) {
          return Result<ProcessStream>::failure(
              {ErrorCode::invalid_container, "A phase report record lies outside the payload."});
        }
        const auto tuple = record.subspan(1, table->tuple_size);
        report.phases.insert_or_assign(read_u32(tuple, object_id->offset),
                                       read_u32(tuple, phase->offset));
      }
    }
  }
  if (const auto* table = schema.find_table("object_numbering");
      table != nullptr && table->ordinal < storage->layout.tables.size()) {
    const auto* id = find_field(schema, *table, "id");
    const auto* group = find_field(schema, *table, "numbering_group");
    const auto& layout = storage->layout.tables[table->ordinal];
    if (id != nullptr && group != nullptr) {
      report.numbering_groups.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) {
          return Result<ProcessStream>::failure(
              {ErrorCode::invalid_container, "A numbering link lies outside the payload."});
        }
        const auto tuple = record.subspan(1, table->tuple_size);
        report.numbering_groups.insert_or_assign(read_u32(tuple, id->offset),
                                                 read_u32(tuple, group->offset));
      }
    }
  }
  const auto load_numbering =
      [&](std::string_view table_name,
          std::unordered_map<std::uint32_t, NumberingReportData>& target) -> Result<bool> {
    const auto* table = schema.find_table(table_name);
    if (table == nullptr || table->ordinal >= storage->layout.tables.size()) {
      return Result<bool>::success(false);
    }
    const auto* id = find_field(schema, *table, "id");
    const auto* start = find_field(schema, *table, "start_no");
    const auto* position = find_field(schema, *table, "fpos");
    const auto* prefix = find_field(schema, *table, "pos");
    if (id == nullptr || start == nullptr || position == nullptr || prefix == nullptr) {
      return Result<bool>::success(false);
    }
    const auto& layout = storage->layout.tables[table->ordinal];
    target.reserve(static_cast<std::size_t>(layout.info.row_count));
    for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
      const auto record = layout.record(storage->payload.bytes(), row);
      if (record.empty()) {
        return Result<bool>::failure(
            {ErrorCode::invalid_container, "A numbering report record lies outside the payload."});
      }
      const auto tuple = record.subspan(1, table->tuple_size);
      target.insert_or_assign(
          read_u32(tuple, id->offset),
          NumberingReportData{.start_number = read_u32(tuple, start->offset),
                              .position = read_u32(tuple, position->offset),
                              .prefix = read_text(tuple, prefix->offset, prefix->size)});
    }
    return Result<bool>::success(true);
  };
  auto loaded_part_numbering = load_numbering("part_numbering", report.part_numbering);
  if (!loaded_part_numbering) return Result<ProcessStream>::failure(loaded_part_numbering.error());
  auto loaded_assembly_numbering = load_numbering("assembly_numbering", report.assembly_numbering);
  if (!loaded_assembly_numbering) {
    return Result<ProcessStream>::failure(loaded_assembly_numbering.error());
  }
  const auto* part_layout = &storage->layout.tables[part_schema->ordinal];
  const auto* attribute_layout_pointer = &attribute_layout;
  auto material_catalog = load_material_catalog(storage->package);
  return Result<ProcessStream>::success(std::make_unique<PartSemanticReader>(
      std::move(storage), *part_layout, *part_schema, part_id.value()->offset,
      part_attribute_id.value()->offset, *attribute_layout_pointer, *attribute_schema, offsets,
      std::move(attribute_rows), std::move(strings), std::move(report),
      std::move(material_catalog),
      batch_size_for(request, 768)));
}

}  // namespace

Result<ProcessStream> make_property_stream(std::shared_ptr<const ModelStorage> storage,
                                           const Schema& schema, const ProcessRequest& request) {
  constexpr std::array<std::string_view, 3> numeric_names{"old_numattr_attr_628",
                                                          "old_numattr_attr_641", "numattr_attr"};
  constexpr std::array<std::string_view, 3> string_names{"old_strattr_attr_628",
                                                         "old_strattr_attr_909", "strattr_attr"};
  constexpr std::array<std::string_view, 1> reference_names{"idattr_attr"};
  auto numeric = load_definitions(*storage, schema, numeric_names, PropertyValueKind::floating);
  auto strings = load_definitions(*storage, schema, string_names, PropertyValueKind::text);
  auto references =
      load_definitions(*storage, schema, reference_names, PropertyValueKind::reference);
  if (!numeric) return Result<ProcessStream>::failure(numeric.error());
  if (!strings) return Result<ProcessStream>::failure(strings.error());
  if (!references) return Result<ProcessStream>::failure(references.error());

  std::vector<LinkSource> sources;
  const auto add_source = [&](std::string_view table_name, std::string_view definition_id_name,
                              PropertyValueKind kind) -> Result<bool> {
    const auto* table_schema = schema.find_table(table_name);
    if (table_schema == nullptr || table_schema->ordinal >= storage->layout.tables.size() ||
        storage->layout.tables[table_schema->ordinal].info.row_count == 0) {
      return Result<bool>::success(false);
    }
    auto definition_id = required_field(schema, *table_schema, definition_id_name, FieldType::u32);
    auto father_id = required_field(schema, *table_schema, "father_id", FieldType::u32);
    if (!definition_id || !father_id) {
      return Result<bool>::failure(!definition_id ? definition_id.error() : father_id.error());
    }
    sources.push_back(LinkSource{table_schema, &storage->layout.tables[table_schema->ordinal], kind,
                                 definition_id.value()->offset, father_id.value()->offset});
    return Result<bool>::success(true);
  };
  auto added_numeric = add_source("numattr", "numattr_attr_id", PropertyValueKind::floating);
  auto added_strings = add_source("strattr", "strattr_attr_id", PropertyValueKind::text);
  auto added_references = add_source("idattr", "idattr_attr_id", PropertyValueKind::reference);
  if (!added_numeric) return Result<ProcessStream>::failure(added_numeric.error());
  if (!added_strings) return Result<ProcessStream>::failure(added_strings.error());
  if (!added_references) return Result<ProcessStream>::failure(added_references.error());

  auto winners = find_property_winners(*storage, sources, numeric.value(), strings.value(),
                                       references.value());
  if (!winners) return Result<ProcessStream>::failure(winners.error());

  auto attributes = ProcessStream{std::make_unique<PropertyReader>(
      storage, std::move(sources), std::move(numeric.value()), std::move(strings.value()),
      std::move(references.value()), std::move(winners.value()), batch_size_for(request, 96))};
  std::vector<ProcessStream> streams;
  streams.push_back(std::move(attributes));
  const auto* part_schema = schema.find_table("part");
  if (part_schema != nullptr && part_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[part_schema->ordinal].info.row_count != 0U) {
    auto parts = make_part_semantic_stream(storage, schema, request);
    if (!parts) return Result<ProcessStream>::failure(parts.error());
    streams.push_back(std::move(parts.value()));
  }
  const auto* bolt_schema = schema.find_table("bolt");
  if (bolt_schema != nullptr && bolt_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[bolt_schema->ordinal].info.row_count != 0U) {
    auto bolts = make_bolt_semantic_stream(storage, schema, request);
    if (!bolts) return Result<ProcessStream>::failure(bolts.error());
    streams.push_back(std::move(bolts.value()));
  }
  const auto* rebar_schema = schema.find_table("rebar");
  if (rebar_schema != nullptr && rebar_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[rebar_schema->ordinal].info.row_count != 0U) {
    auto rebars = make_rebar_semantic_stream(storage, schema, request);
    if (!rebars) return Result<ProcessStream>::failure(rebars.error());
    streams.push_back(std::move(rebars.value()));
  }
  const auto* welding_schema = schema.find_table("welding");
  if (welding_schema != nullptr && welding_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[welding_schema->ordinal].info.row_count != 0U) {
    auto welds = make_weld_semantic_stream(storage, schema, request);
    if (!welds) return Result<ProcessStream>::failure(welds.error());
    streams.push_back(std::move(welds.value()));
  }
  const auto* surface_schema = schema.find_table("surfacing");
  if (surface_schema != nullptr && surface_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[surface_schema->ordinal].info.row_count != 0U) {
    auto surfaces = make_surface_treatment_semantic_stream(storage, schema, request);
    if (!surfaces) return Result<ProcessStream>::failure(surfaces.error());
    streams.push_back(std::move(surfaces.value()));
  }
  const auto* pour_object_schema = schema.find_table("pour_object");
  if (pour_object_schema != nullptr &&
      pour_object_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[pour_object_schema->ordinal].info.row_count != 0U) {
    auto pour_objects = make_pour_object_semantic_stream(storage, schema, request);
    if (!pour_objects) return Result<ProcessStream>::failure(pour_objects.error());
    streams.push_back(std::move(pour_objects.value()));
  }
  const auto* pour_unit_schema = schema.find_table("pour_unit");
  if (pour_unit_schema != nullptr && pour_unit_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[pour_unit_schema->ordinal].info.row_count != 0U) {
    auto pour_units = make_pour_unit_semantic_stream(storage, schema, request);
    if (!pour_units) return Result<ProcessStream>::failure(pour_units.error());
    streams.push_back(std::move(pour_units.value()));
  }
  const auto* rebar_splice_schema = schema.find_table("rebar_splice");
  if (rebar_splice_schema != nullptr &&
      rebar_splice_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[rebar_splice_schema->ordinal].info.row_count != 0U) {
    auto rebar_splices = make_rebar_splice_semantic_stream(storage, schema, request);
    if (!rebar_splices) return Result<ProcessStream>::failure(rebar_splices.error());
    streams.push_back(std::move(rebar_splices.value()));
  }
  return Result<ProcessStream>::success(std::make_unique<ChainedReader>(std::move(streams)));
}

Result<ProcessStream> make_relation_stream(std::shared_ptr<const ModelStorage> storage,
                                           const Schema& schema, const ProcessRequest& request) {
  const auto* table_schema = schema.find_table("relation");
  if (table_schema == nullptr || table_schema->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The relation table is unavailable."});
  }
  constexpr std::array<std::string_view, 5> names{"id", "type", "id1", "id2", "flags"};
  std::array<std::uint32_t, 5> offsets{};
  for (std::size_t index = 0; index < names.size(); ++index) {
    auto field = required_field(schema, *table_schema, names[index], FieldType::u32);
    if (!field) return Result<ProcessStream>::failure(field.error());
    offsets[index] = field.value()->offset;
  }
  const auto* table_layout = &storage->layout.tables[table_schema->ordinal];
  return Result<ProcessStream>::success(std::make_unique<RelationReader>(
      std::move(storage), *table_layout, *table_schema, offsets, batch_size_for(request, 64)));
}

Result<ProcessStream> make_semantic_relation_stream(std::shared_ptr<const ModelStorage> storage,
                                                    const Schema& schema,
                                                    const ProcessRequest& request) {
  const TableSchema* object_schema = schema.find_table("object");
  if (object_schema == nullptr || object_schema->ordinal >= storage->layout.tables.size()) {
    object_schema = schema.find_table("old_object_948");
  }
  if (object_schema == nullptr || object_schema->ordinal >= storage->layout.tables.size()) {
    return Result<ProcessStream>::failure(
        {ErrorCode::schema_mismatch, "The object table is unavailable for semantic relations."});
  }
  auto object_id = required_field(schema, *object_schema, "id", FieldType::u32);
  auto parent_id = required_field(schema, *object_schema, "kuuluu", FieldType::u32);
  auto assembly_id = required_field(schema, *object_schema, "assembly", FieldType::u32);
  const auto* direct_type = find_field(schema, *object_schema, "type");
  const auto* direct_subtype = find_field(schema, *object_schema, "subtype");
  const auto* attribute_id = find_field(schema, *object_schema, "object_attr_id");
  if (!object_id || !parent_id || !assembly_id ||
      ((direct_type == nullptr || direct_subtype == nullptr) && attribute_id == nullptr)) {
    return Result<ProcessStream>::failure(!object_id   ? object_id.error()
                                          : !parent_id ? parent_id.error()
                                          : !assembly_id ? assembly_id.error()
                                                         : Error{ErrorCode::schema_mismatch,
                                                                 "The object kind fields are unavailable for semantic relations."});
  }

  struct RawKind {
    std::uint32_t type = 0U;
    std::uint32_t subtype = 0U;
  };
  std::unordered_map<std::uint32_t, RawKind> legacy_kinds;
  if (attribute_id != nullptr && (direct_type == nullptr || direct_subtype == nullptr)) {
    constexpr std::array<std::string_view, 4> attribute_tables{
        "old_object_attr_951", "old_object_attr_915", "old_object_attr_900",
        "old_object_attr_879"};
    for (const auto name : attribute_tables) {
      const auto* candidate = schema.find_table(name);
      if (candidate == nullptr || candidate->ordinal >= storage->layout.tables.size() ||
          storage->layout.tables[candidate->ordinal].info.row_count == 0U) {
        continue;
      }
      auto id = required_field(schema, *candidate, "id", FieldType::u32);
      auto type = required_field(schema, *candidate, "type", FieldType::u32);
      auto subtype = required_field(schema, *candidate, "subtype", FieldType::u32);
      if (!id || !type || !subtype) {
        return Result<ProcessStream>::failure(!id ? id.error() : !type ? type.error()
                                                                       : subtype.error());
      }
      const auto& layout = storage->layout.tables[candidate->ordinal];
      legacy_kinds.reserve(static_cast<std::size_t>(layout.info.row_count));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) {
          return Result<ProcessStream>::failure(
              {ErrorCode::invalid_container, "An object attribute lies outside the payload."});
        }
        if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
        const auto tuple = record.subspan(1U, candidate->tuple_size);
        legacy_kinds.insert_or_assign(
            read_u32(tuple, id.value()->offset),
            RawKind{read_u32(tuple, type.value()->offset),
                    read_u32(tuple, subtype.value()->offset)});
      }
      break;
    }
  }

  std::vector<SemanticObject> objects;
  std::unordered_set<std::uint32_t> endpoints;
  AssemblyMembers assembly_members;
  const auto payload = storage->payload.bytes();
  const auto& object_layout = storage->layout.tables[object_schema->ordinal];
  objects.reserve(static_cast<std::size_t>(object_layout.info.row_count));
  endpoints.reserve(static_cast<std::size_t>(object_layout.info.row_count));
  assembly_members.reserve(static_cast<std::size_t>(object_layout.info.row_count / 4U));
  for (std::uint64_t row = 0; row < object_layout.info.row_count; ++row) {
    const auto record = object_layout.record(payload, row);
    if (record.empty()) {
      return Result<ProcessStream>::failure(
          {ErrorCode::invalid_container, "An object record lies outside the payload."});
    }
    if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0) continue;
    const auto tuple = record.subspan(1, object_schema->tuple_size);
    const auto id = read_u32(tuple, object_id.value()->offset);
    if (id == 0U || !endpoints.insert(id).second) continue;
    RawKind raw_kind;
    if (direct_type != nullptr && direct_subtype != nullptr) {
      raw_kind = {read_u32(tuple, direct_type->offset), read_u32(tuple, direct_subtype->offset)};
    } else {
      const auto found = legacy_kinds.find(read_u32(tuple, attribute_id->offset));
      if (found != legacy_kinds.end()) raw_kind = found->second;
    }
    const SemanticObject object{
        .id = id,
        .parent_id = read_u32(tuple, parent_id.value()->offset),
        .kind = object_kind(raw_kind.type, raw_kind.subtype),
    };
    objects.push_back(object);
    const auto assembly = read_u32(tuple, assembly_id.value()->offset);
    if (assembly != 0U && assembly != id) assembly_members[assembly].push_back(id);
  }

  const TableLayout* relation_layout = nullptr;
  const auto* relation_schema = schema.find_table("relation");
  std::array<std::uint32_t, 4> relation_offsets{};
  if (relation_schema != nullptr && relation_schema->ordinal < storage->layout.tables.size()) {
    constexpr std::array<std::string_view, 4> names{"id", "type", "id1", "id2"};
    for (std::size_t index = 0; index < names.size(); ++index) {
      auto field = required_field(schema, *relation_schema, names[index], FieldType::u32);
      if (!field) return Result<ProcessStream>::failure(field.error());
      relation_offsets[index] = field.value()->offset;
    }
    relation_layout = &storage->layout.tables[relation_schema->ordinal];
  }

  const TableLayout* joint_layout = nullptr;
  const auto* joint_schema = schema.find_table("joint");
  std::array<std::uint32_t, 3> joint_offsets{};
  if (joint_schema != nullptr && joint_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[joint_schema->ordinal].info.row_count != 0U) {
    constexpr std::array<std::string_view, 3> names{"id", "prim", "sek"};
    for (std::size_t index = 0; index < names.size(); ++index) {
      auto field = required_field(schema, *joint_schema, names[index], FieldType::u32);
      if (!field) return Result<ProcessStream>::failure(field.error());
      joint_offsets[index] = field.value()->offset;
    }
    joint_layout = &storage->layout.tables[joint_schema->ordinal];
  }

  std::unordered_map<std::uint32_t, std::uint32_t> main_members;
  std::vector<std::uint32_t> assembly_order;
  const auto* assembly_schema = schema.find_table("assembly");
  if (assembly_schema != nullptr && assembly_schema->ordinal < storage->layout.tables.size()) {
    auto id = required_field(schema, *assembly_schema, "id", FieldType::u32);
    auto main = required_field(schema, *assembly_schema, "dum", FieldType::u32);
    if (!id || !main) {
      return Result<ProcessStream>::failure(!id ? id.error() : main.error());
    }
    const auto& layout = storage->layout.tables[assembly_schema->ordinal];
    main_members.reserve(static_cast<std::size_t>(layout.info.row_count));
    assembly_order.reserve(static_cast<std::size_t>(layout.info.row_count));
    for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
      const auto record = layout.record(payload, row);
      if (record.empty()) {
        return Result<ProcessStream>::failure(
            {ErrorCode::invalid_container, "An assembly record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0) continue;
      const auto tuple = record.subspan(1, assembly_schema->tuple_size);
      const auto assembly = read_u32(tuple, id.value()->offset);
      if (assembly == 0U || !endpoints.contains(assembly) || main_members.contains(assembly)) {
        continue;
      }
      main_members.emplace(assembly, read_u32(tuple, main.value()->offset));
      assembly_order.push_back(assembly);
    }
  }

  std::vector<PourMembership> pour_memberships;
  const auto* pour_object_schema = schema.find_table("pour_object");
  if (pour_object_schema != nullptr &&
      pour_object_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[pour_object_schema->ordinal].info.row_count != 0U) {
    auto id = required_field(schema, *pour_object_schema, "id", FieldType::u32);
    auto unit = required_field(schema, *pour_object_schema, "pour_unit_id", FieldType::u32);
    if (!id || !unit) {
      return Result<ProcessStream>::failure(!id ? id.error() : unit.error());
    }
    const auto& layout = storage->layout.tables[pour_object_schema->ordinal];
    pour_memberships.reserve(static_cast<std::size_t>(layout.info.row_count));
    for (std::uint64_t row = 0U; row < layout.info.row_count; ++row) {
      const auto record = layout.record(payload, row);
      if (record.empty()) {
        return Result<ProcessStream>::failure(
            {ErrorCode::invalid_container, "A pour-object record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, pour_object_schema->tuple_size);
      const auto object = read_u32(tuple, id.value()->offset);
      const auto pour_unit = read_u32(tuple, unit.value()->offset);
      if (object != 0U && pour_unit != 0U && object != pour_unit) {
        pour_memberships.push_back(
            PourMembership{.pour_object_id = object, .pour_unit_id = pour_unit});
      }
    }
    std::ranges::sort(pour_memberships, {}, [](const auto& membership) {
      return std::pair{membership.pour_unit_id, membership.pour_object_id};
    });
    pour_memberships.erase(std::unique(pour_memberships.begin(), pour_memberships.end(),
                                       [](const auto& left, const auto& right) {
                                         return left.pour_object_id == right.pour_object_id &&
                                                left.pour_unit_id == right.pour_unit_id;
                                       }),
                           pour_memberships.end());
  }

  std::vector<RebarSpliceConnection> rebar_splices;
  const auto* rebar_splice_schema = schema.find_table("rebar_splice");
  if (rebar_splice_schema != nullptr &&
      rebar_splice_schema->ordinal < storage->layout.tables.size() &&
      storage->layout.tables[rebar_splice_schema->ordinal].info.row_count != 0U) {
    auto id = required_field(schema, *rebar_splice_schema, "id", FieldType::u32);
    auto first = required_field(schema, *rebar_splice_schema, "rebarid1", FieldType::u32);
    auto second = required_field(schema, *rebar_splice_schema, "rebarid2", FieldType::u32);
    if (!id || !first || !second) {
      return Result<ProcessStream>::failure(!id      ? id.error()
                                            : !first ? first.error()
                                                     : second.error());
    }
    const auto& layout = storage->layout.tables[rebar_splice_schema->ordinal];
    rebar_splices.reserve(static_cast<std::size_t>(layout.info.row_count));
    for (std::uint64_t row = 0U; row < layout.info.row_count; ++row) {
      const auto record = layout.record(payload, row);
      if (record.empty()) {
        return Result<ProcessStream>::failure(
            {ErrorCode::invalid_container, "A rebar-splice record lies outside the payload."});
      }
      if ((std::to_integer<std::uint8_t>(record[0]) & 0x08U) != 0U) continue;
      const auto tuple = record.subspan(1U, rebar_splice_schema->tuple_size);
      const RebarSpliceConnection connection{
          .splice_id = read_u32(tuple, id.value()->offset),
          .first_reinforcement_id = read_u32(tuple, first.value()->offset),
          .second_reinforcement_id = read_u32(tuple, second.value()->offset),
      };
      if (connection.splice_id != 0U && connection.first_reinforcement_id != 0U &&
          connection.second_reinforcement_id != 0U) {
        rebar_splices.push_back(connection);
      }
    }
    std::ranges::sort(rebar_splices, {}, [](const auto& splice) {
      return std::tuple{splice.splice_id, splice.first_reinforcement_id,
                        splice.second_reinforcement_id};
    });
    rebar_splices.erase(
        std::unique(rebar_splices.begin(), rebar_splices.end(),
                    [](const auto& left, const auto& right) {
                      return left.splice_id == right.splice_id &&
                             left.first_reinforcement_id == right.first_reinforcement_id &&
                             left.second_reinforcement_id == right.second_reinforcement_id;
                    }),
        rebar_splices.end());
  }

  return Result<ProcessStream>::success(std::make_unique<SemanticRelationReader>(
      std::move(storage), std::move(objects), std::move(endpoints), relation_layout,
      relation_schema, relation_offsets, joint_layout, joint_schema, joint_offsets,
      std::move(assembly_order), std::move(main_members), std::move(assembly_members),
      std::move(pour_memberships), std::move(rebar_splices), batch_size_for(request, 48)));
}

Result<ProcessStream> make_instance_stream(std::shared_ptr<const ModelStorage> storage,
                                           const Schema& schema, const ProcessRequest& request) {
  std::unordered_map<std::uint32_t, std::uint32_t> child_counts;
  const TableSchema* object_schema = schema.find_table("object");
  if (object_schema == nullptr || object_schema->ordinal >= storage->layout.tables.size() ||
      storage->layout.tables[object_schema->ordinal].info.row_count == 0) {
    object_schema = schema.find_table("old_object_948");
  }
  if (object_schema != nullptr && object_schema->ordinal < storage->layout.tables.size()) {
    const auto* id = find_field(schema, *object_schema, "id");
    const auto* parent = find_field(schema, *object_schema, "kuuluu");
    if (id != nullptr && parent != nullptr && id->type == FieldType::u32 &&
        parent->type == FieldType::u32) {
      const auto& layout = storage->layout.tables[object_schema->ordinal];
      child_counts.reserve(static_cast<std::size_t>(layout.info.row_count / 4U));
      for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
        const auto record = layout.record(storage->payload.bytes(), row);
        if (record.empty()) continue;
        const auto tuple = record.subspan(1, object_schema->tuple_size);
        const auto object_id = read_u32(tuple, id->offset);
        const auto parent_id = read_u32(tuple, parent->offset);
        if (parent_id != 0 && parent_id != object_id) ++child_counts[parent_id];
      }
    }
  }

  auto component_definitions = load_component_definitions(*storage, request);
  if (!component_definitions) {
    return Result<ProcessStream>::failure(component_definitions.error());
  }
  const auto strings = load_strings(*storage, schema);
  const auto string_value = [&](std::uint32_t id) -> std::string_view {
    const auto found = strings.find(id);
    return found == strings.end() ? std::string_view{} : found->second;
  };
  std::vector<InstanceView> instances;
  const auto append_table = [&](std::string_view table_name, InstanceKind kind) -> Result<bool> {
    const auto* table_schema = schema.find_table(table_name);
    if (table_schema == nullptr || table_schema->ordinal >= storage->layout.tables.size()) {
      return Result<bool>::success(false);
    }
    const auto& layout = storage->layout.tables[table_schema->ordinal];
    if (layout.info.row_count == 0) return Result<bool>::success(false);
    auto id = required_field(schema, *table_schema, "id", FieldType::u32);
    auto name = required_field(schema, *table_schema, "name", FieldType::u32);
    const auto number_name = kind == InstanceKind::joint ? "joint_no" : "number";
    auto number = required_field(schema, *table_schema, number_name, FieldType::u32);
    const auto type_name = kind == InstanceKind::joint ? "obj_type" : "type";
    auto type = required_field(schema, *table_schema, type_name, FieldType::u32);
    auto description = required_field(schema, *table_schema, "desc", FieldType::u32);
    if (!id || !name || !number || !type || !description) {
      return Result<bool>::failure(!id       ? id.error()
                                   : !name   ? name.error()
                                   : !number ? number.error()
                                   : !type   ? type.error()
                                             : description.error());
    }
    const FieldSchema* primary = nullptr;
    const FieldSchema* secondary = nullptr;
    const FieldSchema* secondary_count = nullptr;
    if (kind == InstanceKind::joint) {
      primary = find_field(schema, *table_schema, "prim");
      secondary = find_field(schema, *table_schema, "sek");
      secondary_count = find_field(schema, *table_schema, "seknum");
      if (primary == nullptr || secondary == nullptr || secondary_count == nullptr ||
          primary->type != FieldType::u32 || secondary->type != FieldType::u32 ||
          secondary_count->type != FieldType::u32) {
        return Result<bool>::failure(
            {ErrorCode::schema_mismatch, "The generated joint instance layout is incomplete."});
      }
    }
    instances.reserve(instances.size() + static_cast<std::size_t>(layout.info.row_count));
    for (std::uint64_t row = 0; row < layout.info.row_count; ++row) {
      const auto record = layout.record(storage->payload.bytes(), row);
      if (record.empty()) {
        return Result<bool>::failure({ErrorCode::invalid_container,
                                      "A component instance record lies outside the payload."});
      }
      const auto tuple = record.subspan(1, table_schema->tuple_size);
      const auto object_id = read_u32(tuple, id.value()->offset);
      const auto instance_name = string_value(read_u32(tuple, name.value()->offset));
      instances.push_back(InstanceView{
          .object_id = object_id,
          .kind = kind,
          .name = instance_name,
          .description = string_value(read_u32(tuple, description.value()->offset)),
          .number = read_u32(tuple, number.value()->offset),
          .type = read_u32(tuple, type.value()->offset),
          .primary_object_id = primary == nullptr ? 0U : read_u32(tuple, primary->offset),
          .secondary_object_id = secondary == nullptr ? 0U : read_u32(tuple, secondary->offset),
          .secondary_object_count =
              secondary_count == nullptr ? 0U : read_u32(tuple, secondary_count->offset),
          .persisted_child_count = child_counts[object_id],
          .definition_status = !component_definitions.value().checked
                                   ? ComponentDefinitionStatus::not_checked
                               : component_definitions.value().contains(kind, instance_name)
                                   ? ComponentDefinitionStatus::available
                                   : ComponentDefinitionStatus::unavailable,
          .visible = (std::to_integer<std::uint8_t>(record[0]) & 0x08U) == 0,
      });
    }
    return Result<bool>::success(true);
  };
  auto joints = append_table("joint", InstanceKind::joint);
  if (!joints) return Result<ProcessStream>::failure(joints.error());
  auto macros = append_table("macro", InstanceKind::macro);
  if (!macros) return Result<ProcessStream>::failure(macros.error());
  std::sort(instances.begin(), instances.end(), [](const auto& lhs, const auto& rhs) {
    return std::pair{lhs.object_id, lhs.kind} < std::pair{rhs.object_id, rhs.kind};
  });
  return Result<ProcessStream>::success(std::make_unique<InstanceReader>(
      std::move(storage), std::move(instances), batch_size_for(request, 128)));
}

}  // namespace tekla::db1::detail
