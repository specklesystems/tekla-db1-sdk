#include "container.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>
#include <zlib.h>

namespace tekla::db1::detail {
namespace {

constexpr std::array<std::byte, 2> gzip_magic{std::byte{0x1f}, std::byte{0x8b}};
constexpr std::array<std::byte, 4> table_end_magic{
    std::byte{0x66}, std::byte{0xc0}, std::byte{0xce}, std::byte{0xdb}};
constexpr std::array<std::byte, 4> table_footer_magic{
    std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc}, std::byte{0x00}};

[[nodiscard]] bool starts_with(std::span<const std::byte> bytes,
                               std::span<const std::byte> prefix) noexcept {
  return bytes.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1])) << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3])) << 24U);
}

[[nodiscard]] bool is_hex(std::byte value) noexcept {
  const auto character = static_cast<unsigned char>(value);
  return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

[[nodiscard]] Result<ModelInfo> parse_header(std::span<const std::byte> bytes,
                                             Wrapper wrapper,
                                             std::uint64_t source_bytes) {
  constexpr std::string_view product = "Xsteel";
  constexpr std::array<std::size_t, 4> uuid_hyphens{8, 13, 18, 23};
  if (bytes.size() < 49 ||
      !starts_with(bytes, std::as_bytes(std::span(product.data(), product.size())))) {
    return Result<ModelInfo>::failure(
        {ErrorCode::unsupported_format, "Expected an Xsteel database header."});
  }

  const auto kind = std::to_integer<std::uint8_t>(bytes[6]);
  if (kind < 0x80 || kind > 0x8f || bytes[7] != std::byte{' '}) {
    return Result<ModelInfo>::failure(
        {ErrorCode::unsupported_format, "The Xsteel database kind marker is unsupported."});
  }

  std::size_t format_end = 8;
  bool saw_dot = false;
  while (format_end < bytes.size() && bytes[format_end] != std::byte{' '}) {
    if (bytes[format_end] == std::byte{'.'}) {
      saw_dot = true;
    } else if (std::isdigit(static_cast<unsigned char>(bytes[format_end])) == 0) {
      return Result<ModelInfo>::failure(
          {ErrorCode::unsupported_format, "The Xsteel internal format is malformed."});
    }
    ++format_end;
  }
  if (!saw_dot || format_end == 8 || format_end + 37 > bytes.size()) {
    return Result<ModelInfo>::failure(
        {ErrorCode::unsupported_format, "The Xsteel internal format header is truncated."});
  }

  const std::size_t uuid_start = format_end + 1;
  for (std::size_t index = 0; index < 36; ++index) {
    const bool hyphen = std::find(uuid_hyphens.begin(), uuid_hyphens.end(), index) !=
                        uuid_hyphens.end();
    if ((hyphen && bytes[uuid_start + index] != std::byte{'-'}) ||
        (!hyphen && !is_hex(bytes[uuid_start + index]))) {
      return Result<ModelInfo>::failure(
          {ErrorCode::unsupported_format, "The Xsteel database UUID is malformed."});
    }
  }

  ModelInfo info;
  info.wrapper = wrapper;
  info.product = product;
  info.kind_marker = kind;
  info.internal_format.assign(reinterpret_cast<const char*>(bytes.data() + 8), format_end - 8);
  info.database_uuid.assign(reinterpret_cast<const char*>(bytes.data() + uuid_start), 36);
  info.source_bytes = source_bytes;
  info.payload_bytes = bytes.size();
  return Result<ModelInfo>::success(std::move(info));
}

struct FooterCandidate {
  std::size_t size = 0;
  TableFooter footer = TableFooter::plain;
  std::optional<std::uint32_t> table_key;
};

[[nodiscard]] Result<TableLayout> parse_table(std::span<const std::byte> section,
                                              std::uint64_t section_offset,
                                              std::uint32_t ordinal,
                                              const FooterCandidate& candidate) {
  if (section.size() < candidate.size + 8) {
    return Result<TableLayout>::failure(
        {ErrorCode::invalid_container, "A table header is truncated."});
  }
  const auto tuple_size = read_u32(section, 0);
  const auto field_count = read_u32(section, 4);
  const std::uint64_t header_size = 8ULL + 4ULL * field_count;
  const std::uint64_t record_size = 1ULL + tuple_size + 8ULL;
  const std::uint64_t content_end = section.size() - candidate.size;
  if (header_size > content_end || record_size == 0) {
    return Result<TableLayout>::failure(
        {ErrorCode::invalid_container, "A table header exceeds its physical section."});
  }
  const std::uint64_t body_size = content_end - header_size;
  if (body_size % record_size != 0) {
    return Result<TableLayout>::failure(
        {ErrorCode::invalid_container, "A table body is not record-aligned."});
  }

  TableLayout layout;
  auto& info = layout.info;
  info.ordinal = ordinal;
  info.tuple_size = tuple_size;
  info.field_count = field_count;
  info.row_count = body_size / record_size;
  info.table_key = candidate.table_key;
  info.footer = candidate.footer;
  layout.record_offset = section_offset + header_size;
  layout.record_size = record_size;
  layout.descriptors.reserve(field_count);
  for (std::uint32_t index = 0; index < field_count; ++index) {
    const auto descriptor = read_u32(section, 8ULL + 4ULL * index);
    if (descriptor > std::numeric_limits<std::uint8_t>::max()) {
      return Result<TableLayout>::failure(
          {ErrorCode::invalid_container, "A table field descriptor is out of range."});
    }
    layout.descriptors.push_back(static_cast<std::uint8_t>(descriptor));
  }
  for (std::uint64_t row = 0; row < info.row_count; ++row) {
    const auto marker_offset = header_size + row * record_size;
    const auto marker = std::to_integer<std::uint8_t>(
        section[static_cast<std::size_t>(marker_offset)]);
    if ((marker & 0x08U) == 0) {
      ++info.visible_rows;
    } else {
      ++info.invisible_rows;
    }
  }
  return Result<TableLayout>::success(std::move(layout));
}

[[nodiscard]] bool has_suffix(std::span<const std::byte> bytes,
                              std::span<const std::byte> suffix) noexcept {
  return bytes.size() >= suffix.size() &&
         std::equal(suffix.begin(), suffix.end(), bytes.end() -
                                                       static_cast<std::ptrdiff_t>(suffix.size()));
}

[[nodiscard]] Result<TableLayout> parse_delimited_table(
    std::span<const std::byte> section, std::uint64_t section_offset,
    std::uint32_t ordinal) {
  std::array<FooterCandidate, 3> candidates{};
  std::size_t count = 0;
  if (section.size() >= 13 && section[section.size() - 13] == std::byte{0} &&
      std::equal(table_footer_magic.begin(), table_footer_magic.end(), section.end() - 12) &&
      has_suffix(section, table_end_magic)) {
    candidates[count++] = FooterCandidate{
        13, TableFooter::connected, read_u32(section, section.size() - 8)};
  }
  if (section.size() >= 9 && section[section.size() - 9] == std::byte{0} &&
      has_suffix(section, table_end_magic)) {
    candidates[count++] =
        FooterCandidate{9, TableFooter::keyed, read_u32(section, section.size() - 8)};
  }
  if (section.size() >= 5 && section[section.size() - 5] == std::byte{0} &&
      has_suffix(section, table_end_magic)) {
    candidates[count++] = FooterCandidate{5, TableFooter::plain, std::nullopt};
  }

  for (std::size_t index = 0; index < count; ++index) {
    auto parsed = parse_table(section, section_offset, ordinal, candidates[index]);
    if (parsed) {
      return parsed;
    }
  }
  return Result<TableLayout>::failure(
      {ErrorCode::invalid_container, "A delimited table has an unknown footer or layout."});
}

}  // namespace

