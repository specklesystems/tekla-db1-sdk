#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <tekla/db1/package.hpp>
#include <utility>
#include <vector>

#include "archive_budget.hpp"

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fstream>
#endif

namespace tekla::db1 {
namespace {

#if defined(_WIN32)

class MappedFile final : public ByteSource {
 public:
  MappedFile(HANDLE file, HANDLE mapping, void* address, std::size_t size)
      : file_(file), mapping_(mapping), address_(address), size_(size) {}

  ~MappedFile() override {
    if (address_ != nullptr) {
      UnmapViewOfFile(address_);
    }
    if (mapping_ != nullptr) {
      CloseHandle(mapping_);
    }
    if (file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(file_);
    }
  }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept override {
    return {static_cast<const std::byte*>(address_), size_};
  }

 private:
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
  void* address_ = nullptr;
  std::size_t size_ = 0;
};

[[nodiscard]] Result<std::shared_ptr<const ByteSource>> map_source(
    const std::filesystem::path& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error, "Could not open the model database for mapping."});
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0 ||
      static_cast<unsigned long long>(size.QuadPart) >
          static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
    CloseHandle(file);
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error, "The model database is empty or too large to map."});
  }
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping == nullptr) {
    CloseHandle(file);
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error, "Could not create a read-only model database mapping."});
  }
  void* address = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (address == nullptr) {
    CloseHandle(mapping);
    CloseHandle(file);
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error, "Could not map the model database into memory."});
  }
  return Result<std::shared_ptr<const ByteSource>>::success(std::make_shared<MappedFile>(
      file, mapping, address, static_cast<std::size_t>(size.QuadPart)));
}

#elif defined(__unix__) || defined(__APPLE__)

class MappedFile final : public ByteSource {
 public:
  MappedFile(void* address, std::size_t size) : address_(address), size_(size) {}
  ~MappedFile() override { munmap(address_, size_); }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept override {
    return {static_cast<const std::byte*>(address_), size_};
  }

 private:
  void* address_ = nullptr;
  std::size_t size_ = 0;
};

[[nodiscard]] Result<std::shared_ptr<const ByteSource>> map_source(
    const std::filesystem::path& path) {
  const int descriptor = open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error,
         std::string("Could not open the model database: ") + std::strerror(errno)});
  }
  struct stat status{};
  if (fstat(descriptor, &status) != 0 || status.st_size <= 0 ||
      static_cast<std::uintmax_t>(status.st_size) >
          static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    close(descriptor);
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error, "The model database is empty or too large to map."});
  }
  const auto size = static_cast<std::size_t>(status.st_size);
  void* address = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, descriptor, 0);
  const int mapping_error = errno;
  close(descriptor);
  if (address == MAP_FAILED) {
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error,
         std::string("Could not map the model database: ") + std::strerror(mapping_error)});
  }
  return Result<std::shared_ptr<const ByteSource>>::success(
      std::make_shared<MappedFile>(address, size));
}

#else

class ReadFile final : public ByteSource {
 public:
  explicit ReadFile(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept override { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
};

[[nodiscard]] Result<std::shared_ptr<const ByteSource>> map_source(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error, "Could not open the model database."});
  }
  const auto end = input.tellg();
  if (end <= 0 || static_cast<std::uintmax_t>(end) >
                      static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error, "The model database is empty or too large to read."});
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), end);
  if (!input) {
    return Result<std::shared_ptr<const ByteSource>>::failure(
        {ErrorCode::io_error, "Could not read the model database."});
  }
  return Result<std::shared_ptr<const ByteSource>>::success(
      std::make_shared<ReadFile>(std::move(bytes)));
}

#endif

class LazyMappedFile final : public ByteSource {
 public:
  explicit LazyMappedFile(std::filesystem::path path) : path_(std::move(path)) {}

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept override {
    try {
      std::call_once(mapped_, [this] {
        auto source = map_source(path_);
        if (source) source_ = std::move(source.value());
      });
    } catch (...) {
      // ByteSource is a noexcept pull interface. A failed optional sidecar is
      // exposed as an empty source and diagnosed only if its stage is asked for.
    }
    return source_ == nullptr ? std::span<const std::byte>{} : source_->bytes();
  }

