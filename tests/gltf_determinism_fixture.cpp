#include <array>
#include <cstdint>
#include <fstream>
#include <tekla/db1/adapters/gltf.hpp>

namespace {

class DeterministicReader final : public tekla::db1::BatchReader {
 public:
  tekla::db1::Result<tekla::db1::BatchView> next() override {
    if (batch_++ == 0) {
      return tekla::db1::Result<tekla::db1::BatchView>::success(
          {.kind = tekla::db1::BatchKind::meshes, .meshes = meshes_});
    }
    if (batch_ == 2) {
      return tekla::db1::Result<tekla::db1::BatchView>::success(
          {.kind = tekla::db1::BatchKind::curves, .curves = curves_});
    }
    return tekla::db1::Result<tekla::db1::BatchView>::success(
        {.kind = tekla::db1::BatchKind::end});
  }

 private:
  std::uint32_t batch_ = 0;
  const std::array<float, 9> positions_{0.0F, 0.0F, 0.0F, 2.0F, 0.0F,
                                         0.0F, 0.0F, 3.0F, 0.0F};
  const std::array<std::uint32_t, 3> indices_{0, 1, 2};
  const std::array<tekla::db1::MeshView, 1> meshes_{
      tekla::db1::MeshView{42, positions_, indices_}};
  const std::array<tekla::db1::Vector3d, 3> points_{
      tekla::db1::Vector3d{0.0, 0.0, 0.0}, tekla::db1::Vector3d{0.0, 1.0, 0.0},
      tekla::db1::Vector3d{1.0, 1.0, 0.0}};
  const std::array<tekla::db1::CurveView, 1> curves_{
      tekla::db1::CurveView{84, tekla::db1::CurveGeometryKind::polyline, points_, 0.25}};
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  DeterministicReader reader;
  std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
  auto report = tekla::db1::gltf::write_glb(reader, output);
  return report && report.value().mesh_count == 1 && report.value().curve_count == 1 && output ? 0
                                                                                              : 1;
}
