#include <cstdint>
#include <tekla/db1/process.hpp>

namespace test_external_adapter {

std::uint64_t count_meshes(tekla::db1::BatchReader& reader) {
  std::uint64_t count = 0;
  for (;;) {
    auto batch = reader.next();
    if (!batch || batch.value().kind == tekla::db1::BatchKind::end) return count;
    if (batch.value().kind == tekla::db1::BatchKind::meshes) {
      count += batch.value().meshes.size();
    }
  }
}

}  // namespace test_external_adapter
