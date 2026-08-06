#include <Standard_Failure.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <new>
#include <numbers>
#include <stdexcept>

#include "occt/occt.hpp"
#include "occt/protocol.hpp"

namespace {

int failures = 0;

#define CHECK(condition, message)         \
  do {                                    \
    if (!(condition)) {                   \
      std::printf("FAIL: %s\n", message); \
      ++failures;                         \
    }                                     \
  } while (false)

std::array<float, 6> bounds(const tekla::db1::detail::OcctMesh& mesh) {
  std::array<float, 6> result{mesh.positions[0], mesh.positions[1], mesh.positions[2],
                              mesh.positions[0], mesh.positions[1], mesh.positions[2]};
  for (std::size_t index = 3; index < mesh.positions.size(); index += 3) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      result[axis] = std::min(result[axis], mesh.positions[index + axis]);
      result[axis + 3] = std::max(result[axis + 3], mesh.positions[index + axis]);
    }
  }
  return result;
}

double volume(const tekla::db1::detail::OcctMesh& mesh) {
  double signed_volume = 0.0;
  for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
    const auto position = [&](std::uint32_t vertex, std::size_t axis) {
      return static_cast<double>(mesh.positions[static_cast<std::size_t>(vertex) * 3U + axis]);
    };
    const auto a = mesh.indices[index];
    const auto b = mesh.indices[index + 1];
    const auto c = mesh.indices[index + 2];
    signed_volume +=
        position(a, 0) * (position(b, 1) * position(c, 2) - position(b, 2) * position(c, 1)) -
        position(a, 1) * (position(b, 0) * position(c, 2) - position(b, 2) * position(c, 0)) +
        position(a, 2) * (position(b, 0) * position(c, 1) - position(b, 1) * position(c, 0));
  }
  return std::abs(signed_volume / 6.0);
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8U) {
    bytes[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

tekla::db1::detail::OcctTriangleMesh cube(double x0, double y0, double z0, double x1, double y1,
                                          double z1) {
  return {.positions = {x0, y0, z0, x1, y0, z0, x1, y1, z0, x0, y1, z0,
                        x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1},
          .indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                      1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7}};
}

tekla::db1::detail::OcctExtrusion hollow_prism() {
  using tekla::db1::detail::OcctLoop;
  return {
      .loops =
          {
              OcctLoop{.positions = {0, 0, 0, 10, 0, 0, 10, 10, 0, 0, 10, 0}},
              OcctLoop{.positions = {3, 3, 0, 7, 3, 0, 7, 7, 0, 3, 7, 0}},
          },
      .vector_x = 0,
      .vector_y = 0,
      .vector_z = 10,
  };
}

tekla::db1::detail::OcctRuledSweep bent_square_sweep() {
  using tekla::db1::detail::OcctLoop;
  return {
      .loops = {{.sections =
                     {
                         OcctLoop{.positions = {0, 0, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0}},
                         OcctLoop{.positions = {4, 0, 4, 8, 0, 4, 8, 4, 4, 4, 4, 4}},
                         OcctLoop{.positions = {8, 0, 8, 12, 0, 8, 12, 4, 8, 8, 4, 8}},
                     }}},
  };
}

tekla::db1::detail::OcctRuledSweep hollow_square_sweep() {
  using tekla::db1::detail::OcctLoop;
  return {
      .loops =
          {
              {.sections =
                   {
                       OcctLoop{.positions = {0, 0, 0, 10, 0, 0, 10, 10, 0, 0, 10, 0}},
                       OcctLoop{.positions = {0, 0, 10, 10, 0, 10, 10, 10, 10, 0, 10, 10}},
                   }},
              {.sections =
                   {
                       OcctLoop{.positions = {3, 3, 0, 7, 3, 0, 7, 7, 0, 3, 7, 0}},
                       OcctLoop{.positions = {3, 3, 10, 7, 3, 10, 7, 7, 10, 3, 7, 10}},
                   }},
          },
  };
}

tekla::db1::detail::OcctRuledSweep circular_hollow_sweep() {
  using tekla::db1::detail::OcctLoop;
  constexpr double diagonal = 7.0710678118654755;
  return {
      .loops =
          {
              {.sections =
                   {
                       OcctLoop{.positions = {9, 0, -1, 11, 0, -1, 11, 0, 1, 9, 0, 1}},
                       OcctLoop{
                           .positions =
                               {diagonal - 0.7071067811865476, diagonal - 0.7071067811865476, -1,
                                diagonal + 0.7071067811865476, diagonal + 0.7071067811865476, -1,
                                diagonal + 0.7071067811865476, diagonal + 0.7071067811865476, 1,
                                diagonal - 0.7071067811865476, diagonal - 0.7071067811865476, 1}},
                       OcctLoop{.positions = {0, 9, -1, 0, 11, -1, 0, 11, 1, 0, 9, 1}},
                   }},
              {.sections =
                   {
                       OcctLoop{
                           .positions = {9.5, 0, -0.5, 10.5, 0, -0.5, 10.5, 0, 0.5, 9.5, 0, 0.5}},
                       OcctLoop{
                           .positions =
                               {diagonal - 0.3535533905932738, diagonal - 0.3535533905932738, -0.5,
                                diagonal + 0.3535533905932738, diagonal + 0.3535533905932738, -0.5,
                                diagonal + 0.3535533905932738, diagonal + 0.3535533905932738, 0.5,
                                diagonal - 0.3535533905932738, diagonal - 0.3535533905932738, 0.5}},
                       OcctLoop{
                           .positions = {0, 9.5, -0.5, 0, 10.5, -0.5, 0, 10.5, 0.5, 0, 9.5, 0.5}},
                   }},
          },
      .circular_spine = true,
  };
}

tekla::db1::Result<tekla::db1::detail::OcctMesh> raise_occt_failure(
    const tekla::db1::detail::OcctRequest&) {
  Standard_Failure::Raise("synthetic StdFail_NotDone");
  return tekla::db1::Result<tekla::db1::detail::OcctMesh>::failure({});
}

tekla::db1::Result<tekla::db1::detail::OcctMesh> raise_standard_failure(
    const tekla::db1::detail::OcctRequest&) {
  throw std::runtime_error("synthetic standard failure");
}

tekla::db1::Result<tekla::db1::detail::OcctMesh> raise_allocation_failure(
    const tekla::db1::detail::OcctRequest&) {
  throw std::bad_alloc();
}

tekla::db1::Result<tekla::db1::detail::OcctMesh> raise_unknown_failure(
    const tekla::db1::detail::OcctRequest&) {
  throw 42;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace tekla::db1::detail;
  CHECK(argc == 4, "normal, hanging, and crashing worker paths are supplied by CTest");
  if (argc != 4) return 2;

  OcctRequest request;
  request.object_id = 42;
  request.base = {0, 0, 0, 10, 20, 30};
  request.subtract.push_back({2, 3, -1, 4, 5, 32});

  const auto occt_failure = invoke_occt_guarded(request, &raise_occt_failure);
  CHECK(!occt_failure.has_value() &&
            occt_failure.error().code == tekla::db1::ErrorCode::invalid_topology &&
            occt_failure.error().message.find("synthetic StdFail_NotDone") != std::string::npos,
        "an OCCT Standard_Failure becomes an object-scoped topology error");
  const auto standard_failure = invoke_occt_guarded(request, &raise_standard_failure);
  CHECK(!standard_failure.has_value() &&
            standard_failure.error().code == tekla::db1::ErrorCode::invalid_topology,
        "a standard evaluator exception becomes an object-scoped topology error");
  const auto allocation_failure = invoke_occt_guarded(request, &raise_allocation_failure);
  CHECK(!allocation_failure.has_value() &&
            allocation_failure.error().code == tekla::db1::ErrorCode::resource_limit,
        "topology allocation exhaustion remains a resource-limit error");
  const auto unknown_failure = invoke_occt_guarded(request, &raise_unknown_failure);
  CHECK(!unknown_failure.has_value() &&
            unknown_failure.error().code == tekla::db1::ErrorCode::invalid_topology,
        "an unknown evaluator exception remains contained");

  DirectOcctHost direct_host;
  auto direct = direct_host.evaluate(request);
  CHECK(direct.has_value(), "the direct host evaluates a Boolean work item");
  if (direct) {
    constexpr std::array<float, 6> expected_bounds{0, 0, 0, 10, 20, 30};
    CHECK(!direct.value().positions.empty() && direct.value().indices.size() % 3U == 0,
          "the OCCT evaluator returns a triangular display mesh");
    CHECK(bounds(direct.value()) == expected_bounds,
          "the cut shape retains the expected outer bounds");
    CHECK(direct.value().has_exact_metrics && direct.value().exact_volume > 0.0 &&
              direct.value().exact_surface_area > 0.0,
          "the evaluator retains exact topology metrics before tessellation");
  }

  SupervisedOcctHost supervised_host(std::filesystem::path(argv[1]), 5000);
  auto supervised = supervised_host.evaluate(request);
  CHECK(supervised.has_value(), "the supervised host evaluates through a worker");
  if (direct && supervised) {
    CHECK(supervised.value().object_id == direct.value().object_id &&
              supervised.value().positions == direct.value().positions &&
              supervised.value().indices == direct.value().indices &&
              supervised.value().has_exact_metrics == direct.value().has_exact_metrics &&
              supervised.value().exact_volume == direct.value().exact_volume &&
              supervised.value().exact_surface_area == direct.value().exact_surface_area,
          "direct and supervised hosts return the one evaluator's exact result");
  }
  auto second = supervised_host.evaluate(request);
  CHECK(second.has_value(), "the supervised worker remains warm across work items");

  OcctRequest mesh_request;
  mesh_request.object_id = 43;
  mesh_request.base_mesh = cube(0, 0, 0, 10, 20, 30);
  mesh_request.subtract_meshes.push_back(cube(2, 3, -1, 6, 8, 31));
  mesh_request.keep_half_spaces.push_back({5, 0, 0, 1, 0, 0});
  auto mesh_direct = direct_host.evaluate(mesh_request);
  CHECK(mesh_direct.has_value(), "a sewn triangle mesh accepts half-space and mesh-cut operations");
  if (mesh_direct) {
    constexpr std::array<float, 6> expected_bounds{5, 0, 0, 10, 20, 30};
    const auto actual_bounds = bounds(mesh_direct.value());
    CHECK(std::equal(actual_bounds.begin(), actual_bounds.end(), expected_bounds.begin(),
                     [](float lhs, float rhs) { return std::abs(lhs - rhs) < 1.0e-4F; }),
          "the mesh operation graph retains the requested half-space");
  }
  auto mesh_supervised = supervised_host.evaluate(mesh_request);
  CHECK(mesh_supervised.has_value(),
        "the variable mesh operation graph crosses the worker protocol");
  if (mesh_direct && mesh_supervised) {
    CHECK(mesh_supervised.value().positions == mesh_direct.value().positions &&
              mesh_supervised.value().indices == mesh_direct.value().indices,
          "direct and supervised mesh evaluations are identical");
  }

  OcctRequest recipe_request;
  recipe_request.object_id = 431;
  recipe_request.nodes = {{.object_id = 431, .base_extrusion = hollow_prism()}};
  auto recipe_direct = direct_host.evaluate(recipe_request);
  CHECK(recipe_direct.has_value(), "an analytic hollow extrusion evaluates without sewing");
  if (recipe_direct) {
    CHECK(std::abs(volume(recipe_direct.value()) - 840.0) < 1.0e-3,
          "an analytic extrusion preserves its inner loop");
    CHECK(recipe_direct.value().has_exact_metrics &&
              std::abs(recipe_direct.value().exact_volume - 840.0) < 1.0e-9,
          "exact volume is measured from the retained analytic solid");
  }
  auto recipe_supervised = supervised_host.evaluate(recipe_request);
  CHECK(recipe_supervised.has_value(),
        "an analytic extrusion crosses the supervised worker protocol");
  if (recipe_direct && recipe_supervised) {
    CHECK(recipe_supervised.value().positions == recipe_direct.value().positions &&
              recipe_supervised.value().indices == recipe_direct.value().indices &&
              recipe_supervised.value().exact_volume == recipe_direct.value().exact_volume,
          "direct and supervised analytic extrusion results are identical");
  }

  auto encoded_recipe = tekla::db1::detail::encode_occt_request(recipe_request);
  CHECK(encoded_recipe.has_value(), "an analytic extrusion request encodes");
  if (encoded_recipe) {
    // A hostile worker message must be rejected before reserving memory from
    // an untrusted count. Node zero starts immediately after the 112-byte
    // request header and its loop count is at byte 76 of the node header.
    write_u32(encoded_recipe.value(), 112U + 76U, 4'097U);
    auto hostile = tekla::db1::detail::decode_occt_request(encoded_recipe.value());
    CHECK(!hostile.has_value() && hostile.error().code == tekla::db1::ErrorCode::resource_limit,
          "an excessive extrusion-loop count fails before allocation");
  }

  OcctRequest non_planar_recipe = recipe_request;
  non_planar_recipe.nodes.front().base_extrusion.loops.front().positions[2] = 1.0;
  auto non_planar = direct_host.evaluate(non_planar_recipe);
  CHECK(
      !non_planar.has_value() && non_planar.error().code == tekla::db1::ErrorCode::invalid_argument,
      "a non-planar extrusion recipe fails before entering OCCT");

  OcctRequest parallel_recipe = recipe_request;
  parallel_recipe.nodes.front().base_extrusion.vector_x = 10.0;
  parallel_recipe.nodes.front().base_extrusion.vector_z = 0.0;
  auto parallel = direct_host.evaluate(parallel_recipe);
  CHECK(!parallel.has_value() && parallel.error().code == tekla::db1::ErrorCode::invalid_argument,
        "an extrusion vector parallel to its profile fails before entering OCCT");

  OcctRequest sweep_request;
  sweep_request.object_id = 432;
  sweep_request.nodes = {{.object_id = 432, .base_ruled_sweep = bent_square_sweep()}};
  auto sweep_direct = direct_host.evaluate(sweep_request);
  CHECK(sweep_direct.has_value(), "an analytic multi-station sweep evaluates without sewing");
  if (sweep_direct) {
    constexpr std::array<float, 6> expected_bounds{0, 0, 0, 12, 4, 8};
    CHECK(bounds(sweep_direct.value()) == expected_bounds,
          "an analytic multi-station sweep preserves every retained section");
  }
  auto sweep_supervised = supervised_host.evaluate(sweep_request);
  CHECK(sweep_supervised.has_value(),
        "an analytic multi-station sweep crosses the supervised worker protocol");
  if (sweep_direct && sweep_supervised) {
    CHECK(sweep_supervised.value().positions == sweep_direct.value().positions &&
              sweep_supervised.value().indices == sweep_direct.value().indices,
          "direct and supervised analytic sweep results are identical");
  }

  OcctRequest duplicate_station_request = sweep_request;
  duplicate_station_request.nodes.front().base_ruled_sweep.loops.front().sections[1U] =
      duplicate_station_request.nodes.front().base_ruled_sweep.loops.front().sections.front();
  auto duplicate_station = direct_host.evaluate(duplicate_station_request);
  CHECK(!duplicate_station.has_value() &&
            duplicate_station.error().code == tekla::db1::ErrorCode::invalid_argument,
        "a sweep with duplicate stations fails before entering OCCT");

  OcctRequest non_planar_sweep_request = sweep_request;
  non_planar_sweep_request.nodes.front()
      .base_ruled_sweep.loops.front()
      .sections[1U]
      .positions[2U] += 1.0;
  auto non_planar_sweep = direct_host.evaluate(non_planar_sweep_request);
  CHECK(!non_planar_sweep.has_value() &&
            non_planar_sweep.error().code == tekla::db1::ErrorCode::invalid_argument,
        "a non-planar sweep section fails before entering OCCT");

  OcctRequest hollow_sweep_request;
  hollow_sweep_request.object_id = 433;
  hollow_sweep_request.nodes = {{.object_id = 433, .base_ruled_sweep = hollow_square_sweep()}};
  auto hollow_sweep = direct_host.evaluate(hollow_sweep_request);
  CHECK(hollow_sweep.has_value(), "an analytic hollow sweep evaluates without sewing");
  if (hollow_sweep) {
    CHECK(std::abs(volume(hollow_sweep.value()) - 840.0) < 1.0e-3,
          "an analytic sweep preserves its inner loop");
  }

  auto encoded_sweep = tekla::db1::detail::encode_occt_request(sweep_request);
  CHECK(encoded_sweep.has_value(), "an analytic sweep request encodes");
  if (encoded_sweep) {
    // Node zero starts at byte 112; its sweep-loop count is at byte 104 of
    // the version-five node header.
    write_u32(encoded_sweep.value(), 112U + 104U, 4'097U);
    auto hostile = tekla::db1::detail::decode_occt_request(encoded_sweep.value());
    CHECK(!hostile.has_value() && hostile.error().code == tekla::db1::ErrorCode::resource_limit,
          "an excessive sweep-loop count fails before allocation");
  }

  OcctRequest short_circular_sweep = hollow_sweep_request;
  short_circular_sweep.nodes.front().base_ruled_sweep.circular_spine = true;
  auto invalid_circular = direct_host.evaluate(short_circular_sweep);
  CHECK(!invalid_circular.has_value() &&
            invalid_circular.error().code == tekla::db1::ErrorCode::invalid_argument,
        "a circular sweep with fewer than three stations fails before entering OCCT");

  OcctRequest circular_sweep_request;
  circular_sweep_request.object_id = 434;
  circular_sweep_request.nodes = {{.object_id = 434, .base_ruled_sweep = circular_hollow_sweep()}};
  auto circular_sweep = direct_host.evaluate(circular_sweep_request);
  CHECK(circular_sweep.has_value(), "an analytic hollow circular sweep evaluates without sewing");
  if (circular_sweep) {
    CHECK(std::abs(volume(circular_sweep.value()) - 15.0 * std::numbers::pi) < 0.5,
          "a hollow circular sweep is one revolved profile with its void preserved");
  }
  auto supervised_circular_sweep = supervised_host.evaluate(circular_sweep_request);
  CHECK(supervised_circular_sweep.has_value(),
        "an analytic hollow circular sweep crosses the supervised worker protocol");
  if (circular_sweep && supervised_circular_sweep) {
    CHECK(supervised_circular_sweep.value().positions == circular_sweep.value().positions &&
              supervised_circular_sweep.value().indices == circular_sweep.value().indices,
          "direct and supervised circular sweeps are identical");
  }

  OcctRequest non_rigid_circular_request = circular_sweep_request;
  for (auto& loop : non_rigid_circular_request.nodes.front().base_ruled_sweep.loops) {
    auto& middle = loop.sections[1U];
    for (std::size_t coordinate = 2U; coordinate < middle.positions.size(); coordinate += 3U) {
      middle.positions[coordinate] += 2.0;
    }
  }
  auto non_rigid_circular = direct_host.evaluate(non_rigid_circular_request);
  CHECK(non_rigid_circular.has_value(),
        "a non-rigid circular hint falls back to the retained ruled sections");
  if (non_rigid_circular) {
    CHECK(bounds(non_rigid_circular.value())[5] > 2.9F,
          "a non-rigid circular hint never discards intermediate section geometry");
  }

  // The child cutter contains its own subtraction. This is the topology shape
  // that motivated the flat CSG protocol: the child must remain a BRep until
  // its subtraction from the visible root, rather than being tessellated and
  // sewn between the two Boolean operations.
  OcctRequest graph_request;
  graph_request.object_id = 44;
  graph_request.nodes = {
      {.object_id = 44, .base_mesh = cube(0, 0, 0, 10, 10, 10), .subtract_nodes = {1}},
      {.object_id = 45, .base_mesh = cube(2, 2, -1, 8, 8, 11), .subtract_nodes = {2}},
      {.object_id = 46, .base_mesh = cube(4, 1, -2, 6, 9, 12)},
  };
  OcctRequest translated_graph = graph_request;
  translated_graph.object_id = 144;
  for (auto& node : translated_graph.nodes) {
    node.object_id += 100;
    for (std::size_t index = 0U; index + 2U < node.base_mesh.positions.size(); index += 3U) {
      node.base_mesh.positions[index] += 1234.0;
      node.base_mesh.positions[index + 1U] -= 5678.0;
      node.base_mesh.positions[index + 2U] += 90.0;
    }
  }
  auto graph_key = encode_translation_normalized_occt_request(graph_request);
  auto translated_graph_key = encode_translation_normalized_occt_request(translated_graph);
  CHECK(graph_key.has_value() && translated_graph_key.has_value(),
        "translated topology requests produce cache keys");
  if (graph_key && translated_graph_key) {
    CHECK(graph_key.value().bytes == translated_graph_key.value().bytes,
          "translated topology requests share a canonical cache key");
    CHECK(graph_key.value().origin != translated_graph_key.value().origin,
          "canonical topology keys retain the request placement separately");
  }
  translated_graph.nodes.front().base_mesh.positions[3] += 1.0e-4;
  auto changed_graph_key = encode_translation_normalized_occt_request(translated_graph);
  CHECK(changed_graph_key.has_value() && graph_key.has_value() &&
            changed_graph_key.value().bytes != graph_key.value().bytes,
        "a material topology change never aliases a translated cache key");
  auto graph_direct = direct_host.evaluate(graph_request);
  CHECK(graph_direct.has_value(), "a nested CSG graph evaluates without mesh round-trips");
  if (graph_direct) {
    CHECK(std::abs(volume(graph_direct.value()) - 760.0) < 1.0e-3,
          "a grandchild subtraction remains material in the visible root");
  }
  auto graph_supervised = supervised_host.evaluate(graph_request);
  CHECK(graph_supervised.has_value(), "a nested CSG graph crosses the worker protocol");
  if (graph_direct && graph_supervised) {
    CHECK(graph_supervised.value().positions == graph_direct.value().positions &&
              graph_supervised.value().indices == graph_direct.value().indices,
          "direct and supervised nested CSG results are identical");
  }

  OcctRequest split_request;
  split_request.object_id = 47;
  split_request.nodes = {
      {.object_id = 47, .base_mesh = cube(0, 0, 0, 10, 10, 10), .subtract_nodes = {1}},
      {.object_id = 48, .base_mesh = cube(4, -1, -1, 6, 11, 11)},
  };
  auto split = direct_host.evaluate(split_request);
  CHECK(split.has_value(), "a Boolean operation may return multiple material bodies");
  if (split) {
    CHECK(std::abs(volume(split.value()) - 800.0) < 1.0e-3,
          "all valid material bodies survive final tessellation");
  }

  SupervisedOcctHost timeout_host(std::filesystem::path(argv[2]), 50);
  auto timed_out = timeout_host.evaluate(request);
  CHECK(!timed_out.has_value() && timed_out.error().code == tekla::db1::ErrorCode::geometry_timeout,
        "a stuck worker becomes a bounded object-scoped timeout");

  OcctRequest large_request = request;
  large_request.subtract.resize(100000, OcctBox{1, 1, 1, 1, 1, 1});
  SupervisedOcctHost blocked_write_host(std::filesystem::path(argv[2]), 50);
  auto blocked_write = blocked_write_host.evaluate(large_request);
  CHECK(!blocked_write.has_value() &&
            blocked_write.error().code == tekla::db1::ErrorCode::geometry_timeout,
        "a large request cannot evade the whole-transaction deadline");

  SupervisedOcctHost crash_host(std::filesystem::path(argv[3]), 1000);
  auto crashed = crash_host.evaluate(request);
  CHECK(
      !crashed.has_value() && crashed.error().code == tekla::db1::ErrorCode::geometry_backend_crash,
      "a terminated worker becomes a recoverable backend-crash result");

  OcctRequest invalid;
  invalid.base = {0, 0, 0, -1, 2, 3};
  auto invalid_result = direct_host.evaluate(invalid);
  CHECK(!invalid_result.has_value() &&
            invalid_result.error().code == tekla::db1::ErrorCode::invalid_argument,
        "invalid topology inputs fail before entering OCCT");

  if (failures != 0) {
    std::printf("FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("ALL PASS\n");
  return 0;
}
