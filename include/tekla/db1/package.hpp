#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <tekla/db1/result.hpp>

namespace tekla::db1 {

enum class AssetRole {
  model_database,
  numbering_database,
  component_catalog,
  material_catalog,
  environment,
  catalog_snapshot,
  auxiliary,
};

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  [[nodiscard]] virtual std::span<const std::byte> bytes() const noexcept = 0;
};

class Asset {
 public:
  Asset(AssetRole role, std::string logical_name, std::shared_ptr<const ByteSource> source);

  static Asset copy(AssetRole role, std::string logical_name, std::span<const std::byte> bytes);

  [[nodiscard]] AssetRole role() const noexcept { return role_; }
  [[nodiscard]] const std::string& logical_name() const noexcept { return logical_name_; }
  [[nodiscard]] const std::shared_ptr<const ByteSource>& source() const noexcept { return source_; }

 private:
  AssetRole role_;
  std::string logical_name_;
  std::shared_ptr<const ByteSource> source_;
};

class ModelPackage {
 public:
  void add(Asset asset);

  [[nodiscard]] const Asset* find_first(AssetRole role) const noexcept;
  [[nodiscard]] std::span<const Asset> assets() const noexcept { return assets_; }

 private:
  std::vector<Asset> assets_;
};

// Maps a native file without copying it into the SDK. The returned asset owns
// the mapping and remains valid if the directory entry is subsequently moved
// or removed. Native filesystem input is intentionally separate from the
// ByteSource contract used by embedded and WebAssembly hosts.
[[nodiscard]] Result<Asset> map_asset(AssetRole role, std::string logical_name,
                                      const std::filesystem::path& path);

// Maps a native model database and readable model-local assets used by optional
// semantic and geometry stages. Embedded and WebAssembly hosts can build the
// same ModelPackage explicitly from their own ByteSource implementations.
[[nodiscard]] Result<ModelPackage> map_model_package(
    const std::filesystem::path& model_database_path);

}  // namespace tekla::db1
