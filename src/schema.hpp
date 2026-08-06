#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <tekla/db1/result.hpp>

namespace tekla::db1::detail {

struct DatabaseLayout;

enum class FieldType : std::uint8_t {
  u32,
  f32,
  f64,
  text,
  bytes,
};

struct FieldSchema {
  std::string_view name;
  FieldType type = FieldType::bytes;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
};

struct TableSchema {
  std::uint32_t ordinal = 0;
  std::string_view name;
  std::uint32_t tuple_size = 0;
  bool has_table_key = false;
  std::uint32_t table_key = 0;
  std::uint32_t descriptor_offset = 0;
  std::uint32_t descriptor_count = 0;
  std::uint32_t field_offset = 0;
  std::uint32_t field_count = 0;
};

struct Schema {
  std::string_view internal_format;
  std::uint8_t database_kind_marker = 0;
  std::span<const TableSchema> tables;
  std::span<const std::uint8_t> descriptors;
  std::span<const FieldSchema> fields;

  [[nodiscard]] const TableSchema* find_table(std::string_view name) const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> table_descriptors(
      const TableSchema& table) const noexcept;
  [[nodiscard]] std::span<const FieldSchema> table_fields(
      const TableSchema& table) const noexcept;
};

[[nodiscard]] const Schema* schema_for(std::string_view internal_format,
                                       std::uint8_t database_kind_marker) noexcept;
[[nodiscard]] Result<const Schema*> validate_schema(const DatabaseLayout& layout);

}  // namespace tekla::db1::detail
