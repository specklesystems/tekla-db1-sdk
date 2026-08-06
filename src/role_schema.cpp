#include "role_schema.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace tekla::db1::detail {

Result<ProjectedSchema> project_keyed_role_schema(const DatabaseLayout& layout) {
  const auto* base = schema_for(layout.info.internal_format, layout.info.kind_marker);
  if (base == nullptr) {
    return Result<ProjectedSchema>::failure(
        {ErrorCode::unsupported_format,
         "No generated schema is available for this adjacent database role."});
  }
  ProjectedSchema projected;
  projected.internal_format = base->internal_format;
  projected.database_kind_marker = base->database_kind_marker;
  projected.tables.reserve(layout.tables.size());
  for (const auto& physical : layout.tables) {
    if (!physical.info.table_key.has_value()) continue;
    const TableSchema* match = nullptr;
    for (const auto& candidate : base->tables) {
      if (!candidate.has_table_key || candidate.table_key != *physical.info.table_key ||
          candidate.tuple_size != physical.info.tuple_size) {
        continue;
      }
      const auto descriptors = base->table_descriptors(candidate);
      if (descriptors.size() != physical.descriptors.size() ||
          !std::equal(descriptors.begin(), descriptors.end(), physical.descriptors.begin())) {
        continue;
      }
      if (match != nullptr) {
        return Result<ProjectedSchema>::failure(
            {ErrorCode::schema_mismatch,
             "An adjacent database table key maps to multiple generated tables."});
      }
      match = &candidate;
    }
    if (match == nullptr) continue;
    auto table = *match;
    table.ordinal = physical.info.ordinal;
    table.descriptor_offset = static_cast<std::uint32_t>(projected.descriptors.size());
    table.field_offset = static_cast<std::uint32_t>(projected.fields.size());
    const auto descriptors = base->table_descriptors(*match);
    projected.descriptors.insert(projected.descriptors.end(), descriptors.begin(),
                                 descriptors.end());
    const auto fields = base->table_fields(*match);
    projected.fields.insert(projected.fields.end(), fields.begin(), fields.end());
    projected.tables.push_back(table);
  }
  if (projected.tables.empty()) {
    return Result<ProjectedSchema>::failure(
        {ErrorCode::schema_mismatch,
         "The adjacent database has no keyed tables matching the generated schema."});
  }
  return Result<ProjectedSchema>::success(std::move(projected));
}

}  // namespace tekla::db1::detail
