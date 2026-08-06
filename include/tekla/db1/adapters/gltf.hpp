#pragma once

#include <cstdint>
#include <iosfwd>
#include <tekla/db1/process.hpp>
#include <tekla/db1/result.hpp>

namespace tekla::db1::gltf {

struct WriteReport {
  std::uint64_t mesh_count = 0;
  std::uint64_t curve_count = 0;
  std::uint64_t bytes_written = 0;
};

// Writes a self-contained binary glTF 2.0 document. The adapter depends only on
// public batches; it has no access to model database or geometry implementation details.
[[nodiscard]] Result<WriteReport> write_glb(BatchReader& reader, std::ostream& output);

}  // namespace tekla::db1::gltf
