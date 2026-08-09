#include <zlib.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

void append_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::byte>(value & 0xffU));
  bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void write_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32U; shift += 8U) {
    bytes[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void append_ascii(std::vector<std::byte>& bytes, const char* text) {
  const auto size = std::strlen(text);
  const auto* first = reinterpret_cast<const std::byte*>(text);
  bytes.insert(bytes.end(), first, first + size);
}

void append_record(std::vector<std::byte>& bytes, std::uint8_t marker, std::uint32_t value) {
  bytes.push_back(static_cast<std::byte>(marker));
  append_u32(bytes, value);
  append_u32(bytes, 7);
  append_u32(bytes, 9);
}

std::vector<std::byte> gzip_member(std::span<const std::byte> input) {
  z_stream stream{};
  CHECK(deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) ==
            Z_OK,
        "the test gzip encoder initializes");

  std::vector<std::byte> output(deflateBound(&stream, static_cast<uLong>(input.size())));
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());
  CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END, "the test gzip member completes");
  output.resize(stream.total_out);
  deflateEnd(&stream);
  return output;
}

std::vector<std::byte> stored_zip(std::string_view name, std::span<const std::byte> payload) {
  const auto checksum =
      crc32(0U, reinterpret_cast<const Bytef*>(payload.data()), static_cast<uInt>(payload.size()));
  std::vector<std::byte> bytes;
  append_u32(bytes, 0x04034b50U);
  append_u16(bytes, 20U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u32(bytes, static_cast<std::uint32_t>(checksum));
  append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
  append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
  append_u16(bytes, static_cast<std::uint16_t>(name.size()));
  append_u16(bytes, 0U);
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(name.data()),
               reinterpret_cast<const std::byte*>(name.data() + name.size()));
  bytes.insert(bytes.end(), payload.begin(), payload.end());

  const auto central_offset = static_cast<std::uint32_t>(bytes.size());
  append_u32(bytes, 0x02014b50U);
  append_u16(bytes, 20U);
  append_u16(bytes, 20U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u32(bytes, static_cast<std::uint32_t>(checksum));
  append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
  append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
  append_u16(bytes, static_cast<std::uint16_t>(name.size()));
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u32(bytes, 0U);
  append_u32(bytes, 0U);
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(name.data()),
               reinterpret_cast<const std::byte*>(name.data() + name.size()));

  const auto central_size = static_cast<std::uint32_t>(bytes.size()) - central_offset;
  append_u32(bytes, 0x06054b50U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u16(bytes, 1U);
  append_u16(bytes, 1U);
  append_u32(bytes, central_size);
  append_u32(bytes, central_offset);
  append_u16(bytes, 0U);
  return bytes;
}

std::vector<std::byte> stored_zip_with_declared_size(std::string_view name,
                                                     std::span<const std::byte> payload,
                                                     std::uint32_t declared_size) {
  auto bytes = stored_zip(name, payload);
  write_u32(bytes, 22U, declared_size);
  const auto central_offset = 30U + name.size() + payload.size();
  write_u32(bytes, central_offset + 24U, declared_size);
  return bytes;
}

std::vector<std::byte> stored_zip_with_repeated_entry(std::string_view name,
                                                      std::span<const std::byte> payload,
                                                      std::uint16_t count) {
  const auto single = stored_zip(name, payload);
  const auto local_size = 30U + name.size() + payload.size();
  const auto central_size = 46U + name.size();
  std::vector<std::byte> bytes(single.begin(),
                               single.begin() + static_cast<std::ptrdiff_t>(local_size));
  const auto central = std::span(single).subspan(local_size, central_size);
  bytes.reserve(local_size + central_size * count + 22U);
  for (std::uint16_t index = 0U; index < count; ++index) {
    bytes.insert(bytes.end(), central.begin(), central.end());
  }
  append_u32(bytes, 0x06054b50U);
  append_u16(bytes, 0U);
  append_u16(bytes, 0U);
  append_u16(bytes, count);
  append_u16(bytes, count);
  append_u32(bytes, static_cast<std::uint32_t>(central_size * count));
  append_u32(bytes, static_cast<std::uint32_t>(local_size));
  append_u16(bytes, 0U);
  return bytes;
}

std::vector<std::byte> synthetic_database() {
  constexpr std::array<std::byte, 4> table_end{std::byte{0x66}, std::byte{0xc0}, std::byte{0xce},
                                               std::byte{0xdb}};
  constexpr std::array<std::byte, 4> final_footer{std::byte{0x4f}, std::byte{0x61}, std::byte{0xbc},
                                                  std::byte{0x00}};

  std::vector<std::byte> payload;
  append_ascii(payload, "Xsteel");
  payload.push_back(std::byte{0x85});
  append_ascii(payload, " 9.66 7d72d8c9-0250-4f3a-8760-bcef517f016e");
  append_u32(payload, 1);
  payload.insert(payload.end(), table_end.begin(), table_end.end());

  append_u32(payload, 4);
  append_u32(payload, 1);
  append_u32(payload, 0);
  append_record(payload, 0x00, 42);
  payload.push_back(std::byte{0});
  payload.insert(payload.end(), table_end.begin(), table_end.end());

  append_u32(payload, 4);
  append_u32(payload, 1);
  append_u32(payload, 0);
  append_record(payload, 0x08, 84);
  payload.push_back(std::byte{0});
  payload.insert(payload.end(), final_footer.begin(), final_footer.end());
  return payload;
}

}  // namespace