 private:
  std::filesystem::path path_;
  mutable std::once_flag mapped_;
  mutable std::shared_ptr<const ByteSource> source_;
};

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
         static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (unsigned index = 0; index != 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

[[nodiscard]] bool range_is_valid(std::span<const std::byte> bytes, std::size_t offset,
                                  std::size_t size) noexcept {
  return offset <= bytes.size() && size <= bytes.size() - offset;
}

[[nodiscard]] std::optional<std::size_t> zip_directory_offset(
    std::span<const std::byte> bytes) noexcept {
  constexpr std::uint32_t signature = 0x06054b50U;
  constexpr std::size_t minimum_size = 22U;
  constexpr std::size_t maximum_comment_size = 65'535U;
  if (bytes.size() < minimum_size) return std::nullopt;
  const auto begin = bytes.size() > minimum_size + maximum_comment_size
                         ? bytes.size() - minimum_size - maximum_comment_size
                         : 0U;
  for (std::size_t offset = bytes.size() - minimum_size;; --offset) {
    if (read_u32(bytes, offset) == signature &&
        range_is_valid(bytes, offset, minimum_size + read_u16(bytes, offset + 20U))) {
      return offset;
    }
    if (offset == begin) break;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::byte>> inflate_zip_entry(
    std::span<const std::byte> compressed, std::size_t uncompressed_size, std::uint16_t method) {
  constexpr std::size_t maximum_entry_size = 64U * 1024U * 1024U;
  if (uncompressed_size > maximum_entry_size ||
      compressed.size() > std::numeric_limits<uInt>::max() ||
      uncompressed_size > std::numeric_limits<uInt>::max()) {
    return std::nullopt;
  }
  if ((method == 0U && compressed.size() != uncompressed_size) || (method != 0U && method != 8U)) {
    return std::nullopt;
  }
  std::vector<std::byte> output(uncompressed_size);
  if (method == 0U) {
    std::copy(compressed.begin(), compressed.end(), output.begin());
    return output;
  }
  if (output.empty()) return std::nullopt;

  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return std::nullopt;
  const int status = inflate(&stream, Z_FINISH);
  const bool complete = status == Z_STREAM_END && stream.total_out == output.size();
  inflateEnd(&stream);
  return complete ? std::optional{std::move(output)} : std::nullopt;
}

enum class ArchiveAppend { ignored, appended, resource_limit };

[[nodiscard]] ArchiveAppend append_zip_catalogs(ModelPackage& package,
                                                const std::filesystem::path& archive_path,
                                                detail::ArchiveBudget& budget) {
  auto source = map_source(archive_path);
  if (!source) return ArchiveAppend::ignored;
  const auto bytes = source.value()->bytes();
  const auto end = zip_directory_offset(bytes);
  if (!end || read_u16(bytes, *end + 4U) != 0U || read_u16(bytes, *end + 6U) != 0U) {
    return ArchiveAppend::ignored;
  }
  const auto entries = read_u16(bytes, *end + 10U);
  const auto directory_size = read_u32(bytes, *end + 12U);
  std::size_t cursor = read_u32(bytes, *end + 16U);
  if (entries == std::numeric_limits<std::uint16_t>::max() ||
      directory_size == std::numeric_limits<std::uint32_t>::max() ||
      !range_is_valid(bytes, cursor, directory_size)) {
    return ArchiveAppend::ignored;
  }
  if (!budget.reserve_entries(entries)) {
    return ArchiveAppend::resource_limit;
  }

  constexpr std::uint32_t central_signature = 0x02014b50U;
  constexpr std::uint32_t local_signature = 0x04034b50U;
  constexpr std::size_t central_header_size = 46U;
  constexpr std::size_t local_header_size = 30U;
  bool appended = false;
  for (std::uint16_t index = 0; index != entries; ++index) {
    if (!range_is_valid(bytes, cursor, central_header_size) ||
        read_u32(bytes, cursor) != central_signature) {
      return ArchiveAppend::ignored;
    }
    const auto flags = read_u16(bytes, cursor + 8U);
    const auto method = read_u16(bytes, cursor + 10U);
    const auto checksum = read_u32(bytes, cursor + 16U);
    const auto compressed_size = read_u32(bytes, cursor + 20U);
    const auto uncompressed_size = read_u32(bytes, cursor + 24U);
    const auto name_size = read_u16(bytes, cursor + 28U);
    const auto extra_size = read_u16(bytes, cursor + 30U);
    const auto comment_size = read_u16(bytes, cursor + 32U);
    const auto local_offset = read_u32(bytes, cursor + 42U);
    const auto record_size = central_header_size + name_size + extra_size + comment_size;
    if (!range_is_valid(bytes, cursor, record_size)) return ArchiveAppend::ignored;
    const std::string_view name(
        reinterpret_cast<const char*>(bytes.data() + cursor + central_header_size), name_size);
    auto normalized = std::string(name);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    // Tekla project templates use both one-profile-per-file assets under
    // Environment/Profiles and a root PG.lis containing the full catalog.
    // The profile decoder ignores non-profile records, so accepting bounded
    // LIS entries is both conservative and compatible with either layout.
    const bool is_profile = normalized.ends_with(".lis");
    const auto contains_directory = [&](std::string_view directory) {
      for (auto offset = normalized.find(directory); offset != std::string::npos;
           offset = normalized.find(directory, offset + 1U)) {
        if (offset == 0U || normalized[offset - 1U] == '/') return true;
      }
      return false;
    };
    const bool is_shape_geometry = contains_directory("shapegeometries/") &&
                                   (normalized.ends_with(".tez") || normalized.ends_with(".xml"));
    const bool is_shape_metadata =
        !is_shape_geometry && contains_directory("shapes/") && normalized.ends_with(".xml");
    const bool is_catalog = is_profile || is_shape_geometry || is_shape_metadata;
    if (is_catalog && (flags & 1U) == 0U &&
        compressed_size != std::numeric_limits<std::uint32_t>::max() &&
        uncompressed_size != std::numeric_limits<std::uint32_t>::max() &&
        range_is_valid(bytes, local_offset, local_header_size) &&
        read_u32(bytes, local_offset) == local_signature) {
      const auto local_name_size = read_u16(bytes, local_offset + 26U);
      const auto local_extra_size = read_u16(bytes, local_offset + 28U);
      const auto data_offset =
          local_offset + local_header_size + local_name_size + local_extra_size;
      if (!budget.reserve_inflated_bytes(uncompressed_size)) {
        return ArchiveAppend::resource_limit;
      }
      if (range_is_valid(bytes, data_offset, compressed_size)) {
        auto payload = inflate_zip_entry(bytes.subspan(data_offset, compressed_size),
                                         uncompressed_size, method);
        if (payload) {
          const auto actual_checksum = crc32(0U, reinterpret_cast<const Bytef*>(payload->data()),
                                             static_cast<uInt>(payload->size()));
          if (actual_checksum == checksum) {
            package.add(Asset::copy(AssetRole::catalog_snapshot, std::string(name), *payload));
            appended = true;
          }
        }
      }
    }
    cursor += record_size;
  }
  return appended ? ArchiveAppend::appended : ArchiveAppend::ignored;
}

}  // namespace

Result<Asset> map_asset(AssetRole role, std::string logical_name,
                        const std::filesystem::path& path) {
  auto source = map_source(path);
  if (!source) {
    return Result<Asset>::failure(source.error());
  }
  return Result<Asset>::success(Asset(role, std::move(logical_name), std::move(source.value())));
}

Result<ModelPackage> map_model_package(const std::filesystem::path& model_database_path) {
  auto database = map_asset(AssetRole::model_database, model_database_path.filename().string(),
                            model_database_path);
  if (!database) return Result<ModelPackage>::failure(database.error());
  ModelPackage package;
  package.add(std::move(database.value()));
  const auto root = model_database_path.parent_path();
  {
    std::error_code error;
    const auto component_database = root / "xslib.db1";
    if (std::filesystem::is_regular_file(component_database, error) && !error) {
      package.add(Asset(AssetRole::component_catalog, "xslib.db1",
                        std::make_shared<LazyMappedFile>(component_database)));
    }
  }
  {
    std::error_code error;
    const auto material_database = root / "matdb.bin";
    if (std::filesystem::is_regular_file(material_database, error) && !error) {
      package.add(Asset(AssetRole::material_catalog, "matdb.bin",
                        std::make_shared<LazyMappedFile>(material_database)));
    }
  }
  constexpr std::array<std::string_view, 3> directories{"Shapes", "ShapeGeometries",
                                                        "Environment/Profiles"};
  std::vector<std::filesystem::path> sidecars;
  for (const auto relative : directories) {
    std::error_code error;
    const auto directory = root / relative;
    if (!std::filesystem::is_directory(directory, error)) continue;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (iterator->is_regular_file(error) && !error) {
        auto extension = iterator->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const bool accepted = relative == "Shapes" ? extension == ".xml"
                              : relative == "ShapeGeometries"
                                  ? extension == ".tez" || extension == ".xml"
                                  : extension == ".lis";
        if (accepted) sidecars.push_back(iterator->path());
      }
    }
    if (error) {
      return Result<ModelPackage>::failure(
          {ErrorCode::io_error, "Could not enumerate model-local assets."});
    }
  }
  std::sort(sidecars.begin(), sidecars.end());
  for (const auto& path : sidecars) {
    std::error_code error;
    auto relative = std::filesystem::relative(path, root, error);
    if (error) relative = path.filename();
    auto asset = map_asset(AssetRole::catalog_snapshot, relative.generic_string(), path);
    if (!asset) return Result<ModelPackage>::failure(asset.error());
    package.add(std::move(asset.value()));
  }
  std::vector<std::filesystem::path> archives;
  detail::ArchiveBudget archive_budget;
  std::error_code archive_error;
  for (std::filesystem::directory_iterator iterator(root, archive_error), end;
       !archive_error && iterator != end; iterator.increment(archive_error)) {
    if (!iterator->is_regular_file(archive_error) || archive_error) continue;
    auto extension = iterator->path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".zip") {
      if (!archive_budget.reserve_archives(1U)) {
        return Result<ModelPackage>::failure(
            {ErrorCode::resource_limit, "The adjacent archive count exceeds the package limit."});
      }
      archives.push_back(iterator->path());
    }
  }
  if (archive_error) {
    return Result<ModelPackage>::failure(
        {ErrorCode::io_error, "Could not enumerate adjacent package archives."});
  }
  std::sort(archives.begin(), archives.end());
  for (const auto& archive : archives) {
    if (append_zip_catalogs(package, archive, archive_budget) == ArchiveAppend::resource_limit) {
      return Result<ModelPackage>::failure(
          {ErrorCode::resource_limit, "Adjacent archives exceed the package resource limit."});
    }
  }
  return Result<ModelPackage>::success(std::move(package));
}

}  // namespace tekla::db1
