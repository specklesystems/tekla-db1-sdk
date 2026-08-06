#pragma once

#include "storage.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekla::db1::detail {

struct ShapeMesh {
  std::vector<Vector3d> vertices;
  std::vector<std::uint32_t> indices;
};

// Resolves model-local imported shapes from package assets. Definitions are
// decoded once and retained for cheap per-part instancing.
class ShapeCatalog {
 public:
  explicit ShapeCatalog(const ModelPackage& package);

  [[nodiscard]] const ShapeMesh* resolve(std::string_view guid, Error& error);

 private:
  struct Definition {
    std::string storage_id;
    std::shared_ptr<const ByteSource> geometry;
  };

  std::unordered_map<std::string, Definition> definitions_;
  std::unordered_map<std::string, ShapeMesh> meshes_;
  std::unordered_map<std::string, Error> errors_;
};

}  // namespace tekla::db1::detail
