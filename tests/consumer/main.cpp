#include <tekla/db1/tekla_db1.hpp>

#if defined(TEKLA_DB1_CONSUMER_WITH_GLTF)
#include <sstream>
#include <tekla/db1/adapters/gltf.hpp>

namespace {

class EmptyReader final : public tekla::db1::BatchReader {
 public:
  tekla::db1::Result<tekla::db1::BatchView> next() override {
    return tekla::db1::Result<tekla::db1::BatchView>::success(
        {.kind = tekla::db1::BatchKind::end});
  }
};

}  // namespace
#endif

static_assert(tekla::db1::version_major == TEKLA_DB1_EXPECTED_VERSION_MAJOR);
static_assert(tekla::db1::version_minor == TEKLA_DB1_EXPECTED_VERSION_MINOR);
static_assert(tekla::db1::version_patch == TEKLA_DB1_EXPECTED_VERSION_PATCH);
static_assert(tekla::db1::source_revision == TEKLA_DB1_EXPECTED_SOURCE_REVISION);

int main() {
  tekla::db1::ModelPackage package;
  if (!package.assets().empty() || tekla::db1::version != TEKLA_DB1_EXPECTED_VERSION) return 1;
#if defined(TEKLA_DB1_CONSUMER_WITH_GLTF)
  EmptyReader reader;
  std::ostringstream output(std::ios::binary);
  auto report = tekla::db1::gltf::write_glb(reader, output);
  if (!report || report.value().mesh_count != 0 || report.value().curve_count != 0 ||
      report.value().bytes_written == 0 || output.str().empty()) {
    return 1;
  }
#endif
  return 0;
}