Result<Payload> decode_payload(std::shared_ptr<const ByteSource> source,
                               std::uint64_t max_payload_bytes) {
  if (source == nullptr || source->bytes().empty()) {
    return Result<Payload>::failure(
        {ErrorCode::invalid_argument, "The model database is empty."});
  }

  Payload result;
  result.source = std::move(source);
  const auto input = result.source->bytes();
  if (!starts_with(input, gzip_magic)) {
    if (max_payload_bytes != 0 && input.size() > max_payload_bytes) {
      return Result<Payload>::failure(
          {ErrorCode::resource_limit, "The raw payload exceeds its configured limit."});
    }
    result.wrapper = Wrapper::raw;
    return Result<Payload>::success(std::move(result));
  }

  result.wrapper = Wrapper::gzip;
  z_stream stream{};
  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
    return Result<Payload>::failure(
        {ErrorCode::decompression_failed, "Could not initialize gzip decompression."});
  }

  constexpr std::size_t chunk_size = 1024 * 1024;
  std::vector<std::byte> chunk(chunk_size);
  std::size_t input_cursor = 0;
  bool complete = false;
  while (!complete) {
    if (stream.avail_in == 0 && input_cursor < input.size()) {
      const auto remaining = input.size() - input_cursor;
      const auto offered = std::min<std::size_t>(remaining, std::numeric_limits<uInt>::max());
      stream.next_in = reinterpret_cast<Bytef*>(
          const_cast<std::byte*>(input.data() + input_cursor));
      stream.avail_in = static_cast<uInt>(offered);
      input_cursor += offered;
    }

    stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    const int status = inflate(&stream, Z_NO_FLUSH);
    const auto produced = chunk.size() - stream.avail_out;
    if (max_payload_bytes != 0 &&
        produced > max_payload_bytes - std::min<std::uint64_t>(
                                         max_payload_bytes, result.inflated.size())) {
      inflateEnd(&stream);
      return Result<Payload>::failure(
          {ErrorCode::resource_limit, "The decompressed payload exceeds its configured limit."});
    }
    result.inflated.insert(result.inflated.end(), chunk.begin(), chunk.begin() +
                                                                    static_cast<std::ptrdiff_t>(produced));

    if (status == Z_STREAM_END) {
      if (stream.avail_in == 0 && input_cursor == input.size()) {
        complete = true;
      } else {
        Bytef* next_input = stream.next_in;
        const uInt available = stream.avail_in;
        if (inflateReset2(&stream, 16 + MAX_WBITS) != Z_OK) {
          inflateEnd(&stream);
          return Result<Payload>::failure(
              {ErrorCode::decompression_failed, "Could not advance to the next gzip member."});
        }
        stream.next_in = next_input;
        stream.avail_in = available;
      }
    } else if (status != Z_OK) {
      inflateEnd(&stream);
      return Result<Payload>::failure(
          {ErrorCode::decompression_failed, "The gzip payload is corrupt or truncated."});
    } else if (stream.avail_in == 0 && input_cursor == input.size() && produced == 0) {
      inflateEnd(&stream);
      return Result<Payload>::failure(
          {ErrorCode::decompression_failed, "The gzip payload ended before its checksum."});
    }
  }
  inflateEnd(&stream);
  return Result<Payload>::success(std::move(result));
}

