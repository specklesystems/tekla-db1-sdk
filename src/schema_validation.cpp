#include "schema.hpp"

#include "container.hpp"

#include <algorithm>
#include <string>

namespace tekla::db1::detail {

Result<const Schema*> validate_schema(const DatabaseLayout& layout) {
  const Schema* schema =
      schema_for(layout.info.internal_format, layout.info.kind_marker);
  if (schema == nullptr) {
    return Result<const Schema*>::failure(
        {ErrorCode::unsupported_format,
         "No generated schema is available for this database format."});
  }
  if (layout.tables.size() != schema->tables.size()) {
    return Result<const Schema*>::failure(
        {ErrorCode::schema_mismatch,
         "The physical table count does not match the generated model schema."});
  }
  for (std::size_t index = 0; index < layout.tables.size(); ++index) {
    const auto& physical = layout.tables[index];
    const auto& expected = schema->tables[index];
    const auto expected_descriptors = schema->table_descriptors(expected);
    if (physical.info.ordinal != expected.ordinal ||
        physical.info.tuple_size != expected.tuple_size ||
        physical.descriptors.size() != expected_descriptors.size() ||
        !std::equal(physical.descriptors.begin(), physical.descriptors.end(),
                    expected_descriptors.begin())) {
      return Result<const Schema*>::failure(
          {ErrorCode::schema_mismatch,
           "A physical table differs from the generated schema at ordinal " +
               std::to_string(index) + "."});
    }
    if (physical.info.table_key.has_value() && expected.has_table_key &&
        *physical.info.table_key != expected.table_key) {
      return Result<const Schema*>::failure(
          {ErrorCode::schema_mismatch,
           "A physical table key differs from the generated schema at ordinal " +
               std::to_string(index) + "."});
    }
  }
  return Result<const Schema*>::success(schema);
}

}  // namespace tekla::db1::detail
