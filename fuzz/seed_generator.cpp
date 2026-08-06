#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <vector>

#include "schema.hpp"

namespace {

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_ascii(std::vector<std::byte>& bytes, std::string_view text) {
  const auto* first = reinterpret_cast<const std::byte*>(text.data());
  bytes.insert(bytes.end(), first, first + text.size());
}

void append_table(std::vector<std::byte>& bytes, const tekla::db1::detail::Schema& schema,
                  const tekla::db1::detail::TableSchema& table, bool final) {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0},
                                               std::byte{0xce}, std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61},
                                                  std::byte{0xbc}, std::byte{0x00}};
  append_u32(bytes, table.tuple_size);
  append_u32(bytes, table.descriptor_count);
  for (const auto descriptor : schema.table_descriptors(table)) append_u32(bytes, descriptor);
  if (table.name == "object") {
    bytes.push_back(std::byte{0});
    append_u32(bytes, 1'201);
    append_u32(bytes, 0);
    append_u32(bytes, 700);
    constexpr std::array<std::uint8_t, 16> guid{0x12, 0x34, 0x56, 0x78, 0x12, 0x34,
                                                0x4a, 0xbc, 0x8d, 0xef, 0x12, 0x34,
                                                0x56, 0x78, 0x9a, 0xbc};
    for (const auto byte : guid) bytes.push_back(static_cast<std::byte>(byte));
    append_u32(bytes, 1);
    append_u32(bytes, 22);
    for (int index = 0; index < 4; ++index) append_u32(bytes, 0);
    append_u32(bytes, 3);
    append_u32(bytes, 4);
    append_u32(bytes, 5);
    append_u32(bytes, 6);
    append_u32(bytes, 7);
    append_u32(bytes, 91);
    append_u32(bytes, 92);
  }
  bytes.push_back(std::byte{0});
  bytes.insert(bytes.end(), final ? final_footer.begin() : table_end.begin(),
               final ? final_footer.end() : table_end.end());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0},
                                               std::byte{0xce}, std::byte{0xdb}};
  const auto* schema = tekla::db1::detail::schema_for("9.66", 0x85);
  if (schema == nullptr) return 1;
  std::vector<std::byte> bytes;
  append_ascii(bytes, "Xsteel");
  bytes.push_back(std::byte{0x85});
  append_ascii(bytes, " 9.66 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(bytes, 1);
  bytes.insert(bytes.end(), table_end.begin(), table_end.end());
  for (std::size_t index = 0; index < schema->tables.size(); ++index) {
    append_table(bytes, *schema, schema->tables[index], index + 1 == schema->tables.size());
  }
  std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output ? 0 : 1;
}
