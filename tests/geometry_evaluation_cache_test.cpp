#include "geometry_evaluation_cache.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

#define CHECK(condition, message)         \
  do {                                    \
    if (!(condition)) {                   \
      std::printf("FAIL: %s\n", message); \
      ++failures;                         \
    }                                     \
  } while (false)

tekla::db1::detail::OcctRequest translated_box(std::uint64_t object_id, double x, double y,
                                               double z) {
  using tekla::db1::detail::OcctRequest;
  return OcctRequest{.object_id = object_id,
                     .base = {.x = x, .y = y, .z = z, .size_x = 2.0, .size_y = 3.0, .size_z = 4.0}};
}

bool near(float actual, double expected) {
  return std::abs(static_cast<double>(actual) - expected) < 1.0e-5;
}

tekla::db1::detail::GeometryEvaluationPlacement placement(std::array<double, 3> origin,
                                                          std::array<double, 3> x_axis,
                                                          std::array<double, 3> y_axis,
                                                          std::array<double, 3> z_axis) {
  return {.origin = origin, .x_axis = x_axis, .y_axis = y_axis, .z_axis = z_axis};
}

std::array<double, 3> placed_point(const tekla::db1::detail::GeometryEvaluationPlacement& frame,
                                   std::array<double, 3> local) {
  std::array<double, 3> world = frame.origin;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    world[axis] += frame.x_axis[axis] * local[0] + frame.y_axis[axis] * local[1] +
                   frame.z_axis[axis] * local[2];
  }
  return world;
}

tekla::db1::detail::OcctRequest placed_feature_request(
    std::uint64_t object_id, const tekla::db1::detail::GeometryEvaluationPlacement& frame,
    double extrusion = 4.0) {
  using namespace tekla::db1::detail;
  const auto point = [&](std::array<double, 3> local) { return placed_point(frame, local); };
  const auto append = [](std::vector<double>& target, std::array<double, 3> value) {
    target.insert(target.end(), value.begin(), value.end());
  };
  OcctRequest request;
  request.object_id = object_id;
  request.nodes.resize(2U);
  request.nodes[0].object_id = object_id;
  auto& loop = request.nodes[0].base_extrusion.loops.emplace_back().positions;
  for (const auto local :
       {std::array<double, 3>{0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {2.0, 3.0, 0.0}, {0.0, 3.0, 0.0}}) {
    append(loop, point(local));
  }
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const double component = frame.z_axis[axis] * extrusion;
    if (axis == 0U) request.nodes[0].base_extrusion.vector_x = component;
    if (axis == 1U) request.nodes[0].base_extrusion.vector_y = component;
    if (axis == 2U) request.nodes[0].base_extrusion.vector_z = component;
  }
  const auto plane_origin = point({1.0, 0.0, 0.0});
  request.nodes[0].keep_half_spaces.push_back({.origin_x = plane_origin[0],
                                               .origin_y = plane_origin[1],
                                               .origin_z = plane_origin[2],
                                               .normal_x = frame.x_axis[0],
                                               .normal_y = frame.x_axis[1],
                                               .normal_z = frame.x_axis[2]});
  request.nodes[0].subtract_nodes.push_back(1U);
  request.nodes[1].object_id = object_id + 1U;
  for (const auto local :
       {std::array<double, 3>{0.5, 0.5, -1.0}, {1.5, 0.5, -1.0}, {0.5, 0.5, 5.0}}) {
    append(request.nodes[1].base_mesh.positions, point(local));
  }
  request.nodes[1].base_mesh.indices = {0U, 1U, 2U};
  return request;
}

void set_cache_disabled(bool disabled) {
#if defined(_WIN32)
  _putenv_s("TEKLA_DB1_DISABLE_OCCT_CACHE", disabled ? "1" : "");
#else
  if (disabled) {
    setenv("TEKLA_DB1_DISABLE_OCCT_CACHE", "1", 1);
  } else {
    unsetenv("TEKLA_DB1_DISABLE_OCCT_CACHE");
  }
#endif
}

}  // namespace

