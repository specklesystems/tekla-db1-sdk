#pragma once

#include <array>
#include <cstdint>
#include <tekla/db1/process.hpp>
#include <vector>

namespace tekla::db1::detail {

// Output-neutral semantic geometry retained through feature planning. The
// first loop is the material boundary; following loops are voids. Tessellation
// and kernel adapters consume this recipe but do not own it.
struct RecipeLoop {
  std::vector<Vector3d> points;
};

struct ExtrusionRecipe {
  std::uint64_t object_id = 0;
  std::vector<RecipeLoop> loops;
  Vector3d vector;
};

struct SweepSectionLoopRecipe {
  std::vector<std::array<double, 2>> points;
};

struct SweepStationRecipe {
  Vector3d origin;
  Vector3d y_axis;
  Vector3d z_axis;
};

struct RuledSweepRecipe {
  std::uint64_t object_id = 0;
  std::vector<SweepSectionLoopRecipe> loops;
  std::vector<SweepStationRecipe> stations;
  bool circular_spine = false;
};

enum class EvaluationTier : std::uint8_t {
  direct_recipe,
  analytic_brep,
  guarded_mesh_fallback,
};

struct FeaturePlanSummary {
  std::uint64_t object_id = 0;
  bool has_semantic_recipe = false;
  bool has_features = false;
};

[[nodiscard]] constexpr EvaluationTier select_evaluation_tier(
    const FeaturePlanSummary& plan) noexcept {
  if (!plan.has_features) return EvaluationTier::direct_recipe;
  return plan.has_semantic_recipe ? EvaluationTier::analytic_brep
                                  : EvaluationTier::guarded_mesh_fallback;
}

}  // namespace tekla::db1::detail