int main() {
  using namespace tekla::db1;

  auto missing = open(ModelPackage{});
  CHECK(!missing.has_value(), "a package without a model database is rejected");
  CHECK(missing.error().code == ErrorCode::missing_model_database,
        "the missing-database error is stable and machine-readable");

  std::array<std::byte, 4> bytes{};
  ModelPackage package;
  package.add(Asset::copy(AssetRole::model_database, "model.db1", bytes));

  auto opened = open(std::move(package));
  CHECK(!opened.has_value(), "a malformed database is rejected while opening");
  CHECK(opened.error().code == ErrorCode::unsupported_format,
        "an unsupported database header is machine-readable");

  auto raw = synthetic_database();
  const auto midpoint = raw.size() / 2;
  auto first_member = gzip_member(std::span(raw).first(midpoint));
  auto second_member = gzip_member(std::span(raw).subspan(midpoint));
  first_member.insert(first_member.end(), second_member.begin(), second_member.end());

  ModelPackage synthetic_package;
  synthetic_package.add(Asset::copy(AssetRole::model_database, "synthetic.db1", first_member));
  auto synthetic = open(std::move(synthetic_package));
  CHECK(synthetic.has_value(), "a multi-member gzip database opens");
  if (synthetic) {
    const auto& info = synthetic.value().info();
    CHECK(info.wrapper == Wrapper::gzip, "the gzip wrapper is reported");
    CHECK(info.product == "Xsteel", "the database product is reported");
    CHECK(info.kind_marker == 0x85, "the database kind marker is reported");
    CHECK(info.internal_format == "9.66", "the internal format is reported");
    CHECK(info.database_uuid == "7d72d8c9-0250-4f3a-8760-bcef517f016e",
          "the database UUID is reported");
    CHECK(info.payload_bytes == raw.size(), "the complete multi-member payload is inflated");
    CHECK(info.tables.size() == 2, "ordinary and final tables are framed");
    CHECK(info.visible_rows == 1, "visible row markers are counted");
    CHECK(info.invisible_rows == 1, "invisible row markers are counted");
    CHECK(info.tables[0].tuple_size == 4 && info.tables[0].field_count == 1,
          "table tuple metadata is decoded");
    CHECK(info.tables[0].footer == TableFooter::plain, "a plain delimited table footer is decoded");
    CHECK(info.tables[1].footer == TableFooter::final, "the final table footer is decoded");
  }

  ModelPackage limited_package;
  limited_package.add(Asset::copy(AssetRole::model_database, "limited.db1", raw));
  OpenOptions limited_options;
  limited_options.max_payload_bytes = raw.size() - 1;
  auto limited = open(std::move(limited_package), limited_options);
  CHECK(!limited.has_value() && limited.error().code == ErrorCode::resource_limit,
        "raw payloads obey the same memory limit as compressed payloads");

  const auto mapped_path = std::filesystem::current_path() / "mapped-model.db1";
  {
    std::ofstream output(mapped_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(raw.data()),
                 static_cast<std::streamsize>(raw.size()));
  }
  auto mapped_asset = map_asset(AssetRole::model_database, "mapped-model.db1", mapped_path);
  CHECK(mapped_asset.has_value(), "a model file can be memory mapped");
  std::filesystem::remove(mapped_path);
  if (mapped_asset) {
    ModelPackage mapped_package;
    mapped_package.add(std::move(mapped_asset.value()));
    auto mapped = open(std::move(mapped_package));
    CHECK(mapped.has_value(), "a mapping owns the file pages after the directory entry is removed");
    if (mapped) {
      CHECK(mapped.value().info().wrapper == Wrapper::raw, "a raw mapped payload is zero-copy");
    }
  }

  auto absent_asset = map_asset(AssetRole::model_database, "absent.db1",
                                std::filesystem::current_path() / "absent.db1");
  CHECK(!absent_asset.has_value() && absent_asset.error().code == ErrorCode::io_error,
        "mapping an absent file reports an I/O error");

  const auto package_directory =
      std::filesystem::temp_directory_path() /
      ("tekla-db1-package-test-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(package_directory / "Shapes");
  std::filesystem::create_directories(package_directory / "ShapeGeometries");
  std::filesystem::create_directories(package_directory / "Environment" / "Profiles");
  const auto package_database = package_directory / "package.db1";
  {
    std::ofstream output(package_database, std::ios::binary);
    output.write(reinterpret_cast<const char*>(raw.data()),
                 static_cast<std::streamsize>(raw.size()));
  }
  const auto component_database = package_directory / "xslib.db1";
  {
    constexpr std::array<std::byte, 8> component_bytes{
        std::byte{'X'}, std::byte{'s'}, std::byte{'t'}, std::byte{'e'},
        std::byte{'e'}, std::byte{'l'}, std::byte{0},   std::byte{1}};
    std::ofstream output(component_database, std::ios::binary);
    output.write(reinterpret_cast<const char*>(component_bytes.data()),
                 static_cast<std::streamsize>(component_bytes.size()));
  }
  {
    constexpr std::array<std::byte, 4> material_bytes{
        std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08}, std::byte{0x00}};
    std::ofstream output(package_directory / "matdb.bin", std::ios::binary);
    output.write(reinterpret_cast<const char*>(material_bytes.data()),
                 static_cast<std::streamsize>(material_bytes.size()));
  }
  {
    std::ofstream(package_directory / "Shapes" / "shape.xml") << "<ImportPart/>";
    std::ofstream(package_directory / "ShapeGeometries" / "shape.tez") << "geometry";
    std::ofstream(package_directory / "Environment" / "Profiles" / "custom.lis")
        << "PROFILE_NAME = custom";
  }
  {
    constexpr std::string_view profile =
        "PROFILE DATABASE EXPORT VERSION = 3\nPROFILE_NAME = \"archived\";\n";
    const auto archive = stored_zip("Template/PG.lis", std::as_bytes(std::span(profile)));
    std::ofstream output(package_directory / "template.zip", std::ios::binary);
    output.write(reinterpret_cast<const char*>(archive.data()),
                 static_cast<std::streamsize>(archive.size()));
  }
  {
    constexpr std::string_view metadata =
        "<ImportPart><Guid>archived-shape</Guid>"
        "<BrepStorageId>archived-storage</BrepStorageId></ImportPart>";
    const auto archive =
        stored_zip("Project/Shapes/archived-shape.xml", std::as_bytes(std::span(metadata)));
    std::ofstream output(package_directory / "shape-metadata.zip", std::ios::binary);
    output.write(reinterpret_cast<const char*>(archive.data()),
                 static_cast<std::streamsize>(archive.size()));
  }
  {
    constexpr std::string_view geometry = "<Polymesh/>";
    const auto archive = stored_zip("Project/ShapeGeometries/archived-storage.tez",
                                    std::as_bytes(std::span(geometry)));
    std::ofstream output(package_directory / "shape-geometry.zip", std::ios::binary);
    output.write(reinterpret_cast<const char*>(archive.data()),
                 static_cast<std::streamsize>(archive.size()));
  }
  {
    constexpr std::string_view unrelated = "<NotAShape/>";
    const auto archive =
        stored_zip("Project/NotShapes/unrelated.xml", std::as_bytes(std::span(unrelated)));
    std::ofstream output(package_directory / "unrelated.zip", std::ios::binary);
    output.write(reinterpret_cast<const char*>(archive.data()),
                 static_cast<std::streamsize>(archive.size()));
  }
  auto mapped_model_package = map_model_package(package_database);
  CHECK(mapped_model_package.has_value(),
        "a model directory maps through the native convenience API");
  if (mapped_model_package) {
    CHECK(mapped_model_package.value().assets().size() == 9,
          "the component and material databases, shape metadata, shape geometry, and bounded "
          "archive catalogs join the database package");
    CHECK(mapped_model_package.value().find_first(AssetRole::model_database) != nullptr,
          "the mapped package identifies its model database");
    const auto* components = mapped_model_package.value().find_first(AssetRole::component_catalog);
    CHECK(components != nullptr && components->logical_name() == "xslib.db1" &&
              components->source()->bytes().size() == 8,
          "the mapped package identifies the model-local custom-component database");
    const auto* materials = mapped_model_package.value().find_first(AssetRole::material_catalog);
    CHECK(materials != nullptr && materials->logical_name() == "matdb.bin" &&
              materials->source()->bytes().size() == 4,
          "the mapped package identifies the model-local material database");
  }
  std::filesystem::remove_all(package_directory);

  const auto archive_limit_directory = package_directory.string() + "-archive-limit";
  std::filesystem::create_directories(archive_limit_directory);
  const auto archive_limit_database =
      std::filesystem::path(archive_limit_directory) / "package.db1";
  {
    std::ofstream output(archive_limit_database, std::ios::binary);
    output.write(reinterpret_cast<const char*>(raw.data()),
                 static_cast<std::streamsize>(raw.size()));
  }
  constexpr std::string_view tiny_profile = "PROFILE_NAME = bounded";
  const auto tiny_archive = stored_zip("Template/PG.lis", std::as_bytes(std::span(tiny_profile)));
  for (std::size_t index = 0; index < 65U; ++index) {
    std::ofstream output(std::filesystem::path(archive_limit_directory) /
                             ("catalog-" + std::to_string(index) + ".zip"),
                         std::ios::binary);
    output.write(reinterpret_cast<const char*>(tiny_archive.data()),
                 static_cast<std::streamsize>(tiny_archive.size()));
  }
  auto excessive_archives = map_model_package(archive_limit_database);
  CHECK(!excessive_archives.has_value() &&
            excessive_archives.error().code == ErrorCode::resource_limit,
        "adjacent archives obey one package-wide count limit");
  std::filesystem::remove_all(archive_limit_directory);

  const auto entry_limit_directory = package_directory.string() + "-entry-limit";
  std::filesystem::create_directories(entry_limit_directory);
  const auto entry_limit_database = std::filesystem::path(entry_limit_directory) / "package.db1";
  {
    std::ofstream output(entry_limit_database, std::ios::binary);
    output.write(reinterpret_cast<const char*>(raw.data()),
                 static_cast<std::streamsize>(raw.size()));
  }
  constexpr std::string_view unrelated_payload = "x";
  const auto many_entries = stored_zip_with_repeated_entry(
      "Other/data.txt", std::as_bytes(std::span(unrelated_payload)), 32'769U);
  for (std::size_t index = 0; index < 2U; ++index) {
    std::ofstream output(std::filesystem::path(entry_limit_directory) /
                             ("entries-" + std::to_string(index) + ".zip"),
                         std::ios::binary);
    output.write(reinterpret_cast<const char*>(many_entries.data()),
                 static_cast<std::streamsize>(many_entries.size()));
  }
  auto excessive_entries = map_model_package(entry_limit_database);
  CHECK(
      !excessive_entries.has_value() && excessive_entries.error().code == ErrorCode::resource_limit,
      "central-directory entries obey one package-wide count limit");
  std::filesystem::remove_all(entry_limit_directory);

  const auto inflated_limit_directory = package_directory.string() + "-inflated-limit";
  std::filesystem::create_directories(inflated_limit_directory);
  const auto inflated_limit_database =
      std::filesystem::path(inflated_limit_directory) / "package.db1";
  {
    std::ofstream output(inflated_limit_database, std::ios::binary);
    output.write(reinterpret_cast<const char*>(raw.data()),
                 static_cast<std::streamsize>(raw.size()));
  }
  const auto declared_large_archive = stored_zip_with_declared_size(
      "Template/PG.lis", std::as_bytes(std::span(tiny_profile)), 140U * 1024U * 1024U);
  for (std::size_t index = 0; index < 2U; ++index) {
    std::ofstream output(std::filesystem::path(inflated_limit_directory) /
                             ("inflated-" + std::to_string(index) + ".zip"),
                         std::ios::binary);
    output.write(reinterpret_cast<const char*>(declared_large_archive.data()),
                 static_cast<std::streamsize>(declared_large_archive.size()));
  }
  auto excessive_inflated_bytes = map_model_package(inflated_limit_database);
  CHECK(!excessive_inflated_bytes.has_value() &&
            excessive_inflated_bytes.error().code == ErrorCode::resource_limit,
        "declared inflated bytes obey one package-wide allocation limit");
  std::filesystem::remove_all(inflated_limit_directory);

  if (failures != 0) {
    std::printf("FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("ALL PASS\n");
  return 0;
}