int main() {
  using namespace tekla::db1;
  using namespace tekla::db1::detail;

  GeometryEvaluationCache cache;
  std::size_t evaluations = 0U;
  const auto evaluator = [&](const OcctRequest& request) {
    ++evaluations;
    return Result<OcctMesh>::success(
        {.object_id = request.object_id,
         .positions = {static_cast<float>(request.base.x), static_cast<float>(request.base.y),
                       static_cast<float>(request.base.z),
                       static_cast<float>(request.base.x + request.base.size_x),
                       static_cast<float>(request.base.y + request.base.size_y),
                       static_cast<float>(request.base.z + request.base.size_z)},
         .indices = {0U, 1U, 1U},
         .exact_surface_area = 52.0,
         .exact_volume = 24.0,
         .has_exact_metrics = true});
  };

  const auto original = cache.evaluate(translated_box(41U, 10.0, 20.0, 30.0), evaluator);
  CHECK(original.has_value(), "the first topology evaluation succeeds");
  const auto translated = cache.evaluate(translated_box(99U, 1010.0, -1980.0, 530.0), evaluator);
  CHECK(translated.has_value(), "a translated topology evaluation succeeds from the cache");
  CHECK(evaluations == 1U, "translated requests share one completed evaluation");
  if (translated) {
    CHECK(translated.value().object_id == 99U, "a cache hit adopts the current owner identity");
    CHECK(translated.value().positions.size() == 6U &&
              near(translated.value().positions[0], 1010.0) &&
              near(translated.value().positions[1], -1980.0) &&
              near(translated.value().positions[2], 530.0) &&
              near(translated.value().positions[3], 1012.0) &&
              near(translated.value().positions[4], -1977.0) &&
              near(translated.value().positions[5], 534.0),
          "a cache hit restores the current model-space translation");
    CHECK(translated.value().has_exact_metrics && translated.value().exact_surface_area == 52.0 &&
              translated.value().exact_volume == 24.0,
          "a cache hit preserves exact topology metrics");
  }

  auto changed = translated_box(100U, 1010.0, -1980.0, 530.0);
  changed.base.size_z = 5.0;
  const auto distinct = cache.evaluate(changed, evaluator);
  CHECK(distinct.has_value() && evaluations == 2U,
        "a topology change does not alias a translated completed result");

  const auto first_frame =
      placement({10.0, 20.0, 30.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0});
  const auto translated_frame =
      placement({1010.0, -1980.0, 530.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0});
  const auto rotated_frame =
      placement({100.0, -50.0, 20.0}, {0.0, 1.0, 0.0}, {-1.0, 0.0, 0.0}, {0.0, 0.0, 1.0});
  GeometryEvaluationCache rigid_cache;
  std::size_t rigid_evaluations = 0U;
  const auto rigid_evaluator = [&](const OcctRequest& request) {
    ++rigid_evaluations;
    const auto& source = request.nodes.front().base_extrusion.loops.front().positions;
    return Result<OcctMesh>::success(
        {.object_id = request.object_id,
         .positions = {static_cast<float>(source[0]), static_cast<float>(source[1]),
                       static_cast<float>(source[2]), static_cast<float>(source[3]),
                       static_cast<float>(source[4]), static_cast<float>(source[5])},
         .indices = {0U, 1U, 1U},
         .exact_surface_area = 52.0,
         .exact_volume = 24.0,
         .has_exact_metrics = true});
  };
  const auto rigid_first =
      rigid_cache.evaluate(placed_feature_request(401U, first_frame), first_frame, rigid_evaluator);
  const auto rigid_translated = rigid_cache.evaluate(placed_feature_request(451U, translated_frame),
                                                     translated_frame, rigid_evaluator);
  const auto rigid_rotated = rigid_cache.evaluate(placed_feature_request(501U, rotated_frame),
                                                  rotated_frame, rigid_evaluator);
  CHECK(rigid_first.has_value() && rigid_translated.has_value() && rigid_rotated.has_value() &&
            rigid_evaluations == 1U,
        "equivalent topology features share a completed result across rigid placement");
  if (rigid_translated) {
    CHECK(rigid_translated.value().positions[0] == 1010.0F &&
              rigid_translated.value().positions[1] == -1980.0F &&
              rigid_translated.value().positions[2] == 530.0F &&
              rigid_translated.value().positions[3] == 1012.0F &&
              rigid_translated.value().positions[4] == -1980.0F &&
              rigid_translated.value().positions[5] == 530.0F,
          "an unchanged orientation retains the translation-only byte path");
  }
  if (rigid_rotated) {
    const auto expected_first = placed_point(rotated_frame, {0.0, 0.0, 0.0});
    const auto expected_second = placed_point(rotated_frame, {2.0, 0.0, 0.0});
    CHECK(rigid_rotated.value().object_id == 501U &&
              near(rigid_rotated.value().positions[0], expected_first[0]) &&
              near(rigid_rotated.value().positions[1], expected_first[1]) &&
              near(rigid_rotated.value().positions[2], expected_first[2]) &&
              near(rigid_rotated.value().positions[3], expected_second[0]) &&
              near(rigid_rotated.value().positions[4], expected_second[1]) &&
              near(rigid_rotated.value().positions[5], expected_second[2]),
          "a rigid cache hit reconstructs the current world-space output");
    CHECK(rigid_rotated.value().has_exact_metrics &&
              rigid_rotated.value().exact_surface_area == 52.0 &&
              rigid_rotated.value().exact_volume == 24.0,
          "a rigid cache hit preserves exact metrics");
  }
  const auto feature_miss = rigid_cache.evaluate(placed_feature_request(601U, rotated_frame, 5.0),
                                                 rotated_frame, rigid_evaluator);
  CHECK(feature_miss.has_value() && rigid_evaluations == 2U,
        "a materially different feature recipe misses the rigid cache");

  const auto invalid_frame =
      placement({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0});
  GeometryEvaluationCache invalid_frame_cache;
  std::size_t invalid_frame_evaluations = 0U;
  const auto invalid_evaluator = [&](const OcctRequest& request) {
    ++invalid_frame_evaluations;
    return Result<OcctMesh>::success({.object_id = request.object_id});
  };
  const auto invalid_first = invalid_frame_cache.evaluate(placed_feature_request(701U, first_frame),
                                                          invalid_frame, invalid_evaluator);
  const auto invalid_rotated = invalid_frame_cache.evaluate(
      placed_feature_request(801U, rotated_frame), invalid_frame, invalid_evaluator);
  CHECK(invalid_first.has_value() && invalid_rotated.has_value() && invalid_frame_evaluations == 2U,
        "an invalid rigid frame falls back without aliasing rotated topology");

  const auto reflected_frame =
      placement({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, -1.0});
  GeometryEvaluationCache reflected_frame_cache;
  std::size_t reflected_frame_evaluations = 0U;
  const auto reflected_evaluator = [&](const OcctRequest& request) {
    ++reflected_frame_evaluations;
    return Result<OcctMesh>::success({.object_id = request.object_id});
  };
  CHECK(reflected_frame_cache
                .evaluate(placed_feature_request(901U, first_frame), reflected_frame,
                          reflected_evaluator)
                .has_value() &&
            reflected_frame_cache
                .evaluate(placed_feature_request(902U, rotated_frame), reflected_frame,
                          reflected_evaluator)
                .has_value() &&
            reflected_frame_evaluations == 2U,
        "a reflected frame falls back without entering rigid canonicalization");

  set_cache_disabled(true);
  GeometryEvaluationCache disabled_cache;
  std::size_t disabled_evaluations = 0U;
  const auto disabled_evaluator = [&](const OcctRequest& request) {
    ++disabled_evaluations;
    return Result<OcctMesh>::success({.object_id = request.object_id});
  };
  const auto disabled_first =
      disabled_cache.evaluate(translated_box(201U, 0.0, 0.0, 0.0), disabled_evaluator);
  const auto disabled_second =
      disabled_cache.evaluate(translated_box(202U, 0.0, 0.0, 0.0), disabled_evaluator);
  set_cache_disabled(false);
  CHECK(disabled_first.has_value() && disabled_second.has_value() && disabled_evaluations == 2U,
        "the cache-disable switch bypasses completed-result lookup and insertion");

  GeometryEvaluationCache retry_cache;
  std::size_t retry_attempts = 0U;
  const auto retry_evaluator = [&](const OcctRequest& request) {
    ++retry_attempts;
    if (retry_attempts == 1U) {
      return Result<OcctMesh>::failure({ErrorCode::invalid_topology, "synthetic topology failure"});
    }
    return Result<OcctMesh>::success({.object_id = request.object_id});
  };
  const auto failed = retry_cache.evaluate(translated_box(301U, 0.0, 0.0, 0.0), retry_evaluator);
  const auto retried = retry_cache.evaluate(translated_box(301U, 0.0, 0.0, 0.0), retry_evaluator);
  CHECK(!failed.has_value() && retried.has_value() && retry_attempts == 2U,
        "failed evaluations remain retryable instead of poisoning the completed-result cache");

  GeometryEvaluationCache bounded_cache;
  std::size_t bounded_evaluations = 0U;
  const auto bounded_evaluator = [&](const OcctRequest& request) {
    ++bounded_evaluations;
    return Result<OcctMesh>::success({.object_id = request.object_id});
  };
  constexpr std::size_t maximum_entries = 4096U;
  for (std::size_t index = 0U; index < maximum_entries; ++index) {
    auto request = translated_box(1000U + index, 0.0, 0.0, 0.0);
    request.base.size_x = static_cast<double>(index + 1U);
    CHECK(bounded_cache.evaluate(request, bounded_evaluator).has_value(),
          "a completed result within the entry limit evaluates");
  }
  auto overflow = translated_box(9999U, 0.0, 0.0, 0.0);
  overflow.base.size_x = static_cast<double>(maximum_entries + 1U);
  CHECK(bounded_cache.evaluate(overflow, bounded_evaluator).has_value() &&
            bounded_cache.evaluate(overflow, bounded_evaluator).has_value() &&
            bounded_evaluations == maximum_entries + 2U,
        "the 4096-entry limit leaves later completed results uncached");

  return failures == 0 ? 0 : 1;
}
