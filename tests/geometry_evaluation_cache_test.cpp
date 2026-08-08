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
