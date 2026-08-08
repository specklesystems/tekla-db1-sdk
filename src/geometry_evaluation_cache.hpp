#pragma once

#include <array>
#include <functional>
#include <memory>

#include "occt/occt.hpp"

namespace tekla::db1::detail {

struct GeometryEvaluationPlacement {
  std::array<double, 3> origin{};
  std::array<double, 3> x_axis{1.0, 0.0, 0.0};
  std::array<double, 3> y_axis{0.0, 1.0, 0.0};
  std::array<double, 3> z_axis{0.0, 0.0, 1.0};
};

// Owns completed topology evaluations independently of model traversal. The
// cache canonicalizes validated rigid placement, preserves result ownership,
// and applies the bounded-memory and diagnostic policy for this expensive
// evaluator boundary. Callers without an authoritative frame retain the
// translation-only path.
class GeometryEvaluationCache {
 public:
  using Evaluator = std::function<Result<OcctMesh>(const OcctRequest&)>;

  GeometryEvaluationCache();
  ~GeometryEvaluationCache();
  GeometryEvaluationCache(GeometryEvaluationCache&&) noexcept;
  GeometryEvaluationCache& operator=(GeometryEvaluationCache&&) noexcept;

  GeometryEvaluationCache(const GeometryEvaluationCache&) = delete;
  GeometryEvaluationCache& operator=(const GeometryEvaluationCache&) = delete;

  [[nodiscard]] Result<OcctMesh> evaluate(const OcctRequest& request, const Evaluator& evaluator);
  [[nodiscard]] Result<OcctMesh> evaluate(const OcctRequest& request,
                                          const GeometryEvaluationPlacement& placement,
                                          const Evaluator& evaluator);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tekla::db1::detail
