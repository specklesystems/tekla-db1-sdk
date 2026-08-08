#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "geometry.hpp"
#include "nonpart_geometry.hpp"

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                               \
    }                                                                           \
  } while (false)

bool near(double lhs, double rhs) { return std::abs(lhs - rhs) < 1.0e-9; }

bool near(tekla::db1::Vector3d value, std::array<double, 3> expected) {
  return near(value.x, expected[0]) && near(value.y, expected[1]) && near(value.z, expected[2]);
}

}  // namespace

int main() {
  using namespace tekla::db1;
  using namespace tekla::db1::detail;

  CHECK(normalized_plate_dimensions(10.0, 600.0) == (std::array<double, 2>{600.0, 10.0}));
  CHECK(normalized_plate_dimensions(600.0, 10.0) == (std::array<double, 2>{600.0, 10.0}));

  const auto fastener = known_fastener_dimensions("7990", 20.0);
  CHECK(fastener && near(fastener->across_flats, 30.0) && near(fastener->height, 13.0));
  CHECK(!known_fastener_dimensions("UNKNOWN", 20.0));

  constexpr auto maximum_vertex = std::numeric_limits<std::uint32_t>::max();
  CHECK(checked_indexed_mesh_append_base(12U, 9U) == 4U);
  CHECK(checked_indexed_mesh_append_base(static_cast<std::size_t>(maximum_vertex - 1U) * 3U, 3U) ==
        maximum_vertex - 1U);
  CHECK(!checked_indexed_mesh_append_base(static_cast<std::size_t>(maximum_vertex - 1U) * 3U, 9U));
  CHECK(!checked_indexed_mesh_append_base(4U, 3U));

  const std::array<double, 1> three_bars{3.0};
  auto tapered = evaluate_tapered_straight_group_centerlines(
      std::array<Vector3d, 2>{Vector3d{0.0, 0.0, 0.0}, Vector3d{10.0, 0.0, 0.0}},
      std::array<Vector3d, 2>{Vector3d{0.0, 20.0, 0.0}, Vector3d{20.0, 20.0, 0.0}}, {0.0, 0.0, 0.0},
      {0.0, 20.0, 0.0}, 0.0, 0.0, 2U, three_bars, 1U, 16U);
  CHECK(tapered && tapered.value().size() == 3U);
  if (tapered && tapered.value().size() == 3U) {
    CHECK(near(tapered.value()[1][0], {0.0, 10.0, 0.0}));
    CHECK(near(tapered.value()[1][1], {15.0, 10.0, 0.0}));
  }

  const std::array<Vector3d, 3> nonplanar_polygon{Vector3d{0.0, 0.0, 0.0}, Vector3d{10.0, 0.0, 0.0},
                                                  Vector3d{10.0, 0.0, 10.0}};
  const std::array<double, 1> two_bars{2.0};
  auto nonplanar = evaluate_nonplanar_group_reference_centerlines(
      nonplanar_polygon, {0.0, 0.0, 0.0}, {0.0, 20.0, 0.0}, 2.0, 0.0, 0.0, 0.0, 0.0, 2U, two_bars,
      1U, true, 16U);
  CHECK(nonplanar && nonplanar.value().size() == 2U);
  if (nonplanar && nonplanar.value().size() == 2U) {
    CHECK(near(nonplanar.value()[0][0], {-10.0, 0.0, 2.0}));
    CHECK(near(nonplanar.value()[0][1], {-2.0, 0.0, 0.0}));
    CHECK(near(nonplanar.value()[0][2], {0.0, 0.0, 10.0}));
    CHECK(near(nonplanar.value()[1][0], {-10.0, 20.0, 2.0}));
  }

  return failures == 0 ? 0 : 1;
}
