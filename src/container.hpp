#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <tekla/db1/model.hpp>
#include <tekla/db1/package.hpp>
#include <tekla/db1/result.hpp>
#include <vector>

namespace tekla::db1::detail {

struct Payload {
  Wrapper wrapper = Wrapper::raw;
  std::shared_ptr<const ByteSource> source;
  std::vector<std::byte> inflated;

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    if (wrapper == Wrapper::gzip) {
      return inflated;
    }
    return source == nullptr ? std::span<const std::byte>{} : source->bytes();
  }
};

struct TableLayout {
  TableInfo info;
  std::uint64_t record_offset = 0;
  std::uint64_t record_size = 0;
  std::vector<std::uint8_t> descriptors;

  [[nodiscard]] std::span<const std::byte> record(
      std::span<const std::byte> payload, std::uint64_t index) const noexcept;
};

struct DatabaseLayout {
  ModelInfo info;
  std::vector<TableLayout> tables;
};

[[nodiscard]] Result<Payload> decode_payload(std::shared_ptr<const ByteSource> source,
                                             std::uint64_t max_payload_bytes);
[[nodiscard]] Result<DatabaseLayout> inspect_payload(const Payload& payload,
                                                     bool validate_container);

}  // namespace tekla::db1::detail