std::span<const std::byte> TableLayout::record(
    std::span<const std::byte> payload, std::uint64_t index) const noexcept {
  if (index >= info.row_count || record_offset > payload.size() ||
      record_size > payload.size() - record_offset ||
      index > (payload.size() - record_offset - record_size) / record_size) {
    return {};
  }
  const auto offset = record_offset + index * record_size;
  return payload.subspan(static_cast<std::size_t>(offset),
                         static_cast<std::size_t>(record_size));
}

Result<DatabaseLayout> inspect_payload(const Payload& payload, bool validate_container) {
  const auto bytes = payload.bytes();
  auto header = parse_header(bytes, payload.wrapper,
                             payload.source == nullptr ? 0 : payload.source->bytes().size());
  if (!header) {
    return Result<DatabaseLayout>::failure(header.error());
  }
  DatabaseLayout layout;
  layout.info = std::move(header.value());
  if (!validate_container) {
    return Result<DatabaseLayout>::success(std::move(layout));
  }

  std::vector<std::size_t> delimiters;
  for (std::size_t offset = 0; offset + table_end_magic.size() <= bytes.size(); ++offset) {
    if (std::equal(table_end_magic.begin(), table_end_magic.end(), bytes.begin() +
                                                                  static_cast<std::ptrdiff_t>(offset))) {
      delimiters.push_back(offset + table_end_magic.size());
      offset += table_end_magic.size() - 1;
    }
  }
  if (delimiters.empty()) {
    return Result<DatabaseLayout>::failure(
        {ErrorCode::invalid_container, "The database has no global header delimiter."});
  }

  std::size_t section_start = delimiters.front();
  for (std::size_t index = 1; index < delimiters.size(); ++index) {
    const auto section_end = delimiters[index];
    auto table = parse_delimited_table(bytes.subspan(section_start, section_end - section_start),
                                       section_start,
                                       static_cast<std::uint32_t>(index - 1));
    if (!table) {
      return Result<DatabaseLayout>::failure(table.error());
    }
    layout.info.visible_rows += table.value().info.visible_rows;
    layout.info.invisible_rows += table.value().info.invisible_rows;
    layout.info.tables.push_back(table.value().info);
    layout.tables.push_back(std::move(table.value()));
    section_start = section_end;
  }

  const auto trailing = bytes.subspan(section_start);
  if (trailing.size() < 5 || trailing[trailing.size() - 5] != std::byte{0} ||
      !has_suffix(trailing, table_footer_magic)) {
    return Result<DatabaseLayout>::failure(
        {ErrorCode::invalid_container, "The database final table has an unknown footer."});
  }
  auto final_table = parse_table(
      trailing, section_start, static_cast<std::uint32_t>(layout.info.tables.size()),
      FooterCandidate{5, TableFooter::final, std::nullopt});
  if (!final_table) {
    return Result<DatabaseLayout>::failure(final_table.error());
  }
  layout.info.visible_rows += final_table.value().info.visible_rows;
  layout.info.invisible_rows += final_table.value().info.invisible_rows;
  layout.info.tables.push_back(final_table.value().info);
  layout.tables.push_back(std::move(final_table.value()));
  return Result<DatabaseLayout>::success(std::move(layout));
}

}  // namespace tekla::db1::detail
