#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <tekla/db1/model.hpp>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition, message)         \
  do {                                    \
    if (!(condition)) {                   \
      std::printf("FAIL: %s\n", message); \
      ++failures;                         \
    }                                     \
  } while (false)

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void append_ascii(std::vector<std::byte>& bytes, std::string_view text) {
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(text.data()),
               reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

void append_record(std::vector<std::byte>& bytes, std::uint8_t marker, std::uint32_t value) {
  bytes.push_back(static_cast<std::byte>(marker));
  append_u32(bytes, value);
  append_u32(bytes, 7);
  append_u32(bytes, 9);
}

std::vector<std::byte> physical_database(std::string_view format = "9.66") {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  bytes.push_back(std::byte{' '});
  append_ascii(bytes, format);
  append_ascii(bytes, " 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());

  append_u32(bytes, 4);
  append_u32(bytes, 1);
  append_u32(bytes, 0);
  append_record(bytes, 0x00, 42);
  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());

  append_u32(bytes, 4);
  append_u32(bytes, 1);
  append_u32(bytes, 0);
  append_record(bytes, 0x08, 84);
  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final_footer.begin(), final_footer.end());
  return bytes;
}

std::vector<std::byte> gzip_member(std::span<const std::byte> input) {
  z_stream stream{};
  CHECK(deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) ==
            Z_OK,
        "the robustness gzip encoder initializes");
  std::vector<std::byte> output(deflateBound(&stream, input.size()));
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());
  CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END, "the robustness gzip member completes");
  output.resize(stream.total_out);
  deflateEnd(&stream);
  return output;
}

tekla::db1::Result<tekla::db1::Model> open_bytes(std::span<const std::byte> bytes,
                                                 const tekla::db1::OpenOptions& options = {}) {
  tekla::db1::ModelPackage package;
  package.add(
      tekla::db1::Asset::copy(tekla::db1::AssetRole::model_database, "synthetic.db1", bytes));
  return tekla::db1::open(std::move(package), options);
}

}  // namespace

int main() {
  using namespace tekla::db1;

  const std::vector<std::byte> empty;
  auto empty_result = open_bytes(empty);
  CHECK(!empty_result && empty_result.error().code == ErrorCode::invalid_argument,
        "an empty model asset fails with invalid_argument");

  ModelPackage null_package;
  null_package.add(Asset(AssetRole::model_database, "null.db1", nullptr));
  auto null_result = open(std::move(null_package));
  CHECK(!null_result && null_result.error().code == ErrorCode::missing_model_database,
        "a null model source fails with missing_model_database");

  const auto valid = physical_database();
  auto valid_result = open_bytes(valid);
  CHECK(valid_result.has_value(), "the robustness fixture is physically valid");

  auto bad_uuid = valid;
  bad_uuid[20] = std::byte{'z'};
  auto bad_uuid_result = open_bytes(bad_uuid);
  CHECK(!bad_uuid_result && bad_uuid_result.error().code == ErrorCode::unsupported_format,
        "a malformed UUID fails before table parsing");

  constexpr std::array delimiter{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                 std::byte{0xdb}};
  const auto first_delimiter =
      std::search(valid.begin(), valid.end(), delimiter.begin(), delimiter.end());
  std::vector<std::byte> header_only(valid.begin(), first_delimiter);
  auto no_tables = open_bytes(header_only);
  CHECK(!no_tables && no_tables.error().code == ErrorCode::invalid_container,
        "a valid header without physical tables fails container validation");
  OpenOptions header_options;
  header_options.validate_container = false;
  auto unchecked_header = open_bytes(header_only, header_options);
  CHECK(unchecked_header.has_value() && unchecked_header.value().info().tables.empty(),
        "callers can explicitly request header-only inspection");

  auto truncated = valid;
  truncated.pop_back();
  auto truncated_result = open_bytes(truncated);
  CHECK(!truncated_result && truncated_result.error().code == ErrorCode::invalid_container,
        "a truncated final footer fails closed");

  auto unaligned = valid;
  unaligned.insert(unaligned.end() - 5, std::byte{0});
  auto unaligned_result = open_bytes(unaligned);
  CHECK(!unaligned_result && unaligned_result.error().code == ErrorCode::invalid_container,
        "an unaligned table body fails closed");

  auto bad_descriptor = valid;
  const auto first_section = static_cast<std::size_t>(first_delimiter - valid.begin()) + 4U;
  write_u32(bad_descriptor, first_section + 8U, 256U);
  auto bad_descriptor_result = open_bytes(bad_descriptor);
  CHECK(
      !bad_descriptor_result && bad_descriptor_result.error().code == ErrorCode::invalid_container,
      "an out-of-range physical descriptor fails closed");

  auto compressed = gzip_member(valid);
  auto gzip_truncated = compressed;
  gzip_truncated.resize(gzip_truncated.size() - 4U);
  auto gzip_truncated_result = open_bytes(gzip_truncated);
  CHECK(!gzip_truncated_result &&
            gzip_truncated_result.error().code == ErrorCode::decompression_failed,
        "a gzip stream without its checksum fails closed");

  auto bad_second_member = compressed;
  bad_second_member.push_back(std::byte{0x1f});
  bad_second_member.push_back(std::byte{0x8b});
  bad_second_member.push_back(std::byte{0x00});
  auto bad_second_result = open_bytes(bad_second_member);
  CHECK(!bad_second_result && bad_second_result.error().code == ErrorCode::decompression_failed,
        "a corrupt concatenated gzip member invalidates the whole source");

  OpenOptions compressed_limit;
  compressed_limit.max_payload_bytes = valid.size() - 1U;
  auto limited = open_bytes(compressed, compressed_limit);
  CHECK(!limited && limited.error().code == ErrorCode::resource_limit,
        "inflation observes the configured payload ceiling");

  auto physical = open_bytes(valid);
  if (physical) {
    ProcessRequest request;
    request.stages = Stage::identities;
    auto mismatched = physical.value().process(request);
    CHECK(!mismatched && mismatched.error().code == ErrorCode::schema_mismatch,
          "a physically valid but structurally incomplete schema fails explicitly");
  }

  const auto unknown = physical_database("9.99");
  auto unknown_model = open_bytes(unknown);
  CHECK(unknown_model.has_value(), "an unknown schema remains inspectable at the physical layer");
  if (unknown_model) {
    ProcessRequest request;
    request.stages = Stage::identities;
    auto unsupported = unknown_model.value().process(request);
    CHECK(!unsupported && unsupported.error().code == ErrorCode::unsupported_format,
          "processing an unknown schema fails explicitly");
  }

  if (failures != 0) {
    std::printf("FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("ALL PASS\n");
  return 0;
}
