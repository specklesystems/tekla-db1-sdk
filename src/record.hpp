#pragma once

#include "schema.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tekla::db1::detail {

[[nodiscard]] inline std::uint32_t read_u32(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1])) << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3])) << 24U);
}

[[nodiscard]] inline float read_f32(std::span<const std::byte> bytes,
                                    std::size_t offset) noexcept {
  return std::bit_cast<float>(read_u32(bytes, offset));
}

[[nodiscard]] inline double read_f64(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  const auto bits = static_cast<std::uint64_t>(read_u32(bytes, offset)) |
                    (static_cast<std::uint64_t>(read_u32(bytes, offset + 4)) << 32U);
  return std::bit_cast<double>(bits);
}

[[nodiscard]] inline std::string_view read_text(std::span<const std::byte> bytes,
                                                std::size_t offset,
                                                std::size_t size) noexcept {
  const char* data = reinterpret_cast<const char*>(bytes.data() + offset);
  std::size_t length = 0;
  while (length < size && data[length] != '\0') {
    ++length;
  }
  return {data, length};
}

[[nodiscard]] inline const FieldSchema* find_field(const Schema& schema,
                                                   const TableSchema& table,
                                                   std::string_view name) noexcept {
  for (const auto& field : schema.table_fields(table)) {
    if (field.name == name) {
      return &field;
    }
  }
  return nullptr;
}

}  // namespace tekla::db1::detail
