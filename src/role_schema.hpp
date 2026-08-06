#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "container.hpp"
#include "schema.hpp"

namespace tekla::db1::detail {

// Database roles such as xslib.db1 store a keyed subset of the model DBdict.
// Projecting by stable table key keeps role-specific physical ordinals out of
// semantic decoders while still requiring an exact tuple and descriptor match.
struct ProjectedSchema {
  std::string_view internal_format;
  std::uint8_t database_kind_marker = 0;
  std::vector<TableSchema> tables;
  std::vector<std::uint8_t> descriptors;
  std::vector<FieldSchema> fields;

  [[nodiscard]] Schema view() const noexcept {
    return Schema{internal_format, database_kind_marker, tables, descriptors, fields};
  }
};

[[nodiscard]] Result<ProjectedSchema> project_keyed_role_schema(const DatabaseLayout& layout);

}  // namespace tekla::db1::detail
